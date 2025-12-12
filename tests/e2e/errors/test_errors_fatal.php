<?php
/**
 * Fatal Error Test Script
 * 
 * Tests fatal errors and type errors
 * Note: Some fatal errors cannot be caught, but we test recoverable ones
 * 
 * Usage:
 *   php test_errors_fatal.php
 */

echo "=== Fatal Error Test ===\n\n";

// Check if opa extension is loaded
if (!extension_loaded('opa')) {
    die("ERROR: OPA extension is not loaded. Please ensure the extension is installed.\n");
}

echo "✓ OPA extension is loaded\n";
echo "Service: " . ini_get('opa.service') . "\n";
echo "Track Errors: " . (ini_get('opa.track_errors') ? 'enabled' : 'disabled') . "\n\n";

// Test 1: Recoverable Type Error
echo "=== Test 1: Recoverable Type Error ===\n";
if (function_exists('opa_track_error')) {
    // Simulate a type error scenario
    function processString(string $value): string {
        return strtoupper($value);
    }
    
    try {
        // This would cause TypeError in strict mode
        // We'll manually track it since we can't easily trigger it
        opa_track_error(
            'TypeError',
            'Type error: processString() expects parameter 1 to be string, integer given',
            __FILE__,
            __LINE__,
            debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS)
        );
        echo "✓ Type error tracked\n";
        usleep(200000);
    } catch (Exception $e) {
        echo "✓ Exception during type error test: " . $e->getMessage() . "\n";
    }
} else {
    echo "✗ opa_track_error() function does not exist\n";
}
echo "\n";

// Test 2: Call to Undefined Function (E_ERROR)
echo "=== Test 2: Undefined Function Error ===\n";
if (ini_get('opa.track_errors')) {
    // We'll manually track this since calling undefined function would stop execution
    if (function_exists('opa_track_error')) {
        opa_track_error(
            'Error',
            'Call to undefined function undefined_function_xyz123()',
            __FILE__,
            __LINE__,
            debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS)
        );
        echo "✓ Undefined function error tracked\n";
        usleep(200000);
    }
} else {
    echo "⚠ opa.track_errors is disabled\n";
}
echo "\n";

// Test 3: Call to Undefined Method
echo "=== Test 3: Undefined Method Error ===\n";
class TestClass {
    public function existingMethod() {
        return 'test';
    }
}

try {
    $obj = new TestClass();
    // This would cause fatal error, so we simulate it
    if (function_exists('opa_track_error')) {
        opa_track_error(
            'Error',
            'Call to undefined method TestClass::undefinedMethod()',
            __FILE__,
            __LINE__,
            debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS)
        );
        echo "✓ Undefined method error tracked\n";
        usleep(200000);
    }
} catch (Exception $e) {
    echo "✓ Exception: " . $e->getMessage() . "\n";
}
echo "\n";

// Test 4: Access to Undefined Property
echo "=== Test 4: Undefined Property Error ===\n";
if (function_exists('opa_track_error')) {
    opa_track_error(
        'Warning',
        'Undefined property: TestClass::$undefinedProperty',
        __FILE__,
        __LINE__,
        debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS)
    );
    echo "✓ Undefined property error tracked\n";
    usleep(200000);
}
echo "\n";

// Test 5: Maximum Execution Time (simulated)
echo "=== Test 5: Maximum Execution Time Error ===\n";
if (function_exists('opa_track_error')) {
    opa_track_error(
        'Error',
        'Maximum execution time of 30 seconds exceeded',
        __FILE__,
        __LINE__,
        debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS)
    );
    echo "✓ Maximum execution time error tracked\n";
    usleep(200000);
}
echo "\n";

// Test 6: Memory Limit Error (simulated)
echo "=== Test 6: Memory Limit Error ===\n";
if (function_exists('opa_track_error')) {
    opa_track_error(
        'Error',
        'Allowed memory size of 134217728 bytes exhausted (tried to allocate 1048576 bytes)',
        __FILE__,
        __LINE__,
        debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS)
    );
    echo "✓ Memory limit error tracked\n";
    usleep(200000);
}
echo "\n";

// Test 7: Parse Error (simulated - can't actually parse in same file)
echo "=== Test 7: Parse Error ===\n";
if (function_exists('opa_track_error')) {
    opa_track_error(
        'Parse',
        'Parse error: syntax error, unexpected \'}\' in test_file.php on line 42',
        'test_file.php',
        42,
        debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS)
    );
    echo "✓ Parse error tracked\n";
    usleep(200000);
}
echo "\n";

// Test 8: Fatal Error in Shutdown Function Context
echo "=== Test 8: Fatal Error Context ===\n";
// Register shutdown function to demonstrate fatal error tracking
register_shutdown_function(function() {
    $error = error_get_last();
    if ($error !== null && in_array($error['type'], [E_ERROR, E_CORE_ERROR, E_COMPILE_ERROR, E_PARSE])) {
        if (function_exists('opa_track_error')) {
            $errorTypeMap = [
                E_ERROR => 'Error',
                E_CORE_ERROR => 'CoreError',
                E_COMPILE_ERROR => 'CompileError',
                E_PARSE => 'Parse',
            ];
            $errorType = $errorTypeMap[$error['type']] ?? 'Error';
            opa_track_error(
                $errorType,
                $error['message'],
                $error['file'],
                $error['line'],
                null
            );
        }
    }
});

if (function_exists('opa_track_error')) {
    // Simulate a fatal error scenario
    opa_track_error(
        'Error',
        'Fatal error: Uncaught TypeError in shutdown context',
        __FILE__,
        __LINE__,
        null
    );
    echo "✓ Fatal error in shutdown context tracked\n";
    usleep(200000);
}
echo "\n";

// Test 9: Division by Zero (Warning, not fatal but common)
echo "=== Test 9: Division by Zero ===\n";
if (ini_get('opa.track_errors')) {
    // This generates E_WARNING
    @(1 / 0); // Suppress output
    echo "✓ Division by zero warning generated\n";
    usleep(200000);
} else {
    echo "⚠ opa.track_errors is disabled\n";
}
echo "\n";

// Test 10: Array to String Conversion
echo "=== Test 10: Array to String Conversion ===\n";
if (ini_get('opa.track_errors')) {
    // This generates E_NOTICE
    @(string)[1, 2, 3]; // Suppress output
    echo "✓ Array to string conversion notice generated\n";
    usleep(200000);
} else {
    echo "⚠ opa.track_errors is disabled\n";
}
echo "\n";

// Summary
echo "=== Test Summary ===\n";
echo "Fatal error tests completed.\n";
echo "Note: Some fatal errors cannot be easily tested without stopping execution.\n";
echo "Check the dashboard at /errors to verify:\n";
echo "  - Type errors are captured\n";
echo "  - Undefined function/method errors are captured\n";
echo "  - Memory and execution time errors are captured\n";
echo "  - Parse errors are captured\n";
echo "  - Fatal errors in shutdown context are captured\n";
echo "\n";
