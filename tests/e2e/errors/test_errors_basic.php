<?php
/**
 * Basic Error Test Script
 * 
 * Tests basic PHP error types and simple exceptions
 * 
 * Usage:
 *   php test_errors_basic.php
 */

echo "=== Basic Error Test ===\n\n";

// Check if opa extension is loaded
if (!extension_loaded('opa')) {
    die("ERROR: OPA extension is not loaded. Please ensure the extension is installed.\n");
}

echo "✓ OPA extension is loaded\n";
echo "Service: " . ini_get('opa.service') . "\n";
echo "Track Errors: " . (ini_get('opa.track_errors') ? 'enabled' : 'disabled') . "\n";
echo "Track Logs: " . (ini_get('opa.track_logs') ? 'enabled' : 'disabled') . "\n\n";

// Test 1: Manual error tracking via opa_track_error()
echo "=== Test 1: Manual Error Tracking ===\n";
if (function_exists('opa_track_error')) {
    echo "Testing opa_track_error() with custom error...\n";
    opa_track_error(
        'CustomError',
        'This is a manually tracked error message',
        __FILE__,
        __LINE__,
        debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS)
    );
    echo "✓ Manual error tracking completed\n";
    usleep(200000); // Delay for processing
} else {
    echo "✗ opa_track_error() function does not exist\n";
}
echo "\n";

// Test 2: PHP User Errors
echo "=== Test 2: PHP User Errors ===\n";
if (ini_get('opa.track_errors')) {
    echo "Generating E_USER_ERROR...\n";
    trigger_error('This is a test E_USER_ERROR - critical user error', E_USER_ERROR);
    echo "  ✓ E_USER_ERROR triggered\n";
    usleep(200000);
    
    echo "Generating E_USER_WARNING...\n";
    trigger_error('This is a test E_USER_WARNING - user warning', E_USER_WARNING);
    echo "  ✓ E_USER_WARNING triggered\n";
    usleep(200000);
    
    echo "Generating E_USER_NOTICE...\n";
    trigger_error('This is a test E_USER_NOTICE - user notice', E_USER_NOTICE);
    echo "  ✓ E_USER_NOTICE triggered\n";
    usleep(200000);
    
    echo "Generating E_USER_DEPRECATED...\n";
    trigger_error('This is a test E_USER_DEPRECATED - deprecated feature used', E_USER_DEPRECATED);
    echo "  ✓ E_USER_DEPRECATED triggered\n";
    usleep(200000);
} else {
    echo "⚠ opa.track_errors is disabled, skipping user error tests\n";
}
echo "\n";

// Test 3: Standard PHP Errors (if possible)
echo "=== Test 3: Standard PHP Errors ===\n";
if (ini_get('opa.track_errors')) {
    // These require error handler registration
    echo "Note: Standard PHP errors (E_WARNING, E_NOTICE) require error handler\n";
    echo "Testing E_WARNING via undefined variable...\n";
    @$undefined_var; // Suppress to avoid actual error output
    echo "  ✓ Warning test completed\n";
    usleep(200000);
} else {
    echo "⚠ opa.track_errors is disabled\n";
}
echo "\n";

// Test 4: Basic Exception
echo "=== Test 4: Basic Exception ===\n";
try {
    throw new Exception('This is a basic Exception test message');
} catch (Exception $e) {
    echo "✓ Exception caught: " . $e->getMessage() . "\n";
    // Manually track if opa_track_error exists
    if (function_exists('opa_track_error')) {
        opa_track_error(
            get_class($e),
            $e->getMessage(),
            $e->getFile(),
            $e->getLine(),
            $e->getTrace()
        );
        echo "  ✓ Exception manually tracked\n";
    }
    usleep(200000);
}
echo "\n";

// Test 5: RuntimeException
echo "=== Test 5: RuntimeException ===\n";
try {
    throw new RuntimeException('This is a RuntimeException test - runtime error occurred');
} catch (RuntimeException $e) {
    echo "✓ RuntimeException caught: " . $e->getMessage() . "\n";
    if (function_exists('opa_track_error')) {
        opa_track_error(
            get_class($e),
            $e->getMessage(),
            $e->getFile(),
            $e->getLine(),
            $e->getTrace()
        );
        echo "  ✓ RuntimeException manually tracked\n";
    }
    usleep(200000);
}
echo "\n";

// Test 6: Error with context information
echo "=== Test 6: Error with Context ===\n";
function testFunctionWithError() {
    if (function_exists('opa_track_error')) {
        opa_track_error(
            'FunctionError',
            'Error occurred in testFunctionWithError()',
            __FILE__,
            __LINE__,
            debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS)
        );
    }
}

testFunctionWithError();
echo "✓ Function error test completed\n";
usleep(200000);
echo "\n";

// Summary
echo "=== Test Summary ===\n";
echo "Basic error tests completed.\n";
echo "Check the dashboard at /errors to verify:\n";
echo "  - Manual errors appear\n";
echo "  - User errors (E_USER_ERROR, E_USER_WARNING, etc.) appear\n";
echo "  - Exceptions (Exception, RuntimeException) appear\n";
echo "  - Error messages are correct\n";
echo "  - Stack traces are captured\n";
echo "\n";
