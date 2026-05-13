#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "Zend/zend_extensions.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define PHP_PFX_VERSION "0.1.0"
#define PFX_MAX_STACK 1024

// Low byte: arg position (1..N). Reserve 3..0xFF for future args.
#define PFX_META_FIRST_ARG     1
#define PFX_META_SECOND_ARG    2
// 0x100 range: alternate modes that don't read an arg.
#define PFX_META_INHERIT       0x100 // Inherit metadata from caller
#define PFX_META_DEFINITION    0x101 // Use the filename:lineno as meta
#define PFX_META_CURL_URL      0x102 // Read CURLINFO_EFFECTIVE_URL from handle
// 0x200 range: transform flags, OR'd with an arg position.
#define PFX_META_NORMALIZE_SQL 0x200 // Normalize a SQL string before storing

static void pfx_stop(void);


// ============================================================================
// Types
//
// Frame identity is (fn, meta, prev). Cached display fields aren't part of
// identity. meta is NULL or a string borrowed from arg[0] of a tracked call.
// ============================================================================

typedef struct {
  zend_function* fn;
  zend_string* meta;
  uint64_t prev;
} pfx_key;

typedef struct {
  pfx_key key;                 // identity (24 bytes)
  zend_string* class_name;     // addref'd; NULL if not a method
  zend_string* function_name;  // addref'd; NULL for file-scope frames
  zend_string* filename;       // addref'd; NULL for internal funcs
  uint32_t line;
} pfx_frame;

// Serialized frame record. String fields are 1-based indices (0 = absent).
typedef struct {
  uint32_t prev;
  uint32_t line;
  uint32_t class;
  uint32_t function;
  uint32_t filename;
  uint32_t meta;
} pfx_out_frame;


// ============================================================================
// State
// ============================================================================

static pfx_frame* pfx_frames;
static uint32_t pfx_n_frames;
static uint32_t pfx_cap_frames;

static HashTable pfx_frame_dedup;    // pfx_key bytes -> frame index
static HashTable pfx_sample_counts;  // leaf frame idx -> u32* count

static uint64_t pfx_out_bytes;
static uint64_t pfx_max_bytes;

static pthread_t pfx_tick_thread;
static int pfx_timerfd = -1;
static _Atomic uint32_t pfx_ticks;
static _Atomic bool pfx_running;
static uint64_t pfx_period_ns;

static bool pfx_initialized;
static bool pfx_aborted;

static HashTable pfx_meta_fns;      // fn_name -> meta_arg (PFX_META_*)
static HashTable pfx_meta_methods;  // class_name -> (fn_name -> meta_arg)
static HashTable pfx_meta_strings;  // string -> string for new meta strings

static HashTable pfx_request_data;  // key -> value strings set via pfx_set()

static zend_atomic_bool* pfx_vm_interrupt_ptr;
static void (*pfx_prev_interrupt_fn)(zend_execute_data*);

static CURL* pfx_curl;
static struct curl_slist* pfx_curl_headers;
static char* pfx_secret;


// ============================================================================
// Meta capture
//
// Looks up ex->func in the user-registered tracked tables and returns the
// meta string.
// ============================================================================

// Get an existing or new zend_string from bytes.
static zend_string* pfx_meta_string(const char* buf, size_t len) {
  zend_string* s = zend_hash_str_find_ptr(&pfx_meta_strings, buf, len);
  if (s) return s;
  s = zend_string_init(buf, len, 1);
  zend_hash_add_new_ptr(&pfx_meta_strings, s, s);
  zend_string_release(s);
  pfx_out_bytes += sizeof(uint32_t) + len;  // len prefix + bytes
  return s;
}

// Collapse string and numeric literals to ?
static zend_string* normalize_sql(const char* s, size_t n) {
  char stackbuf[1024];
  char* out = (n <= sizeof(stackbuf)) ? stackbuf : pemalloc(n, 1);
  char* w = out;

  for (size_t i = 0; i < n; ) {
    unsigned char c = (unsigned char)s[i];

    if (c == '\'' || c == '"') {
      unsigned char q = c;
      i++;
      while (i < n) {
        if (s[i] == '\\' && i + 1 < n) { i += 2; continue; }
        if ((unsigned char)s[i] == q) {
          if (i + 1 < n && (unsigned char)s[i+1] == q) { i += 2; continue; }
          break;
        }
        i++;
      }
      if (i < n) i++;
      *w++ = '?';
      continue;
    }

    if (c >= '0' && c <= '9') {
      int boundary = (w == out);
      if (!boundary) {
        unsigned char prev = (unsigned char)w[-1];
        boundary = !((prev >= '0' && prev <= '9')
                  || (prev >= 'A' && prev <= 'Z')
                  || (prev >= 'a' && prev <= 'z')
                  || prev == '_');
      }
      if (boundary) {
        while (i < n) {
          unsigned char d = (unsigned char)s[i];
          if ((d >= '0' && d <= '9') || d == '.'
            || d == 'e' || d == 'E' || d == '+' || d == '-') { i++; }
          else break;
        }
        *w++ = '?';
        continue;
      }
    }

    *w++ = s[i++];
  }

  zend_string* z = pfx_meta_string(out, (size_t)(w - out));
  if (out != stackbuf) pefree(out, 1);
  return z;
}

