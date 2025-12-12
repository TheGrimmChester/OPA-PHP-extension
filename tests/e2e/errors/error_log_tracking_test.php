<?php
/**
 * Error and Log Tracking Test
 * 
 * Tests automatic error and log tracking functionality
 * 
 * Usage:
 *   php error_log_tracking_test.php
 */

echo "=== Error and Log Tracking Test ===\n\n";

// Check if opa extension is loaded
if (!extension_loaded('opa')) {
    die("ERROR: OPA extension is not loaded. Please ensure the extension is installed.\n");
}

echo "✓ OPA extension is loaded\n\n";

// Test 1: Check if opa_track_error function exists
echo "=== Test 1: Manual Error Tracking ===\n";
if (function_exists('opa_track_error')) {
    echo "✓ opa_track_error() function exists\n";
    
    // Test manual error tracking
    try {
        opa_track_error(
            'TestError',
            'This is a test error message',
            __FILE__,
            50,
            debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS)
        );
        echo "✓ Manual error tracking called successfully\n";
    } catch (Exception $e) {
        echo "✗ Error tracking failed: " . $e->getMessage() . "\n";
    }
} else {
    echo "✗ opa_track_error() function does not exist\n";
}
echo "\n";

// Test 2: Test error_log() hook for log tracking
echo "=== Test 2: Log Tracking via error_log() ===\n";

// Test different log levels
$test_logs = [
    '[ERROR] This is an error log message',
    '[WARN] This is a warning log message',
    '[CRITICAL] This is a critical log message',
    'error: This is another error format',
    'warning: This is another warning format',
    'critical: This is another critical format',
    'This is a regular info message',
];

foreach ($test_logs as $log_message) {
    echo "Testing: $log_message\n";
    error_log($log_message);
    echo "  ✓ error_log() called\n";
    usleep(100000); // Small delay to ensure messages are sent
}
echo "\n";

// Test 3: Test PHP error generation (if track_errors is enabled)
echo "=== Test 3: PHP Error Tracking ===\n";
echo "Note: This test requires opa.track_errors=1 in php.ini\n";

// Generate a user error (non-fatal)
if (ini_get('opa.track_errors')) {
    echo "Generating E_USER_ERROR...\n";
    trigger_error('This is a test user error', E_USER_ERROR);
    echo "  ✓ User error triggered\n";
    
    echo "Generating E_USER_WARNING...\n";
    trigger_error('This is a test user warning', E_USER_WARNING);
    echo "  ✓ User warning triggered\n";
} else {
    echo "⚠ opa.track_errors is disabled, skipping error generation test\n";
}
echo "\n";

// Test 4: Test exception tracking (if available)
echo "=== Test 4: Exception Tracking ===\n";
try {
    throw new Exception('This is a test exception');
} catch (Exception $e) {
    echo "✓ Exception caught: " . $e->getMessage() . "\n";
    // Note: Exception tracking may require manual opa_track_error() call
    // or a registered exception handler
}
echo "\n";

// Test 5: Verify INI settings
echo "=== Test 5: INI Configuration ===\n";
$ini_settings = [
    'opa.track_errors' => ini_get('opa.track_errors'),
    'opa.track_logs' => ini_get('opa.track_logs'),
    'opa.log_levels' => ini_get('opa.log_levels'),
    'opa.enabled' => ini_get('opa.enabled'),
];

foreach ($ini_settings as $setting => $value) {
    echo "  $setting = " . ($value !== false ? $value : 'not set') . "\n";
}
echo "\n";

// Summary
echo "=== Test Summary ===\n";
echo "All tests completed. Check the agent logs and ClickHouse database to verify:\n";
echo "  1. Error messages are stored in opa.error_instances and opa.error_groups\n";
echo "  2. Log messages are stored in opa.logs table\n";
echo "  3. Messages are properly associated with trace_id and span_id\n";
echo "\n";
echo "To verify in ClickHouse:\n";
echo "  SELECT * FROM opa.logs ORDER BY timestamp DESC LIMIT 10;\n";
echo "  SELECT * FROM opa.error_instances ORDER BY occurred_at DESC LIMIT 10;\n";
echo "\n";
