<?php
/**
 * Web-Accessible Error Test Script
 * 
 * Generates different error types based on query parameters
 * Can be accessed via HTTP: http://localhost/test_errors_web.php?type=exception
 * 
 * Usage:
 *   - Via browser: http://localhost/test_errors_web.php?type=exception
 *   - Via curl: curl "http://localhost/test_errors_web.php?type=user_error"
 */

// Set content type for web output
header('Content-Type: text/html; charset=utf-8');

// Check if opa extension is loaded
if (!extension_loaded('opa')) {
    http_response_code(500);
    die("ERROR: OPA extension is not loaded.\n");
}

// Get error type from query parameter
$errorType = $_GET['type'] ?? 'list';
$service = ini_get('opa.service') ?? 'web-test';

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

?>
<!DOCTYPE html>
<html>
<head>
    <title>Error Test Generator</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 20px; background: #f5f5f5; }
        .container { max-width: 800px; margin: 0 auto; background: white; padding: 20px; border-radius: 8px; }
        h1 { color: #333; }
        .success { color: green; padding: 10px; background: #e8f5e9; border-radius: 4px; margin: 10px 0; }
        .error { color: red; padding: 10px; background: #ffebee; border-radius: 4px; margin: 10px 0; }
        .info { color: #2196F3; padding: 10px; background: #e3f2fd; border-radius: 4px; margin: 10px 0; }
        .links { margin: 20px 0; }
        .links a { display: inline-block; margin: 5px 10px 5px 0; padding: 8px 15px; background: #2196F3; color: white; text-decoration: none; border-radius: 4px; }
        .links a:hover { background: #1976D2; }
        code { background: #f5f5f5; padding: 2px 6px; border-radius: 3px; }
    </style>
</head>
<body>
    <div class="container">
        <h1>Error Test Generator</h1>
        <p>Service: <code><?php echo htmlspecialchars($service); ?></code></p>
        
        <div class="links">
            <a href="?type=list">List All Tests</a>
            <a href="?type=user_error">User Error</a>
            <a href="?type=user_warning">User Warning</a>
            <a href="?type=exception">Exception</a>
            <a href="?type=runtime_exception">RuntimeException</a>
            <a href="?type=invalid_argument">InvalidArgumentException</a>
            <a href="?type=type_error">TypeError</a>
            <a href="?type=sql_error">SQL Error</a>
            <a href="?type=curl_error">cURL Error</a>
            <a href="?type=nested">Nested Exception</a>
        </div>

<?php

// Handle different error types
switch ($errorType) {
    case 'list':
        echo '<div class="info"><h2>Available Error Tests</h2><ul>';
        echo '<li><a href="?type=user_error">User Error</a> - E_USER_ERROR</li>';
        echo '<li><a href="?type=user_warning">User Warning</a> - E_USER_WARNING</li>';
        echo '<li><a href="?type=exception">Exception</a> - Standard Exception</li>';
        echo '<li><a href="?type=runtime_exception">RuntimeException</a> - Runtime Exception</li>';
        echo '<li><a href="?type=invalid_argument">InvalidArgumentException</a> - Invalid Argument</li>';
        echo '<li><a href="?type=logic_exception">LogicException</a> - Logic Exception</li>';
        echo '<li><a href="?type=type_error">TypeError</a> - Type Error</li>';
        echo '<li><a href="?type=sql_error">SQL Error</a> - Database Error</li>';
        echo '<li><a href="?type=curl_error">cURL Error</a> - HTTP Request Error</li>';
        echo '<li><a href="?type=nested">Nested Exception</a> - Exception with Previous</li>';
        echo '<li><a href="?type=function_error">Function Error</a> - Error in Function</li>';
        echo '<li><a href="?type=class_error">Class Error</a> - Error in Class Method</li>';
        echo '</ul></div>';
        break;
        
    case 'user_error':
        if (ini_get('opa.track_errors')) {
            trigger_error('Web test: This is a E_USER_ERROR generated via web interface', E_USER_ERROR);
            echo '<div class="success">✓ E_USER_ERROR generated and tracked</div>';
        } else {
            echo '<div class="error">✗ Error tracking is disabled (opa.track_errors=0)</div>';
        }
        break;
        
    case 'user_warning':
        if (ini_get('opa.track_errors')) {
            trigger_error('Web test: This is a E_USER_WARNING generated via web interface', E_USER_WARNING);
            echo '<div class="success">✓ E_USER_WARNING generated and tracked</div>';
        } else {
            echo '<div class="error">✗ Error tracking is disabled</div>';
        }
        break;
        
    case 'exception':
        try {
            throw new Exception('Web test: Standard Exception generated via web interface');
        } catch (Exception $e) {
            trackError(get_class($e), $e->getMessage(), $e->getFile(), $e->getLine(), $e->getTrace());
            echo '<div class="success">✓ Exception generated and tracked: ' . htmlspecialchars($e->getMessage()) . '</div>';
        }
        break;
        
    case 'runtime_exception':
        try {
            throw new RuntimeException('Web test: RuntimeException generated via web interface');
        } catch (RuntimeException $e) {
            trackError(get_class($e), $e->getMessage(), $e->getFile(), $e->getLine(), $e->getTrace());
            echo '<div class="success">✓ RuntimeException generated and tracked: ' . htmlspecialchars($e->getMessage()) . '</div>';
        }
        break;
        
    case 'invalid_argument':
        try {
            throw new InvalidArgumentException('Web test: InvalidArgumentException - invalid parameter provided');
        } catch (InvalidArgumentException $e) {
            trackError(get_class($e), $e->getMessage(), $e->getFile(), $e->getLine(), $e->getTrace());
            echo '<div class="success">✓ InvalidArgumentException generated and tracked: ' . htmlspecialchars($e->getMessage()) . '</div>';
        }
        break;
        
    case 'logic_exception':
        try {
            throw new LogicException('Web test: LogicException - invalid application state');
        } catch (LogicException $e) {
            trackError(get_class($e), $e->getMessage(), $e->getFile(), $e->getLine(), $e->getTrace());
            echo '<div class="success">✓ LogicException generated and tracked: ' . htmlspecialchars($e->getMessage()) . '</div>';
        }
        break;
        
    case 'type_error':
        try {
            throw new TypeError('Web test: TypeError - expected string, integer provided');
        } catch (TypeError $e) {
            trackError(get_class($e), $e->getMessage(), $e->getFile(), $e->getLine(), $e->getTrace());
            echo '<div class="success">✓ TypeError generated and tracked: ' . htmlspecialchars($e->getMessage()) . '</div>';
        } catch (Exception $e) {
            trackError('TypeError', 'Web test: TypeError simulation - expected string, integer provided', __FILE__, __LINE__);
            echo '<div class="success">✓ TypeError simulated and tracked</div>';
        }
        break;
        
    case 'sql_error':
        trackError(
            'SQLError',
            'Web test: SQL error - Table \'test_db.users\' doesn\'t exist (1146)',
            __FILE__,
            __LINE__
        );
        echo '<div class="success">✓ SQL error tracked</div>';
        break;
        
    case 'curl_error':
        trackError(
            'CurlError',
            'Web test: cURL error - Failed to connect to api.example.com port 443: Connection timed out',
            __FILE__,
            __LINE__
        );
        echo '<div class="success">✓ cURL error tracked</div>';
        break;
        
    case 'nested':
        try {
            try {
                throw new RuntimeException('Web test: Inner exception - database connection failed');
            } catch (RuntimeException $inner) {
                throw new LogicException('Web test: Outer exception - unable to process request', 0, $inner);
            }
        } catch (LogicException $e) {
            trackError(get_class($e), $e->getMessage(), $e->getFile(), $e->getLine(), $e->getTrace());
            echo '<div class="success">✓ Nested exception generated and tracked</div>';
            if ($e->getPrevious()) {
                echo '<div class="info">Previous exception: ' . htmlspecialchars(get_class($e->getPrevious())) . ' - ' . htmlspecialchars($e->getPrevious()->getMessage()) . '</div>';
            }
        }
        break;
        
    case 'function_error':
        function webTestFunction() {
            trackError(
                'FunctionError',
                'Web test: Error in webTestFunction() - invalid operation',
                __FILE__,
                __LINE__
            );
        }
        webTestFunction();
        echo '<div class="success">✓ Function error tracked</div>';
        break;
        
    case 'class_error':
        class WebTestService {
            public function process() {
                trackError(
                    'ClassMethodError',
                    'Web test: WebTestService::process() - validation failed',
                    __FILE__,
                    __LINE__
                );
                throw new RuntimeException('Processing failed');
            }
        }
        try {
            $service = new WebTestService();
            $service->process();
        } catch (RuntimeException $e) {
            echo '<div class="success">✓ Class method error tracked: ' . htmlspecialchars($e->getMessage()) . '</div>';
        }
        break;
        
    default:
        echo '<div class="error">Unknown error type: ' . htmlspecialchars($errorType) . '</div>';
        echo '<div class="info">Use ?type=list to see available tests</div>';
}

// Wait a bit for processing
usleep(300000);

?>

        <div class="info">
            <h3>Next Steps</h3>
            <p>Check the dashboard at <code>/errors</code> to verify the error was captured.</p>
            <p>Service name: <code><?php echo htmlspecialchars($service); ?></code></p>
        </div>
    </div>
</body>
</html>