// Extract metadata from ex
static zend_string* frame_meta(zend_execute_data* ex, zend_string* parent_meta) {
  if (!ex) {
    return NULL;
  }

  zend_function* fn = ex->func;
  if (!fn->common.function_name) {
    return NULL;
  }

  zend_class_entry* scope = fn->common.scope;
  void* p;
  if (scope) {
    HashTable* inner = zend_hash_find_ptr(&pfx_meta_methods, scope->name);
    if (!inner) {
      return NULL;
    }
    p = zend_hash_find_ptr(inner, fn->common.function_name);
  } else {
    p = zend_hash_find_ptr(&pfx_meta_fns, fn->common.function_name);
  }
  if (!p) {
    return NULL;
  }

  int meta_arg = (int)(intptr_t)p;
  if (meta_arg == PFX_META_INHERIT) {
    return parent_meta;
  }

  if (meta_arg == PFX_META_DEFINITION) {
    zend_string* fname;
    zend_string* def = NULL;
    size_t cap;
    char stackbuf[512];
    char* buf;
    int n;

    if (!ZEND_USER_CODE(fn->type) || !fn->op_array.filename) {
      return NULL;
    }

    fname = fn->op_array.filename;
    cap = ZSTR_LEN(fname) + 16;
    buf = (cap <= sizeof(stackbuf)) ? stackbuf : pemalloc(cap, 1);
    n = snprintf(buf, cap, "%s:%u", ZSTR_VAL(fname), fn->op_array.line_start);
    if (n > 0 && (size_t)n < cap) {
      def = pfx_meta_string(buf, n);
    }
    if (buf != stackbuf) pefree(buf, 1);
    return def;
  }

  // Use with curl_exec to extract effective URL from the handle.
  if (meta_arg == PFX_META_CURL_URL) {
    zval* handle;
    zval func_name, params[2], retval;
    zend_string* url = NULL;

    if (ZEND_CALL_NUM_ARGS(ex) < 1) return NULL;
    handle = ZEND_CALL_ARG(ex, 1);
    if (Z_TYPE_P(handle) != IS_OBJECT) return NULL;

    ZVAL_STRING(&func_name, "curl_getinfo");
    ZVAL_COPY_VALUE(&params[0], handle);
    ZVAL_LONG(&params[1], CURLINFO_EFFECTIVE_URL);
    ZVAL_UNDEF(&retval);
    call_user_function(NULL, NULL, &func_name, &retval, 2, params);

    if (Z_TYPE(retval) == IS_STRING && Z_STRLEN(retval) > 0) {
      url = pfx_meta_string(Z_STRVAL(retval), Z_STRLEN(retval));
    }

    zval_ptr_dtor(&func_name);
    zval_ptr_dtor(&retval);
    return url;
  }

  int argpos = meta_arg & 0xFF;
  int flags = meta_arg & ~0xFF;
  if (argpos == 0 || ZEND_CALL_NUM_ARGS(ex) < (uint32_t)argpos) {
    return NULL;
  }
  zval* arg = ZEND_CALL_ARG(ex, argpos);
  if (Z_TYPE_P(arg) != IS_STRING) {
    return NULL;
  }
  if (flags & PFX_META_NORMALIZE_SQL) {
    return normalize_sql(Z_STRVAL_P(arg), Z_STRLEN_P(arg));
  }
  if (ZSTR_IS_INTERNED(Z_STR_P(arg))) {
    return Z_STR_P(arg);
  }
  return pfx_meta_string(Z_STRVAL_P(arg), Z_STRLEN_P(arg));
}


// ============================================================================
// Frame table
//
// Frames live in pfx_frames[], linked parent-ward via prev to form a call
// tree. pfx_frame_dedup collapses repeat paths by (fn, meta, prev).
// ============================================================================

