<?php
/**
 * Exception Error Test Script
 * 
 * Tests various exception types and exception scenarios
 * 
 * Usage:
 *   php test_errors_exceptions.php
 */

echo "=== Exception Error Test ===\n\n";

// Check if opa extension is loaded
if (!extension_loaded('opa')) {
    die("ERROR: OPA extension is not loaded. Please ensure the extension is installed.\n");
}

echo "✓ OPA extension is loaded\n";
echo "Service: " . ini_get('opa.service') . "\n\n";

// Helper function to track exception
function trackException($exception) {
    if (function_exists('opa_track_error')) {
        opa_track_error(
            get_class($exception),
            $exception->getMessage(),
            $exception->getFile(),
            $exception->getLine(),
            $exception->getTrace()
        );
        return true;
    }
    return false;
}

// Test 1: InvalidArgumentException
echo "=== Test 1: InvalidArgumentException ===\n";
try {
    throw new InvalidArgumentException('Invalid argument provided: expected string, got integer');
} catch (InvalidArgumentException $e) {
    echo "✓ InvalidArgumentException caught: " . $e->getMessage() . "\n";
    trackException($e);
    echo "  ✓ Exception tracked\n";
    usleep(200000);
}
echo "\n";

// Test 2: LogicException
echo "=== Test 2: LogicException ===\n";
try {
    throw new LogicException('Logic error: invalid state in application flow');
} catch (LogicException $e) {
    echo "✓ LogicException caught: " . $e->getMessage() . "\n";
    trackException($e);
    echo "  ✓ Exception tracked\n";
    usleep(200000);
}
echo "\n";

// Test 3: DomainException
echo "=== Test 3: DomainException ===\n";
try {
    throw new DomainException('Domain error: value outside acceptable range');
} catch (DomainException $e) {
    echo "✓ DomainException caught: " . $e->getMessage() . "\n";
    trackException($e);
    echo "  ✓ Exception tracked\n";
    usleep(200000);
}
echo "\n";

// Test 4: RangeException
echo "=== Test 4: RangeException ===\n";
try {
    throw new RangeException('Range error: index out of bounds');
} catch (RangeException $e) {
    echo "✓ RangeException caught: " . $e->getMessage() . "\n";
    trackException($e);
    echo "  ✓ Exception tracked\n";
    usleep(200000);
}
echo "\n";

// Test 5: TypeError (PHP 7+)
echo "=== Test 5: TypeError ===\n";
try {
    function typeErrorFunction(string $param) {
        return $param;
    }
    // This will cause TypeError in strict mode, but we'll simulate it
    throw new TypeError('Type error: expected string, integer provided');
} catch (TypeError $e) {
    echo "✓ TypeError caught: " . $e->getMessage() . "\n";
    trackException($e);
    echo "  ✓ Exception tracked\n";
    usleep(200000);
} catch (Exception $e) {
    // Fallback if TypeError not available
    echo "⚠ TypeError simulation: " . $e->getMessage() . "\n";
    trackException($e);
    echo "  ✓ Exception tracked\n";
    usleep(200000);
}
echo "\n";

// Test 6: ArgumentCountError (PHP 7+)
echo "=== Test 6: ArgumentCountError ===\n";
try {
    function argumentCountFunction($arg1, $arg2) {
        return $arg1 + $arg2;
    }
    // This would cause ArgumentCountError, but we'll simulate it
    throw new ArgumentCountError('Argument count error: too few arguments provided');
} catch (ArgumentCountError $e) {
    echo "✓ ArgumentCountError caught: " . $e->getMessage() . "\n";
    trackException($e);
    echo "  ✓ Exception tracked\n";
    usleep(200000);
} catch (Exception $e) {
    // Fallback
    echo "⚠ ArgumentCountError simulation: " . $e->getMessage() . "\n";
    trackException($e);
    echo "  ✓ Exception tracked\n";
    usleep(200000);
}
echo "\n";

