<?php
/**
 * Comprehensive Error Context Test Script
 * 
 * Tests errors with full context: HTTP request, user context, environment, release,
 * SQL queries, HTTP requests, and exception codes
 * 
 * Usage:
 *   php test_errors_context_comprehensive.php
 */

echo "=== Comprehensive Error Context Test ===\n\n";

// Check if opa extension is loaded
if (!extension_loaded('opa')) {
    die("ERROR: OPA extension is not loaded. Please ensure the extension is installed.\n");
}

echo "✓ OPA extension is loaded\n";
echo "Service: " . ini_get('opa.service') . "\n\n";

// Set up environment variables for testing
putenv('APP_ENV=testing');
putenv('APP_VERSION=1.2.3');

// Set up HTTP request context (simulate web request)
$_SERVER['REQUEST_METHOD'] = 'POST';
$_SERVER['REQUEST_URI'] = '/api/users/create';
$_SERVER['QUERY_STRING'] = 'id=123&name=test';
$_SERVER['REMOTE_ADDR'] = '192.168.1.100';
$_SERVER['HTTP_USER_AGENT'] = 'Mozilla/5.0 TestAgent/1.0';
$_SERVER['HTTP_HOST'] = 'example.com';
$_SERVER['HTTPS'] = 'on';

// Set up session with user context
if (session_status() === PHP_SESSION_NONE) {
    session_start();
}
$_SESSION['user_id'] = 42;
$_SESSION['username'] = 'testuser';
$_SESSION['email'] = 'test@example.com';

echo "✓ Test environment configured:\n";
echo "  - Environment: " . getenv('APP_ENV') . "\n";
echo "  - Version: " . getenv('APP_VERSION') . "\n";
echo "  - HTTP Method: " . $_SERVER['REQUEST_METHOD'] . "\n";
echo "  - HTTP URI: " . $_SERVER['REQUEST_URI'] . "\n";
echo "  - User ID: " . $_SESSION['user_id'] . "\n";
echo "  - Username: " . $_SESSION['username'] . "\n\n";

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

// Test 1: Error with HTTP Request Context
echo "=== Test 1: Error with HTTP Request Context ===\n";
trackError(
    'HttpRequestError',
    'Error occurred during HTTP POST request to /api/users/create',
    __FILE__,
    __LINE__
);
echo "✓ HTTP request context error tracked\n";
usleep(200000);
echo "\n";

// Test 2: Error with User Context
echo "=== Test 2: Error with User Context ===\n";
trackError(
    'UserContextError',
    'Error occurred for user ' . $_SESSION['username'],
    __FILE__,
    __LINE__
);
echo "✓ User context error tracked\n";
usleep(200000);
echo "\n";

// Test 3: Error with Exception Code
echo "=== Test 3: Error with Exception Code ===\n";
try {
    throw new Exception('Test exception with code', 500);
} catch (Exception $e) {
    echo "✓ Exception caught with code: " . $e->getCode() . "\n";
    if (function_exists('opa_track_error')) {
        opa_track_error(
            get_class($e),
            $e->getMessage(),
            $e->getFile(),
            $e->getLine(),
            $e->getTrace()
        );
        echo "  ✓ Exception with code tracked\n";
    }
    usleep(200000);
}
echo "\n";

// Test 4: Error After SQL Query (simulated)
echo "=== Test 4: Error After SQL Query ===\n";
// Simulate SQL query execution
if (extension_loaded('mysqli') || extension_loaded('pdo')) {
    // If MySQL is available, try a real query
    try {
        $host = getenv('MYSQL_HOST') ?: 'mysql-test';
        $user = getenv('MYSQL_USER') ?: 'test_user';
        $pass = getenv('MYSQL_PASSWORD') ?: 'test_password';
        $db = getenv('MYSQL_DATABASE') ?: 'test_db';
        
        if (extension_loaded('mysqli')) {
            $mysqli = @new mysqli($host, $user, $pass, $db);
            if (!$mysqli->connect_error) {
                $mysqli->query("SELECT 1");
                $mysqli->close();
            }
        }
    } catch (Exception $e) {
        // Ignore connection errors
    }
}

trackError(
    'SQLContextError',
    'Error occurred after executing SQL query: SELECT * FROM users WHERE id = 123',
    __FILE__,
    __LINE__
);
echo "✓ SQL context error tracked\n";
usleep(200000);
echo "\n";