static uint32_t add_frame(zend_function* fn, zend_string* meta, uint32_t prev) {
  pfx_key key;
  key.fn = fn;
  key.meta = meta;
  key.prev = prev;

  uintptr_t existing = (uintptr_t)zend_hash_str_find_ptr(
      &pfx_frame_dedup, (const char*)&key, sizeof(key));
  if (existing) {
    return (uint32_t)existing;
  }

  if (pfx_n_frames + 1 >= pfx_cap_frames) {
    uint32_t new_cap = pfx_cap_frames ? pfx_cap_frames * 2 : 512;
    pfx_frame* new_frames = realloc(pfx_frames, new_cap * sizeof(pfx_frame));
    if (!new_frames) {
      return 0;
    }
    pfx_frames = new_frames;
    pfx_cap_frames = new_cap;
  }

  uint32_t new_idx = ++pfx_n_frames;
  pfx_frame* f = &pfx_frames[new_idx];
  f->key = key;
  f->class_name = NULL;
  f->function_name = NULL;
  f->filename = NULL;
  f->line = 0;

  if (fn) {
    if (fn->common.scope && fn->common.scope->name) {
      f->class_name = fn->common.scope->name;
      zend_string_addref(f->class_name);
    }
    if (fn->common.function_name) {
      f->function_name = fn->common.function_name;
      zend_string_addref(f->function_name);
    }
    if (ZEND_USER_CODE(fn->type)) {
      if (fn->op_array.filename) {
        f->filename = fn->op_array.filename;
        zend_string_addref(f->filename);
      }
      f->line = fn->op_array.line_start;
    }
  }

  if (meta) {
    zend_string_addref(meta);
  }

  zend_hash_str_add_ptr(
      &pfx_frame_dedup, (const char*)&key, sizeof(key),
      (void*)(uintptr_t)new_idx);

  pfx_out_bytes += sizeof(pfx_out_frame);
  return new_idx;
}

static uint32_t build_chain(zend_execute_data* ex, int depth) {
  if (!ex || depth >= PFX_MAX_STACK) {
    return 0;
  }
  uint32_t parent = build_chain(ex->prev_execute_data, depth + 1);
  zend_string* parent_meta = parent ? pfx_frames[parent].key.meta : NULL;
  zend_string* meta = frame_meta(ex, parent_meta);
  return add_frame(ex->func, meta, parent);
}

static void record_sample(uint32_t leaf, uint32_t count) {
  uint32_t* c = zend_hash_index_find_ptr(&pfx_sample_counts, (zend_ulong)leaf);
  if (!c) {
    c = pemalloc(sizeof(*c), 1);
    *c = 0;
    zend_hash_index_add_ptr(&pfx_sample_counts, (zend_ulong)leaf, c);
    pfx_out_bytes += sizeof(uint32_t) + sizeof(uint32_t);  // leaf + count
  }
  *c += count;
}


// ============================================================================
// Sampling plumbing
//
// Tick thread reads the timerfd, bumps pfx_ticks, flags vm_interrupt. PHP's
// main thread does the actual stack walk at the next opcode boundary.
// ============================================================================

static void* pfx_tick_loop(void* arg) {
  (void)arg;

  while (atomic_load(&pfx_running)) {
    uint64_t ticks = 0;
    if (read(pfx_timerfd, &ticks, sizeof(ticks)) != sizeof(ticks)) {
      continue;  // EINTR, short read, etc.
    }
    if (!atomic_load(&pfx_running)) {
      break;
    }

    atomic_fetch_add(&pfx_ticks, (uint32_t)ticks);
    if (pfx_vm_interrupt_ptr) {
      zend_atomic_bool_store(pfx_vm_interrupt_ptr, 1);
    }
  }

  return NULL;
}

static void pfx_interrupt_fn(zend_execute_data* ex) {
  if (atomic_load(&pfx_running)) {
    uint32_t n = atomic_exchange(&pfx_ticks, 0);
    if (n > 0 && ex) {
      uint32_t leaf = build_chain(ex, 0);
      if (leaf) {
        record_sample(leaf, n);
      }
      if (pfx_max_bytes && pfx_out_bytes > pfx_max_bytes) {
        pfx_stop();
        pfx_aborted = true;
        php_error_docref(NULL, E_WARNING,
          "pfx: profile exceeded pfx.max_size (%llu bytes); aborted",
          (unsigned long long)pfx_max_bytes);
      }
    }
  }

  if (pfx_prev_interrupt_fn) {
    pfx_prev_interrupt_fn(ex);
  }
}

// Install once per process; the handler is a passthrough when not profiling.
static void install_interrupt_fn(void) {
  static bool installed;
  if (installed) return;
  pfx_vm_interrupt_ptr = &EG(vm_interrupt);
  pfx_prev_interrupt_fn = zend_interrupt_function;
  zend_interrupt_function = pfx_interrupt_fn;
  installed = true;
}


