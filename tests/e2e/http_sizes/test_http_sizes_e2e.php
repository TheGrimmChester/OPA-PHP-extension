<?php
/**
 * E2E Test for HTTP Request and Response Size Tracking
 * 
 * This test validates that:
 * 1. request_size is tracked in tags.http_request
 * 2. response_size is tracked in tags.http_response
 * 3. Sizes include headers, body, and files (for requests)
 * 4. Data is properly stored in ClickHouse
 * 
 * This script generates a response with a known size.
 * It should be accessed via HTTP request to properly test request/response size tracking.
 */

// Set service name for identification in ClickHouse
if (function_exists('ini_set')) {
    ini_set('opa.service', 'http-sizes-test');
}

// Generate response with known size
// Response body will be approximately 250 bytes
$response_data = [
    'test' => 'http_sizes',
    'timestamp' => time(),
    'message' => str_repeat('X', 200), // 200 bytes
    'description' => 'HTTP request/response size tracking test'
];

// Output JSON response (approximately 250 bytes total)
header('Content-Type: application/json');
echo json_encode($response_data, JSON_PRETTY_PRINT);

// Ensure output is flushed
if (function_exists('fastcgi_finish_request')) {
    fastcgi_finish_request();
}