// Test 5: Error After HTTP Request (cURL)
echo "=== Test 5: Error After HTTP Request (cURL) ===\n";
if (function_exists('curl_init')) {
    $ch = curl_init('http://example.com/api');
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_TIMEOUT, 1);
    @curl_exec($ch); // Suppress errors
    curl_close($ch);
}

trackError(
    'HttpRequestContextError',
    'Error occurred after making HTTP request to external API',
    __FILE__,
    __LINE__
);
echo "✓ HTTP request context error tracked\n";
usleep(200000);
echo "\n";

// Test 6: Error with Environment and Release
echo "=== Test 6: Error with Environment and Release ===\n";
trackError(
    'EnvironmentError',
    'Error in ' . getenv('APP_ENV') . ' environment, version ' . getenv('APP_VERSION'),
    __FILE__,
    __LINE__
);
echo "✓ Environment context error tracked\n";
usleep(200000);
echo "\n";

// Test 7: Multiple Contexts Combined
echo "=== Test 7: Error with Multiple Contexts ===\n";
// Simulate complex scenario: user action, SQL query, then error
if (extension_loaded('mysqli')) {
    try {
        $host = getenv('MYSQL_HOST') ?: 'mysql-test';
        $user = getenv('MYSQL_USER') ?: 'test_user';
        $pass = getenv('MYSQL_PASSWORD') ?: 'test_password';
        $db = getenv('MYSQL_DATABASE') ?: 'test_db';
        
        $mysqli = @new mysqli($host, $user, $pass, $db);
        if (!$mysqli->connect_error) {
            $mysqli->query("SELECT COUNT(*) FROM users");
            $mysqli->close();
        }
    } catch (Exception $e) {
        // Ignore
    }
}

trackError(
    'MultiContextError',
    'Complex error: User ' . $_SESSION['username'] . ' (ID: ' . $_SESSION['user_id'] . ') encountered error during ' . $_SERVER['REQUEST_METHOD'] . ' ' . $_SERVER['REQUEST_URI'],
    __FILE__,
    __LINE__
);
echo "✓ Multi-context error tracked\n";
usleep(200000);
echo "\n";

// Test 8: Exception with Code in Different Contexts
echo "=== Test 8: Exceptions with Codes ===\n";
$exceptionCodes = [404, 500, 403, 401];
foreach ($exceptionCodes as $code) {
    try {
        throw new Exception("Test exception with code $code", $code);
    } catch (Exception $e) {
        if (function_exists('opa_track_error')) {
            opa_track_error(
                get_class($e),
                $e->getMessage(),
                $e->getFile(),
                $e->getLine(),
                $e->getTrace()
            );
        }
        echo "  ✓ Exception with code $code tracked\n";
        usleep(100000);
    }
}
echo "\n";

// Test 9: Error in Different Environments
echo "=== Test 9: Errors in Different Environments ===\n";
$environments = ['development', 'staging', 'production'];
foreach ($environments as $env) {
    putenv("APP_ENV=$env");
    trackError(
        'EnvironmentSpecificError',
        "Error in $env environment",
        __FILE__,
        __LINE__
    );
    echo "  ✓ Error in $env environment tracked\n";
    usleep(100000);
}
// Restore original
putenv('APP_ENV=testing');
echo "\n";

// Test 10: Error with Release Versions
echo "=== Test 10: Errors with Different Release Versions ===\n";
$versions = ['1.0.0', '1.1.0', '2.0.0'];
foreach ($versions as $version) {
    putenv("APP_VERSION=$version");
    trackError(
        'VersionSpecificError',
        "Error in version $version",
        __FILE__,
        __LINE__
    );
    echo "  ✓ Error in version $version tracked\n";
    usleep(100000);
}
// Restore original
putenv('APP_VERSION=1.2.3');
echo "\n";

// Summary
echo "=== Test Summary ===\n";
echo "Comprehensive error context tests completed.\n";
echo "Check the dashboard at /errors to verify:\n";
echo "  - HTTP request context (method, URI, user agent, IP) is captured\n";
echo "  - User context (user_id, username, session_id) is captured\n";
echo "  - Environment and release version are captured\n";
echo "  - Exception codes are captured\n";
echo "  - SQL queries executed before error are captured\n";
echo "  - HTTP requests made before error are captured\n";
echo "  - All context fields appear in error_instances table\n";
echo "\n";