// ============================================================================
// Hash dtors
// ============================================================================

static void pfx_count_dtor(zval* z) {
  pefree(Z_PTR_P(z), 1);
}

static void pfx_meta_methods_dtor(zval* z) {
  HashTable* ht = Z_PTR_P(z);
  zend_hash_destroy(ht);
  pefree(ht, 1);
}

static void pfx_request_data_dtor(zval* z) {
  zend_string* s = Z_PTR_P(z);
  if (s) zend_string_release(s);
}


// ============================================================================
// Request-scoped setup / teardown
// ============================================================================

static void pfx_init_state(void) {
  if (pfx_initialized) {
    return;
  }

  pfx_frames = NULL;
  pfx_n_frames = 0;
  pfx_cap_frames = 0;
  pfx_out_bytes = 0;

  zend_hash_init(&pfx_frame_dedup, 256, NULL, NULL, 1);
  zend_hash_init(&pfx_sample_counts, 256, NULL, pfx_count_dtor, 1);

  pfx_initialized = true;
}

static void pfx_stop(void) {
  if (!atomic_load(&pfx_running)) {
    return;
  }

  atomic_store(&pfx_running, false);

  // Force an immediate tick so the tick thread's blocking read() returns.
  struct itimerspec now = {
    .it_value    = { .tv_sec = 0, .tv_nsec = 1 },
    .it_interval = { 0, 0 },
  };
  timerfd_settime(pfx_timerfd, 0, &now, NULL);
  pthread_join(pfx_tick_thread, NULL);

  close(pfx_timerfd);
  pfx_timerfd = -1;
}

// Fork in the child is treated as an abort.
static void pfx_atfork_child(void) {
  atomic_store(&pfx_running, false);
  atomic_store(&pfx_ticks, 0);
  if (pfx_timerfd >= 0) {
    close(pfx_timerfd);
    pfx_timerfd = -1;
  }
  pfx_aborted = true;
}

// ============================================================================
// Binary output
//
//   magic        : 4 bytes "PFX0"
//   period_ns    : u64
//   uid          : u32
//   gid          : u32
//   n_req_data   : u32
//   req_data[]   : { u32 klen; bytes; u32 vlen; bytes }
//   n_strings    : u32
//   strings[]    : { u32 len; bytes }
//   n_frames     : u32
//   frames[]     : pfx_out_frame  (prev, line, class, function, filename, meta)
//   n_samples    : u32
//   samples[]    : { u32 leaf; u32 count }
//   end_magic    : 4 bytes "END0"
// ============================================================================

// 1-based index of s in strings, inserting it if new. 0 if s is NULL.
static uint32_t string_idx(HashTable* strings, zend_string* s) {
  if (!s) return 0;
  uintptr_t existing = (uintptr_t)zend_hash_find_ptr(strings, s);
  if (existing) return (uint32_t)existing;
  uint32_t idx = zend_hash_num_elements(strings) + 1;
  zend_hash_add_new_ptr(strings, s, (void*)(uintptr_t)idx);
  return idx;
}

