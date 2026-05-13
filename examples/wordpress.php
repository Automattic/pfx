<?php
// Inherit from caller WP_Hook's inner dispatch
pfx_meta( 'WP_Hook::apply_filters', PFX_META_INHERIT );
pfx_meta( 'WP_Hook::do_action', PFX_META_INHERIT );

// Actions/filters
pfx_meta( 'apply_filters' );
pfx_meta( 'apply_filters_ref_array' );
pfx_meta( 'do_action' );
pfx_meta( 'do_action_ref_array' );

// Database
pfx_meta( 'wpdb::query', PFX_META_FIRST_ARG | PFX_META_NORMALIZE_SQL );
pfx_meta( 'wpdb::_do_query', PFX_META_FIRST_ARG | PFX_META_NORMALIZE_SQL );

// HTTP
pfx_meta( 'wp_remote_get' );
pfx_meta( 'wp_remote_post' );
pfx_meta( 'wp_remote_head' );
pfx_meta( 'wp_remote_request' );
pfx_meta( 'wp_safe_remote_get' );
pfx_meta( 'wp_safe_remote_post' );
pfx_meta( 'wp_safe_remote_head' );
pfx_meta( 'wp_safe_remote_request' );
pfx_meta( 'WP_Http::request' );
pfx_meta( 'WP_Http::get' );
pfx_meta( 'WP_Http::post' );
pfx_meta( 'WP_Http::head' );
pfx_meta( 'curl_exec', PFX_META_CURL_URL );

// Misc
pfx_meta( '{closure}', PFX_META_DEFINITION );

pfx_start();
