<?php
/**
 * Context Error Test Script
 * 
 * Tests errors in various contexts (functions, classes, SQL, cURL)
 * 
 * Usage:
 *   php test_errors_context.php
 */

echo "=== Context Error Test ===\n\n";

// Check if opa extension is loaded
if (!extension_loaded('opa')) {
    die("ERROR: OPA extension is not loaded. Please ensure the extension is installed.\n");
}

echo "✓ OPA extension is loaded\n";
echo "Service: " . ini_get('opa.service') . "\n\n";

// Helper function to track error
function trackError($errorType, $message, $file = null, $line = null, $trace = null) {
    if (function_exists('opa_track_error')) {
        opa_track_error(
            $errorType,
            $message,
            $file ?? __FILE__,
            $line ?? __LINE__,
            $trace ?? debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS)
        );
        return true;
    }
    return false;
}

// Test 1: Error in Simple Function
echo "=== Test 1: Error in Simple Function ===\n";
function simpleFunctionWithError() {
    trackError(
        'FunctionError',
        'Error occurred in simpleFunctionWithError() - invalid parameter',
        __FILE__,
        __LINE__
    );
    return false;
}

simpleFunctionWithError();
echo "✓ Function error test completed\n";
usleep(200000);
echo "\n";

// Test 2: Error in Class Method
echo "=== Test 2: Error in Class Method ===\n";
class DataProcessor {
    private $data;
    
    public function __construct($data) {
        $this->data = $data;
    }
    
    public function process() {
        if (empty($this->data)) {
            trackError(
                'ClassMethodError',
                'DataProcessor::process() - data is empty',
                __FILE__,
                __LINE__
            );
            throw new RuntimeException('Cannot process empty data');
        }
        return $this->data;
    }
    
    public function validate() {
        if (!is_array($this->data)) {
            trackError(
                'ValidationError',
                'DataProcessor::validate() - data must be an array',
                __FILE__,
                __LINE__
            );
            return false;
        }
        return true;
    }
}

try {
    $processor = new DataProcessor(null);
    $processor->process();
} catch (RuntimeException $e) {
    echo "✓ Class method error caught: " . $e->getMessage() . "\n";
    usleep(200000);
}

$processor2 = new DataProcessor('string');
$processor2->validate();
echo "✓ Class validation error test completed\n";
usleep(200000);
echo "\n";

// Test 3: Error in Nested Function Calls
echo "=== Test 3: Error in Nested Function Calls ===\n";
function outerFunction() {
    return middleFunction();
}

function middleFunction() {
    return innerFunction();
}

function innerFunction() {
    trackError(
        'NestedError',
        'Error in innerFunction() - deep in call stack',
        __FILE__,
        __LINE__
    );
    throw new LogicException('Error occurred deep in call stack');
}

try {
    outerFunction();
} catch (LogicException $e) {
    echo "✓ Nested function error caught: " . $e->getMessage() . "\n";
    echo "  Stack depth: " . count($e->getTrace()) . " frames\n";
    usleep(200000);
}
echo "\n";

// Test 4: Error During SQL Operation
echo "=== Test 4: Error During SQL Operation ===\n";
if (extension_loaded('mysqli') || extension_loaded('pdo')) {
    // Simulate SQL error
    trackError(
        'SQLError',
        'SQL error: Table \'test_db.nonexistent_table\' doesn\'t exist (1146)',
        __FILE__,
        __LINE__
    );
    echo "✓ SQL error tracked\n";
    usleep(200000);
} else {
    echo "⚠ MySQL extensions not loaded, simulating SQL error\n";
    trackError(
        'SQLError',
        'SQL error: Connection failed - Access denied for user',
        __FILE__,
        __LINE__
    );
    echo "✓ SQL error simulated and tracked\n";
    usleep(200000);
}
echo "\n";

// Test 5: Error During cURL Operation
echo "=== Test 5: Error During cURL Operation ===\n";
if (function_exists('curl_init')) {
    // Simulate cURL error
    trackError(
        'CurlError',
        'cURL error: Failed to connect to example.com port 80: Connection refused',
        __FILE__,
        __LINE__
    );
    echo "✓ cURL error tracked\n";
    usleep(200000);
} else {
    echo "⚠ cURL extension not loaded, simulating cURL error\n";
    trackError(
        'CurlError',
        'cURL error: Could not resolve host: invalid-domain-xyz.test',
        __FILE__,
        __LINE__
    );
    echo "✓ cURL error simulated and tracked\n";
    usleep(200000);
}
echo "\n";