static bool emit_binary(FILE* out) {
  pfx_out_frame* records = malloc(pfx_n_frames * sizeof(*records));
  if (!records) {
    php_error_docref(NULL, E_WARNING, "pfx: out of memory encoding profile");
    return false;
  }

  fwrite("PFX0", 4, 1, out);

  fwrite(&pfx_period_ns, sizeof(pfx_period_ns), 1, out);

  uint32_t uid = (uint32_t)geteuid();
  uint32_t gid = (uint32_t)getegid();
  fwrite(&uid, 4, 1, out);
  fwrite(&gid, 4, 1, out);

  uint32_t n_req_data = zend_hash_num_elements(&pfx_request_data);
  fwrite(&n_req_data, 4, 1, out);
  {
    zend_string* key;
    void* ptr;
    ZEND_HASH_FOREACH_STR_KEY_PTR(&pfx_request_data, key, ptr) {
      zend_string* value = (zend_string*)ptr;
      uint32_t key_len = (uint32_t)ZSTR_LEN(key);
      uint32_t value_len = (uint32_t)ZSTR_LEN(value);
      fwrite(&key_len, 4, 1, out);
      fwrite(ZSTR_VAL(key), 1, key_len, out);
      fwrite(&value_len, 4, 1, out);
      fwrite(ZSTR_VAL(value), 1, value_len, out);
    } ZEND_HASH_FOREACH_END();
  }

  HashTable strings;
  zend_hash_init(&strings, pfx_n_frames * 3 + 16, NULL, NULL, 1);

  for (uint32_t i = 1; i <= pfx_n_frames; i++) {
    pfx_frame* f = &pfx_frames[i];
    pfx_out_frame* record = &records[i - 1];

    record->prev = f->key.prev;
    record->line = f->line;
    record->class = string_idx(&strings, f->class_name);
    record->function = string_idx(&strings, f->function_name);
    record->filename = string_idx(&strings, f->filename);
    record->meta = string_idx(&strings, f->key.meta);
  }

  // Strings section.
  uint32_t n_strings = zend_hash_num_elements(&strings);
  fwrite(&n_strings, 4, 1, out);
  zend_string* key;
  ZEND_HASH_FOREACH_STR_KEY(&strings, key) {
    uint32_t len = (uint32_t)ZSTR_LEN(key);
    fwrite(&len, 4, 1, out);
    fwrite(ZSTR_VAL(key), 1, len, out);
  } ZEND_HASH_FOREACH_END();

  // Frames section.
  fwrite(&pfx_n_frames, 4, 1, out);
  fwrite(records, sizeof(*records), pfx_n_frames, out);

  // Samples section.
  uint32_t n_samples = zend_hash_num_elements(&pfx_sample_counts);
  fwrite(&n_samples, 4, 1, out);
  zend_ulong leaf;
  uint32_t* c;
  ZEND_HASH_FOREACH_NUM_KEY_PTR(&pfx_sample_counts, leaf, c) {
    uint32_t l = (uint32_t)leaf;
    uint32_t count = *c;
    fwrite(&l, 4, 1, out);
    fwrite(&count, 4, 1, out);
  } ZEND_HASH_FOREACH_END();

  fwrite("END0", 4, 1, out);

  free(records);
  zend_hash_destroy(&strings);

  // ferror() catches any short write or stream error from any fwrite above,
  // including memstream realloc failures.
  if (ferror(out)) return false;

  long written = ftell(out);
  if (pfx_max_bytes && written > 0 && (uint64_t)written > pfx_max_bytes) {
    php_error_docref(NULL, E_WARNING,
      "pfx: profile (%ld bytes) exceeds pfx.max_size (%llu); discarded",
      written, (unsigned long long)pfx_max_bytes);
    return false;
  }
  return true;
}


