<?php
/**
 * Span Expansion End-to-End Test
 * 
 * Tests both multiple spans mode (expand_spans=1) and full span mode (expand_spans=0)
 * Verifies that traces have the correct number of spans based on the mode
 */

// Get configuration from environment
$api_url = getenv('API_URL') ?: 'http://localhost:8081';
$mysql_host = getenv('MYSQL_HOST') ?: 'mysql-test';
$mysql_port = getenv('MYSQL_PORT') ?: 3306;
$mysql_database = getenv('MYSQL_DATABASE') ?: 'test_db';
$mysql_user = getenv('MYSQL_USER') ?: 'test_user';
$mysql_password = getenv('MYSQL_PASSWORD') ?: 'test_password';
$mysql_root_password = getenv('MYSQL_ROOT_PASSWORD') ?: 'root_password';

echo "=== Span Expansion E2E Test ===\n\n";

echo "Configuration:\n";
echo "  API URL: $api_url\n";
echo "  MySQL Host: $mysql_host:$mysql_port\n\n";

$test_results = [];

// Helper function to wait for trace to appear in API
function wait_for_trace($api_url, $trace_id, $max_wait = 30) {
    $start_time = time();
    while (time() - $start_time < $max_wait) {
        $ch = curl_init("$api_url/api/traces/$trace_id");
        curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
        curl_setopt($ch, CURLOPT_TIMEOUT, 5);
        $response = curl_exec($ch);
        $http_code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
        curl_close($ch);
        
        if ($http_code == 200) {
            $trace = json_decode($response, true);
            if ($trace && isset($trace['spans']) && count($trace['spans']) > 0) {
                return $trace;
            }
        }
        sleep(1);
    }
    return null;
}

// Helper function to get trace ID from recent traces
function get_recent_trace_id($api_url) {
    $ch = curl_init("$api_url/api/traces?limit=1");
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_TIMEOUT, 5);
    $response = curl_exec($ch);
    $http_code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    curl_close($ch);
    
    if ($http_code == 200) {
        $data = json_decode($response, true);
        if ($data && isset($data['traces']) && count($data['traces']) > 0) {
            return $data['traces'][0]['trace_id'];
        }
    }
    return null;
}

// Wait for MySQL to be ready
echo "Waiting for MySQL to be ready...\n";
$max_attempts = 30;
$attempt = 0;
$connected = false;

while ($attempt < $max_attempts && !$connected) {
    try {
        $test_conn = @new mysqli($mysql_host, 'root', $mysql_root_password, '', $mysql_port);
        if (!$test_conn->connect_error) {
            $connected = true;
            $test_conn->close();
            echo "MySQL is ready!\n\n";
        }
    } catch (Exception $e) {
        // Continue waiting
    }
    if (!$connected) {
        sleep(1);
        $attempt++;
        echo ".";
    }
}

if (!$connected) {
    die("ERROR: Could not connect to MySQL after $max_attempts attempts\n");
}

// ============================================
// Test 1: Multiple Spans Mode (expand_spans=1)
// ============================================
echo "=== Test 1: Multiple Spans Mode (expand_spans=1) ===\n\n";

// Set expand_spans to 1
ini_set('opa.expand_spans', '1');
echo "Set opa.expand_spans = 1\n";

// Create a function that will generate significant operations
function test_multiple_spans() {
    global $mysql_host, $mysql_port, $mysql_database, $mysql_user, $mysql_password;
    
    // SQL query (should create a child span)
    $conn = new mysqli($mysql_host, $mysql_user, $mysql_password, $mysql_database, $mysql_port);
    if (!$conn->connect_error) {
        $conn->query("SELECT 1");
        $conn->close();
    }
    
    // Simulate a long operation (>10ms) that should create a span
    usleep(15000); // 15ms
    
    return "test_complete";
}

// Execute the test function
echo "Executing test function with SQL query and long operation...\n";
$result = test_multiple_spans();
echo "Function result: $result\n";

// Wait a bit for span to be sent
echo "Waiting for span to be processed...\n";
sleep(3);

// Get the trace
$trace_id = get_recent_trace_id($api_url);
if (!$trace_id) {
    echo "ERROR: Could not get trace ID\n";
    $test_results[] = ['status' => false, 'name' => 'Multiple Spans Mode - Get Trace ID'];
} else {
    echo "Found trace ID: $trace_id\n";
    
    // Wait for trace to be fully processed
    $trace = wait_for_trace($api_url, $trace_id);
    
    if (!$trace) {
        echo "ERROR: Could not retrieve trace\n";
        $test_results[] = ['status' => false, 'name' => 'Multiple Spans Mode - Retrieve Trace'];
    } else {
        $span_count = count($trace['spans']);
        echo "Trace has $span_count spans\n";
        
        // In multiple spans mode, we should have more than 1 span
        // (root span + at least one child span from SQL or long operation)
        $has_multiple_spans = $span_count > 1;
        
        // Check if expand_spans tag is present in root span
        $root_span = $trace['root'] ?? null;
        $has_expand_spans_tag = false;
        $expand_spans_value = null;
        
        if ($root_span && isset($root_span['tags']['expand_spans'])) {
            $has_expand_spans_tag = true;
            $expand_spans_value = $root_span['tags']['expand_spans'];
        }
        
        echo "  - Span count: $span_count\n";
        echo "  - Has expand_spans tag: " . ($has_expand_spans_tag ? 'yes' : 'no') . "\n";
        if ($has_expand_spans_tag) {
            echo "  - expand_spans value: " . var_export($expand_spans_value, true) . "\n";
        }
        
        $test_passed = $has_multiple_spans && $has_expand_spans_tag && $expand_spans_value === true;
        $test_results[] = [
            'status' => $test_passed,
            'name' => 'Multiple Spans Mode',
            'details' => "Span count: $span_count, expand_spans tag: " . ($has_expand_spans_tag ? 'present' : 'missing')
        ];
        
        if ($test_passed) {
            echo "✓ Multiple spans mode test PASSED\n";
        } else {
            echo "✗ Multiple spans mode test FAILED\n";
            if (!$has_multiple_spans) {
                echo "  - Expected multiple spans, got $span_count\n";
            }
            if (!$has_expand_spans_tag) {
                echo "  - Missing expand_spans tag in root span\n";
            }
            if ($expand_spans_value !== true) {
                echo "  - expand_spans value is not true: " . var_export($expand_spans_value, true) . "\n";
            }
        }
    }
}

