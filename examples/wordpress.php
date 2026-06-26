<?php
function __pfx_init() {
	if ( ! function_exists( 'pfx_start' ) ) {
		return;
	}

	pfx_set( 'timestamp', time() );
	pfx_set( 'http_host', $_SERVER['HTTP_HOST'] ?? '' );
	pfx_set( 'request_uri', $_SERVER['REQUEST_URI'] ?? '' );
	pfx_set( 'request_method', $_SERVER['REQUEST_METHOD'] ?? '' );
	pfx_set( 'request_id', uniqid() );

	// Inherit from caller WP_Hook's inner dispatch
	pfx_capture( 'WP_Hook::apply_filters', PFX_META_INHERIT );
	pfx_capture( 'WP_Hook::do_action', PFX_META_INHERIT );

	// Actions/filters
	pfx_capture( 'apply_filters' );
	pfx_capture( 'apply_filters_ref_array' );
	pfx_capture( 'do_action' );
	pfx_capture( 'do_action_ref_array' );

	// Database
	pfx_capture( 'wpdb::query', PFX_META_FIRST_ARG | PFX_META_NORMALIZE_SQL );
	pfx_capture( 'wpdb::_do_query', PFX_META_FIRST_ARG | PFX_META_NORMALIZE_SQL );
	pfx_capture( 'wpdb::get_results', PFX_META_FIRST_ARG | PFX_META_NORMALIZE_SQL );
	pfx_capture( 'wpdb::get_col', PFX_META_FIRST_ARG | PFX_META_NORMALIZE_SQL );
	pfx_capture( 'wpdb::get_row', PFX_META_FIRST_ARG | PFX_META_NORMALIZE_SQL );
	pfx_capture( 'wpdb::get_var', PFX_META_FIRST_ARG | PFX_META_NORMALIZE_SQL );
	pfx_capture( 'mysqli_query', PFX_META_SECOND_ARG | PFX_META_NORMALIZE_SQL );
	pfx_capture( 'QM_DB::query', PFX_META_FIRST_ARG | PFX_META_NORMALIZE_SQL );

	// HTTP
	pfx_capture( 'wp_remote_get' );
	pfx_capture( 'wp_remote_post' );
	pfx_capture( 'wp_remote_head' );
	pfx_capture( 'wp_remote_request' );
	pfx_capture( 'wp_safe_remote_get' );
	pfx_capture( 'wp_safe_remote_post' );
	pfx_capture( 'wp_safe_remote_head' );
	pfx_capture( 'wp_safe_remote_request' );
	pfx_capture( 'WP_Http::request' );
	pfx_capture( 'WP_Http::get' );
	pfx_capture( 'WP_Http::post' );
	pfx_capture( 'WP_Http::head' );
	pfx_capture( 'curl_exec', PFX_META_CURL_URL );

	// Misc
	pfx_capture( '{closure}', PFX_META_DEFINITION );

	register_shutdown_function( function() {
		pfx_set( 'php_sapi', php_sapi_name() );
		pfx_set( 'php_version', phpversion() );
		pfx_set( 'peak_memory', memory_get_peak_usage( true ) );

		pfx_set( 'user_agent', $_SERVER['HTTP_USER_AGENT'] ?? '' );

		$user = wp_get_current_user();

		pfx_set( 'user_login', $user->user_login ?? '' );
		pfx_set( 'user_id', $user->ID ?? '0' );
		pfx_set( 'user_email', $user->user_email ?? '' );
		pfx_set( 'wp_version', $GLOBALS['wp_version'] );

		if ( defined( 'WC_VERSION' ) ) {
			pfx_set( 'woocommerce_version', WC_VERSION );
		}

		pfx_set( 'uname', php_uname( 'a' ) );
		pfx_set( 'is_ssl', is_ssl() );
		pfx_set( 'is_admin', is_admin() );

		$theme = wp_get_theme();
		pfx_set( 'theme', $theme->get_stylesheet() );
		pfx_set( 'object_cache_ext', (string) wp_using_ext_object_cache() );
		pfx_set( 'locale', get_locale() );

		if ( wp_doing_ajax() ) {
			pfx_set( 'ajax_action', $_REQUEST['action'] ?? '' );
		}

		$response_code = http_response_code();
		if ( php_sapi_name() == 'cli' ) {
			$response_code = error_get_last() === null ? 0 : 1;
		}

		pfx_set( 'response_code', $response_code );
	} );

	// Start profiling.
	pfx_start();
}

__pfx_init();