// ============================================================================
// Userland functions
// ============================================================================

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_pfx_start, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_pfx_stop, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_pfx_abort, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_pfx_meta, 0, 1, IS_VOID, 0)
  ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
  ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, meta_arg, IS_LONG, 0, "1")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_pfx_set, 0, 2, IS_VOID, 0)
  ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
  ZEND_ARG_TYPE_INFO(0, value, IS_STRING, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(pfx_start) {
  ZEND_PARSE_PARAMETERS_NONE();

  if (atomic_load(&pfx_running) || pfx_aborted) {
    return;
  }

  const char* url = INI_STR("pfx.endpoint");
  if (!url || !*url) {
    php_error_docref(NULL, E_WARNING, "pfx.endpoint is not configured");
    return;
  }

  double period_ms = 1.0;
  const char* s = INI_STR("pfx.period");
  if (s && *s) {
    double v = strtod(s, NULL);
    if (v > 0) period_ms = v;
  }

  // Some systems can't deliver sub-ms clock resolution.
  if (period_ms < 1.0) {
    struct timespec clock_res;
    if (clock_getres(CLOCK_MONOTONIC, &clock_res) == 0) {
      uint64_t clock_res_ns = (uint64_t)clock_res.tv_sec * 1000000000ULL + (uint64_t)clock_res.tv_nsec;
      uint64_t requested_period_ns = (uint64_t)(period_ms * 1000000.0);
      if (requested_period_ns < clock_res_ns) {
        php_error_docref(NULL, E_WARNING,
          "pfx.period (%gms) is below the kernel's clock resolution (%gms); falling back to 1ms",
          period_ms, (double)clock_res_ns / 1000000.0);
        period_ms = 1.0;
      }
    }
  }

  pfx_period_ns = (uint64_t)(period_ms * 1000000.0);

  pfx_max_bytes = 0;
  const char* m = INI_STR("pfx.max_size");
  if (m && *m) {
    char* end;
    long long v = strtoll(m, &end, 10);
    if (v > 0) {
      if (*end == 'K' || *end == 'k') v *= 1024;
      else if (*end == 'M' || *end == 'm') v *= 1024 * 1024;
      pfx_max_bytes = (uint64_t)v;
    }
  }

  pfx_init_state();

  pfx_timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
  if (pfx_timerfd < 0) {
    return;
  }

  struct itimerspec its;
  its.it_value.tv_sec = (time_t)(pfx_period_ns / 1000000000ULL);
  its.it_value.tv_nsec = (long)(pfx_period_ns % 1000000000ULL);
  its.it_interval = its.it_value;
  if (timerfd_settime(pfx_timerfd, 0, &its, NULL) != 0) {
    close(pfx_timerfd);
    pfx_timerfd = -1;
    return;
  }

  atomic_store(&pfx_ticks, 0);
  atomic_store(&pfx_running, true);

  if (pthread_create(&pfx_tick_thread, NULL, pfx_tick_loop, NULL) != 0) {
    atomic_store(&pfx_running, false);
    close(pfx_timerfd);
    pfx_timerfd = -1;
    return;
  }

  install_interrupt_fn();
}

PHP_FUNCTION(pfx_stop) {
  ZEND_PARSE_PARAMETERS_NONE();
  pfx_stop();
}

PHP_FUNCTION(pfx_abort) {
  ZEND_PARSE_PARAMETERS_NONE();
  pfx_stop();
  pfx_aborted = true;
}

PHP_FUNCTION(pfx_meta) {
  zend_string* name;
  zend_long meta_arg = PFX_META_FIRST_ARG;

  ZEND_PARSE_PARAMETERS_START(1, 2)
    Z_PARAM_STR(name)
    Z_PARAM_OPTIONAL
    Z_PARAM_LONG(meta_arg)
  ZEND_PARSE_PARAMETERS_END();

  bool ok;
  if (meta_arg == PFX_META_INHERIT
   || meta_arg == PFX_META_DEFINITION
   || meta_arg == PFX_META_CURL_URL) {
    ok = true;
  } else {
    int argpos = meta_arg & 0xFF;
    int flags = meta_arg & ~0xFF;
    ok = (argpos == PFX_META_FIRST_ARG || argpos == PFX_META_SECOND_ARG)
      && (flags & ~PFX_META_NORMALIZE_SQL) == 0;
  }
  if (!ok) {
    php_error_docref(
      NULL,
      E_WARNING,
      "pfx_meta: meta_arg must be PFX_META_* (got %ld)",
      (long)meta_arg
    );
    return;
  }

  void* encoded = (void*)(intptr_t)meta_arg;

  const char* val = ZSTR_VAL(name);
  size_t len = ZSTR_LEN(name);
  const char* sep = zend_memnstr(val, "::", 2, val + len);

  if (sep) {
    size_t class_len = sep - val;
    size_t func_off = class_len + 2;
    size_t func_len = len - func_off;

    if (class_len == 0 || func_len == 0) {
      php_error_docref(
        NULL,
        E_WARNING,
        "pfx_meta: name must be 'Class::method' with non-empty parts (got %s)",
        ZSTR_VAL(name)
      );
      return;
    }

    HashTable* inner = zend_hash_str_find_ptr(&pfx_meta_methods, val, class_len);
    if (!inner) {
      inner = pemalloc(sizeof(HashTable), 1);
      zend_hash_init(inner, 8, NULL, NULL, 1);
      zend_hash_str_add_ptr(&pfx_meta_methods, val, class_len, inner);
    }
    zend_hash_str_update_ptr(inner, val + func_off, func_len, encoded);
  } else {
    if (len == 0) {
      php_error_docref(NULL, E_WARNING, "pfx_meta: name must be non-empty");
      return;
    }
    zend_hash_update_ptr(&pfx_meta_fns, name, encoded);
  }
}

PHP_FUNCTION(pfx_set) {
  zend_string* key;
  zend_string* value;

  ZEND_PARSE_PARAMETERS_START(2, 2)
    Z_PARAM_STR(key)
    Z_PARAM_STR(value)
  ZEND_PARSE_PARAMETERS_END();

  if (ZSTR_LEN(key) == 0) {
    php_error_docref(NULL, E_WARNING, "pfx_set: key must be non-empty");
    return;
  }
  if (zend_hash_exists(&pfx_request_data, key)) {
    php_error_docref(NULL, E_WARNING, "pfx_set: key %s is already set", ZSTR_VAL(key));
    return;
  }

  zend_string_addref(value);
  zend_hash_add_ptr(&pfx_request_data, key, value);
}

static const zend_function_entry pfx_functions[] = {
  PHP_FE(pfx_start, arginfo_pfx_start)
  PHP_FE(pfx_stop, arginfo_pfx_stop)
  PHP_FE(pfx_abort, arginfo_pfx_abort)
  PHP_FE(pfx_meta, arginfo_pfx_meta)
  PHP_FE(pfx_set, arginfo_pfx_set)
  PHP_FE_END
};


// ============================================================================
// Module lifecycle
// ============================================================================

PHP_INI_BEGIN()
  PHP_INI_ENTRY("pfx.endpoint", "", PHP_INI_SYSTEM, NULL)
  PHP_INI_ENTRY("pfx.timeout", "25", PHP_INI_SYSTEM, NULL)
  PHP_INI_ENTRY("pfx.period", "1", PHP_INI_SYSTEM, NULL)
  PHP_INI_ENTRY("pfx.secret", "", PHP_INI_SYSTEM, NULL)
  PHP_INI_ENTRY("pfx.max_size", "0", PHP_INI_SYSTEM, NULL)
PHP_INI_END()

// Helper cb to silence libcurl output.
static size_t curl_discard_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  (void)ptr; (void)userdata;
  return size * nmemb;
}

