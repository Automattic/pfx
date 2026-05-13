# PFX

PHP Frame Sampler: a sampling profiler PHP extension that records per-frame
timing for live requests and sends the resulting profile binary to a configured
endpoint. Designed to run at scale in production.

PFX inspects and records the current call stack at pre-configured intervals at
the Zend VM opcode boundaries. Function calls quicker than the interval may not
appear in the resulting profile.

## Requirements

* PHP 8.3+
* PHP NTS (ZTS is not supported)
* Linux (uses `timerfd`)
* libcurl 7.84+ (for thread-safe implicit `curl_easy_init`)

## Building and installing

Build the extension from source:

    phpize
    ./configure
    make
    sudo make install

Or place a pre-compiled .so file into the PHP extensions directory (you can
find it with `php-config --extension-dir`).

Enable the extension in your php.ini file:

    extension=pfx.so

Configure the endpoint, which is where the profile is sent during request
shutdown. PFX supports sending the profile to an HTTP service if the endpoint
starts with `http://` or `https://`, or writing the data directly to a file if
the endpoint starts with `file://`.

For best performance use a local HTTP service, which accepts the payload and
returns immediately. An optional `pfx.timeout` in milliseconds could be
provided to avoid stalling a PHP worker when the collection service is slow to
respond.

The sampling period defaults to 1 millisecond and can be changed with
`pfx.period` (in milliseconds, accepts fractional values such as `0.5`). Lower
values give finer-grained profiles at the cost of more sampling overhead and
a larger output file size. Note that some systems do not support clock
resolution lower than 1 ms.

An optional `pfx.max_size` can be set to limit the output file size, which
aborts with a warning when the size is reached.

Below is an example configuration:

    pfx.endpoint=http://127.0.0.1:8080
    pfx.timeout=25
    pfx.period=1
    pfx.max_size=1m

Enabling the extension does not automatically enable profiling. This has to be
done separately from PHP.

## Usage

You can start profiling at any time from PHP using the `pfx_start()` function.
Anything before this function call will not be included. You can also add logic
to profile specific requests, rather than profiling everything, for example:

    if ( $_SERVER['REQUEST_URI'] == '/some/slow/page/' ) {
        pfx_start();
    }

Profiling will run until the end of the request by default. You can stop it
earlier with `pfx_stop()` if needed. The profile data will be written to the
configured endpoint, unless aborted using `pfx_abort()`.

## Function metadata

Use `pfx_meta()` to register a function or method (`Class::method` format)
whose frames should carry extra metadata in the profile, so you can tell
otherwise-identical frames apart. For example, attaching the first argument of
`apply_filters()` in WordPress splits that frame by the filter name (`init`,
`the_content`, etc.) instead of collapsing everything into one `apply_filters`
entry:

    pfx_meta( 'apply_filters' );

By default, `pfx_meta()` will capture the first argument if it's a string. This
behavior can be changed using different modes:

* `PFX_META_FIRST_ARG` (default) uses `$args[0]` if it is a string.
* `PFX_META_SECOND_ARG` uses `$args[1]` if it is a string.
* `PFX_META_INHERIT` inherits the caller's metadata.
* `PFX_META_DEFINITION` uses `filename:lineno` of the function definition.
* `PFX_META_CURL_URL` uses the effective URL set on a cURL handler, use with `curl_exec`.

Examples:

    pfx_meta( 'WP_Hook::do_action', PFX_META_INHERIT );

Some of these can be combined with transform flags to further alter the
resulting frame metadata:

* `PFX_META_NORMALIZE_SQL` replaces string and numeric literals with `?`

Normalize example:

    pfx_meta( 'wpdb::query', PFX_META_FIRST_ARG | PFX_META_NORMALIZE_SQL );

You can call `pfx_meta()` at any time during the request, however it is
recommended to run all `pfx_meta()` registration before running `pfx_start()`.
The functions do not have to be defined (or even exist) for metadata-capture
registration.

See [`examples/wordpress.php`](examples/wordpress.php) for a set of calls
tailored to WordPress (hooks, database, HTTP).

## Request data

You can use `pfx_set()` to add arbitrary data to the output profile. This can
be used at any time during the request, regardless of whether the profiling has
been started, stopped or aborted.

Use this to pass request IDs, request URIs, timestamps, current user and other
state, which can be useful for profiling:

    pfx_set( 'request_uri', $_SERVER['REQUEST_URI'] );
    pfx_set( 'user_id', (string) get_current_user_id() );
    pfx_set( 'timestamp', (string) time() );

Note that `pfx_set()` accepts strings only. Each key must be unique and will
result in a warning if the same key is used more than once.

## Output format

The resulting profile uses the following format:

    magic        : 4 bytes "PFX0"
    period_ns    : u64
    uid          : u32
    gid          : u32
    n_req_data   : u32
    req_data[]   : { u32 klen; bytes; u32 vlen; bytes }
    n_strings    : u32
    strings[]    : { u32 len; bytes }
    n_frames     : u32
    frames[]     : pfx_out_frame  (prev, line, class, function, filename, meta)
    n_samples    : u32
    samples[]    : { u32 leaf; u32 count }
    end_magic    : 4 bytes "END0"

The `magic` is always `PFX0` for this binary format.

The `period_ns` is the sampling period in nanoseconds.

The `uid` and `gid` numbers are the effective user and group IDs of the running
process.

The request data is stored in `req_data` and can be read without reading the
entire file. This may be useful for quickly indexing or summarizing profiles.

The `strings` section contains unique strings (function and class names,
filenames and metadata) used throughout the frames. Each string can be
referenced multiple times in the `frames` section.

The `frames` section contains every frame captured during the profile. Each
frame can reference `strings` (1-based index, 0 for empty/missing) for class
name, function name, etc. Each frame can also reference the caller frame by
its 1-based index, with 0 being the root.

The `samples` section references a 1-based index leaf frame, and the total
number of samples for that frame.

The `end_magic` is always `END0` for this format, and can be used to determine
whether the profile is complete.