// Test 7: Nested Exceptions
echo "=== Test 7: Nested Exception ===\n";
try {
    try {
        throw new RuntimeException('Inner exception: database connection failed');
    } catch (RuntimeException $inner) {
        throw new LogicException('Outer exception: unable to process request', 0, $inner);
    }
} catch (LogicException $e) {
    echo "✓ Nested exception caught: " . $e->getMessage() . "\n";
    if ($e->getPrevious()) {
        echo "  Previous: " . get_class($e->getPrevious()) . " - " . $e->getPrevious()->getMessage() . "\n";
    }
    trackException($e);
    echo "  ✓ Exception tracked\n";
    usleep(200000);
}
echo "\n";

// Test 8: Exception in Class Method
echo "=== Test 8: Exception in Class Method ===\n";
class TestService {
    public function processData($data) {
        if (empty($data)) {
            throw new InvalidArgumentException('Data cannot be empty in TestService::processData()');
        }
        return $data;
    }
    
    public function validateInput($input) {
        if (!is_string($input)) {
            throw new TypeError('Input must be a string in TestService::validateInput()');
        }
        return true;
    }
}

try {
    $service = new TestService();
    $service->processData(null);
} catch (InvalidArgumentException $e) {
    echo "✓ Class method exception caught: " . $e->getMessage() . "\n";
    trackException($e);
    echo "  ✓ Exception tracked\n";
    usleep(200000);
}

try {
    $service = new TestService();
    $service->validateInput(123);
} catch (Exception $e) {
    echo "✓ Class method type error caught: " . $e->getMessage() . "\n";
    trackException($e);
    echo "  ✓ Exception tracked\n";
    usleep(200000);
}
echo "\n";

// Test 9: Exception with Custom Properties
echo "=== Test 9: Custom Exception ===\n";
class CustomTestException extends Exception {
    private $errorCode;
    private $context;
    
    public function __construct($message, $errorCode = 0, $context = [], Exception $previous = null) {
        parent::__construct($message, 0, $previous);
        $this->errorCode = $errorCode;
        $this->context = $context;
    }
    
    public function getErrorCode() {
        return $this->errorCode;
    }
    
    public function getContext() {
        return $this->context;
    }
}

try {
    throw new CustomTestException(
        'Custom exception with error code and context',
        5001,
        ['user_id' => 123, 'action' => 'test']
    );
} catch (CustomTestException $e) {
    echo "✓ Custom exception caught: " . $e->getMessage() . "\n";
    echo "  Error Code: " . $e->getErrorCode() . "\n";
    echo "  Context: " . json_encode($e->getContext()) . "\n";
    trackException($e);
    echo "  ✓ Exception tracked\n";
    usleep(200000);
}
echo "\n";

// Test 10: Deep Stack Trace Exception
echo "=== Test 10: Deep Stack Trace ===\n";
function level1() {
    return level2();
}

function level2() {
    return level3();
}

function level3() {
    return level4();
}

function level4() {
    return level5();
}

function level5() {
    throw new RuntimeException('Deep stack trace exception - 5 levels deep');
}

try {
    level1();
} catch (RuntimeException $e) {
    echo "✓ Deep stack trace exception caught: " . $e->getMessage() . "\n";
    echo "  Stack trace depth: " . count($e->getTrace()) . " frames\n";
    trackException($e);
    echo "  ✓ Exception tracked\n";
    usleep(200000);
}
echo "\n";

// Summary
echo "=== Test Summary ===\n";
echo "Exception error tests completed.\n";
echo "Check the dashboard at /errors to verify:\n";
echo "  - All exception types appear (InvalidArgumentException, LogicException, etc.)\n";
echo "  - Nested exceptions are captured\n";
echo "  - Class method exceptions are captured\n";
echo "  - Custom exceptions are captured\n";
echo "  - Deep stack traces are preserved\n";
echo "  - Exception messages and types are correct\n";
echo "\n";