// Write the profile directly to a new file <prefix>XXXXXX.bin via mkstemps.
static void pfx_write_profile(const char* prefix) {
  char tmpl[PATH_MAX];
  int n = snprintf(tmpl, sizeof(tmpl), "%sXXXXXX.bin", prefix);
  if (n <= 0 || n >= (int)sizeof(tmpl)) {
    php_error_docref(NULL, E_WARNING, "pfx: file path too long");
    return;
  }

  int fd = mkstemps(tmpl, 4);
  if (fd < 0) {
    php_error_docref(NULL, E_WARNING, "pfx: mkstemps(%s) failed: %s", tmpl, strerror(errno));
    return;
  }

  FILE* f = fdopen(fd, "wb");
  if (!f) {
    php_error_docref(NULL, E_WARNING, "pfx: fdopen(%s) failed: %s", tmpl, strerror(errno));
    close(fd);
    unlink(tmpl);
    return;
  }

  bool ok = emit_binary(f);
  if (fclose(f) != 0) ok = false;
  if (!ok) {
    php_error_docref(NULL, E_WARNING, "pfx: failed to write profile %s", tmpl);
    unlink(tmpl);
  }
}

// PUT len bytes at buf to url with lazy libcurl init.
static void pfx_put_profile(const char* url, const void* buf, size_t len) {
  if (!pfx_curl) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
      return;
    }
    pfx_curl = curl_easy_init();
    if (!pfx_curl) {
      curl_global_cleanup();
      return;
    }
    pfx_curl_headers = curl_slist_append(
        NULL, "Content-Type: application/octet-stream");
    if (pfx_secret) {
      char hdr[512];
      int n = snprintf(hdr, sizeof(hdr), "X-Pfx-Secret: %s", pfx_secret);
      if (n > 0 && (size_t)n < sizeof(hdr)) {
        pfx_curl_headers = curl_slist_append(pfx_curl_headers, hdr);
      } else {
        php_error_docref(NULL, E_WARNING,
          "pfx: secret too long; X-Pfx-Secret header skipped");
      }
    }
    curl_easy_setopt(pfx_curl, CURLOPT_HTTPHEADER, pfx_curl_headers);
    curl_easy_setopt(pfx_curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(pfx_curl, CURLOPT_WRITEFUNCTION, curl_discard_cb);
    curl_easy_setopt(pfx_curl, CURLOPT_CUSTOMREQUEST, "PUT");
  }

  long total_ms = 25;
  const char* t = INI_STR("pfx.timeout");
  if (t && *t) {
    long v = strtol(t, NULL, 10);
    if (v > 0) total_ms = v;
  }

  curl_easy_setopt(pfx_curl, CURLOPT_URL, url);
  curl_easy_setopt(pfx_curl, CURLOPT_POSTFIELDS, buf);
  curl_easy_setopt(pfx_curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)len);
  curl_easy_setopt(pfx_curl, CURLOPT_TIMEOUT_MS, total_ms);

  CURLcode rc = curl_easy_perform(pfx_curl);
  if (rc == CURLE_OPERATION_TIMEDOUT) {
    php_error_docref(NULL, E_WARNING, "pfx: PUT timed out after %ldms", total_ms);
  } else if (rc != CURLE_OK) {
    php_error_docref(NULL, E_WARNING, "pfx: PUT failed: %s", curl_easy_strerror(rc));
  } else {
    long code = 0;
    curl_easy_getinfo(pfx_curl, CURLINFO_RESPONSE_CODE, &code);
    if (code < 200 || code >= 300) {
      php_error_docref(NULL, E_WARNING, "pfx: PUT returned HTTP %ld", code);
    }
  }
}