echo "\n";

// ============================================
// Test 2: Full Span Mode (expand_spans=0)
// ============================================
echo "=== Test 2: Full Span Mode (expand_spans=0) ===\n\n";

// Set expand_spans to 0
ini_set('opa.expand_spans', '0');
echo "Set opa.expand_spans = 0\n";

// Execute test function again
echo "Executing test function again...\n";
$result2 = test_multiple_spans();
echo "Function result: $result2\n";

// Wait for span to be sent
echo "Waiting for span to be processed...\n";
sleep(3);

// Get the trace
$trace_id2 = get_recent_trace_id($api_url);
if (!$trace_id2) {
    echo "ERROR: Could not get trace ID\n";
    $test_results[] = ['status' => false, 'name' => 'Full Span Mode - Get Trace ID'];
} else {
    echo "Found trace ID: $trace_id2\n";
    
    // Wait for trace to be fully processed
    $trace2 = wait_for_trace($api_url, $trace_id2);
    
    if (!$trace2) {
        echo "ERROR: Could not retrieve trace\n";
        $test_results[] = ['status' => false, 'name' => 'Full Span Mode - Retrieve Trace'];
    } else {
        $span_count2 = count($trace2['spans']);
        echo "Trace has $span_count2 spans\n";
        
        // In full span mode, we should have exactly 1 span (root span with nested call stack)
        $has_single_span = $span_count2 == 1;
        
        // Check if expand_spans tag is present and false
        $root_span2 = $trace2['root'] ?? null;
        $has_expand_spans_tag2 = false;
        $expand_spans_value2 = null;
        
        if ($root_span2 && isset($root_span2['tags']['expand_spans'])) {
            $has_expand_spans_tag2 = true;
            $expand_spans_value2 = $root_span2['tags']['expand_spans'];
        }
        
        // Check if call stack is present in the root span
        $has_call_stack = false;
        if ($root_span2 && isset($root_span2['stack']) && is_array($root_span2['stack']) && count($root_span2['stack']) > 0) {
            $has_call_stack = true;
        }
        
        echo "  - Span count: $span_count2\n";
        echo "  - Has expand_spans tag: " . ($has_expand_spans_tag2 ? 'yes' : 'no') . "\n";
        if ($has_expand_spans_tag2) {
            echo "  - expand_spans value: " . var_export($expand_spans_value2, true) . "\n";
        }
        echo "  - Has call stack in root span: " . ($has_call_stack ? 'yes' : 'no') . "\n";
        
        $test_passed2 = $has_single_span && $has_expand_spans_tag2 && $expand_spans_value2 === false && $has_call_stack;
        $test_results[] = [
            'status' => $test_passed2,
            'name' => 'Full Span Mode',
            'details' => "Span count: $span_count2, expand_spans: " . var_export($expand_spans_value2, true) . ", call stack: " . ($has_call_stack ? 'present' : 'missing')
        ];
        
        if ($test_passed2) {
            echo "✓ Full span mode test PASSED\n";
        } else {
            echo "✗ Full span mode test FAILED\n";
            if (!$has_single_span) {
                echo "  - Expected 1 span, got $span_count2\n";
            }
            if (!$has_expand_spans_tag2) {
                echo "  - Missing expand_spans tag in root span\n";
            }
            if ($expand_spans_value2 !== false) {
                echo "  - expand_spans value is not false: " . var_export($expand_spans_value2, true) . "\n";
            }
            if (!$has_call_stack) {
                echo "  - Missing call stack in root span\n";
            }
        }
    }
}

echo "\n";

// ============================================
// Test Summary
// ============================================
echo "=== Test Summary ===\n\n";

$passed = 0;
$failed = 0;

foreach ($test_results as $result) {
    if ($result['status']) {
        $passed++;
        echo "✓ " . $result['name'];
        if (isset($result['details'])) {
            echo " - " . $result['details'];
        }
        echo "\n";
    } else {
        $failed++;
        echo "✗ " . $result['name'];
        if (isset($result['details'])) {
            echo " - " . $result['details'];
        }
        echo "\n";
    }
}

echo "\n";
echo "Total: " . count($test_results) . " tests\n";
echo "Passed: $passed\n";
echo "Failed: $failed\n";

if ($failed > 0) {
    exit(1);
} else {
    echo "\n✓ All tests passed!\n";
    exit(0);
}