// Test 6: Error in Static Method
echo "=== Test 6: Error in Static Method ===\n";
class Utility {
    public static function process($input) {
        if ($input === null) {
            trackError(
                'StaticMethodError',
                'Utility::process() - input cannot be null',
                __FILE__,
                __LINE__
            );
            throw new InvalidArgumentException('Input cannot be null');
        }
        return $input;
    }
}

try {
    Utility::process(null);
} catch (InvalidArgumentException $e) {
    echo "✓ Static method error caught: " . $e->getMessage() . "\n";
    usleep(200000);
}
echo "\n";

// Test 7: Error in Closure/Anonymous Function
echo "=== Test 7: Error in Closure ===\n";
$closure = function($value) {
    if (!is_numeric($value)) {
        trackError(
            'ClosureError',
            'Closure error - value must be numeric',
            __FILE__,
            __LINE__
        );
        throw new TypeError('Value must be numeric');
    }
    return $value * 2;
};

try {
    $closure('not-a-number');
} catch (Exception $e) {
    echo "✓ Closure error caught: " . $e->getMessage() . "\n";
    usleep(200000);
}
echo "\n";

// Test 8: Error in Namespace (simulated)
echo "=== Test 8: Error in Namespace ===\n";
// Simulate namespace error without actual namespace declaration
function testNamespaceFunction() {
    if (function_exists('opa_track_error')) {
        opa_track_error(
            'NamespaceError',
            'Error in TestNamespace\\namespaceFunction() - simulated namespace error',
            __FILE__,
            __LINE__,
            debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS)
        );
    }
}

testNamespaceFunction();
echo "✓ Namespace error test completed\n";
usleep(200000);
echo "\n";

// Test 9: Error with File Context
echo "=== Test 9: Error with File Context ===\n";
$testFile = '/tmp/test_file_xyz.php';
trackError(
    'FileError',
    "File error: Failed to open stream: No such file or directory in {$testFile}",
    $testFile,
    15
);
echo "✓ File context error tracked\n";
usleep(200000);
echo "\n";

// Test 10: Error in Try-Catch-Finally
echo "=== Test 10: Error in Try-Catch-Finally ===\n";
try {
    throw new RuntimeException('Error in try block');
} catch (RuntimeException $e) {
    trackError(
        'TryCatchError',
        'Error caught in catch block: ' . $e->getMessage(),
        __FILE__,
        __LINE__
    );
    echo "✓ Try-catch error tracked\n";
    usleep(200000);
} finally {
    // Finally block executed
    echo "  Finally block executed\n";
}
echo "\n";

// Test 11: Error in Recursive Function
echo "=== Test 11: Error in Recursive Function ===\n";
function recursiveFunction($depth, $maxDepth = 5) {
    if ($depth >= $maxDepth) {
        trackError(
            'RecursiveError',
            "Error in recursive function at depth {$depth}",
            __FILE__,
            __LINE__
        );
        throw new RuntimeException("Maximum recursion depth reached: {$depth}");
    }
    return recursiveFunction($depth + 1, $maxDepth);
}

try {
    recursiveFunction(0);
} catch (RuntimeException $e) {
    echo "✓ Recursive function error caught: " . $e->getMessage() . "\n";
    usleep(200000);
}
echo "\n";

// Summary
echo "=== Test Summary ===\n";
echo "Context error tests completed.\n";
echo "Check the dashboard at /errors to verify:\n";
echo "  - Errors in functions are captured with correct context\n";
echo "  - Errors in class methods are captured\n";
echo "  - Errors in nested calls preserve stack traces\n";
echo "  - Errors during SQL operations are captured\n";
echo "  - Errors during cURL operations are captured\n";
echo "  - Errors in static methods are captured\n";
echo "  - Errors in closures are captured\n";
echo "  - Errors in namespaces are captured\n";
echo "  - File context is preserved in errors\n";
echo "  - Stack traces show correct call hierarchy\n";
echo "\n";