PHP_MINIT_FUNCTION(pfx) {
#ifdef ZTS
  php_error_docref(NULL, E_CORE_WARNING, "pfx: ZTS is not supported");
  return FAILURE;
#endif

  REGISTER_INI_ENTRIES();
  REGISTER_LONG_CONSTANT("PFX_META_FIRST_ARG", PFX_META_FIRST_ARG, CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("PFX_META_SECOND_ARG", PFX_META_SECOND_ARG, CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("PFX_META_INHERIT", PFX_META_INHERIT, CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("PFX_META_DEFINITION", PFX_META_DEFINITION, CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("PFX_META_CURL_URL", PFX_META_CURL_URL, CONST_CS | CONST_PERSISTENT);
  REGISTER_LONG_CONSTANT("PFX_META_NORMALIZE_SQL", PFX_META_NORMALIZE_SQL, CONST_CS | CONST_PERSISTENT);
  pthread_atfork(NULL, NULL, pfx_atfork_child);

  // Read pfx.secret then blank the entry so userland's ini_get can't see it.
  const char* secret = INI_STR("pfx.secret");
  if (secret && *secret) {
    pfx_secret = pestrdup(secret, 1);
    zend_string* name = zend_string_init(ZEND_STRL("pfx.secret"), 1);
    zend_alter_ini_entry_chars(name, "", 0, ZEND_INI_SYSTEM, ZEND_INI_STAGE_STARTUP);
    zend_string_release(name);
  }

  return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(pfx) {
  UNREGISTER_INI_ENTRIES();
  if (pfx_curl) {
    curl_easy_cleanup(pfx_curl);
    curl_slist_free_all(pfx_curl_headers);
    curl_global_cleanup();
    pfx_curl = NULL;
    pfx_curl_headers = NULL;
  }
  if (pfx_secret) {
    pefree(pfx_secret, 1);
    pfx_secret = NULL;
  }
  return SUCCESS;
}

PHP_RINIT_FUNCTION(pfx) {
  pfx_initialized = false;
  pfx_aborted = false;
  atomic_store(&pfx_running, false);
  zend_hash_init(&pfx_meta_fns, 32, NULL, NULL, 1);
  zend_hash_init(&pfx_meta_methods, 8, NULL, pfx_meta_methods_dtor, 1);
  zend_hash_init(&pfx_request_data, 8, NULL, pfx_request_data_dtor, 1);
  zend_hash_init(&pfx_meta_strings, 32, NULL, NULL, 1);
  return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(pfx) {
  if (atomic_load(&pfx_running)) {
    pfx_stop();
  }

  zend_hash_destroy(&pfx_meta_fns);
  zend_hash_destroy(&pfx_meta_methods);

  if (!pfx_initialized) {
    zend_hash_destroy(&pfx_request_data);
    zend_hash_destroy(&pfx_meta_strings);
    return SUCCESS;
  }

  const char* url = INI_STR("pfx.endpoint");
  if (!pfx_aborted && url && *url && zend_hash_num_elements(&pfx_sample_counts) > 0) {
    if (strncmp(url, "file://", 7) == 0) {
      pfx_write_profile(url + 7);
    } else {
      char* buf = NULL;
      size_t len = 0;
      FILE* mem = open_memstream(&buf, &len);
      if (mem) {
        bool ok = emit_binary(mem);
        if (fclose(mem) != 0) ok = false;
        if (ok && len > 0) {
          pfx_put_profile(url, buf, len);
        }
        free(buf);
      }
    }
  }

  zend_hash_destroy(&pfx_request_data);
  zend_hash_destroy(&pfx_sample_counts);
  zend_hash_destroy(&pfx_frame_dedup);

  // Release each frame's string refs before freeing the array.
  for (uint32_t i = 1; i <= pfx_n_frames; i++) {
    pfx_frame* f = &pfx_frames[i];
    if (f->class_name) zend_string_release(f->class_name);
    if (f->function_name) zend_string_release(f->function_name);
    if (f->filename) zend_string_release(f->filename);
    if (f->key.meta) zend_string_release(f->key.meta);
  }
  if (pfx_frames) {
    free(pfx_frames);
    pfx_frames = NULL;
  }
  pfx_n_frames = 0;
  pfx_cap_frames = 0;
  pfx_initialized = false;

  zend_hash_destroy(&pfx_meta_strings);

  return SUCCESS;
}

PHP_MINFO_FUNCTION(pfx) {
  php_info_print_table_start();
  php_info_print_table_row(2, "pfx", "loaded");
  php_info_print_table_row(2, "version", PHP_PFX_VERSION);
  php_info_print_table_end();
}

zend_module_entry pfx_module_entry = {
  STANDARD_MODULE_HEADER,
  "pfx",
  pfx_functions,
  PHP_MINIT(pfx),
  PHP_MSHUTDOWN(pfx),
  PHP_RINIT(pfx),
  PHP_RSHUTDOWN(pfx),
  PHP_MINFO(pfx),
  PHP_PFX_VERSION,
  STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_PFX
ZEND_GET_MODULE(pfx)
#endif
