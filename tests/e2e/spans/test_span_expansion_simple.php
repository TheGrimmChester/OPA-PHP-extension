<?php
/**
 * Simple Span Expansion Test
 * 
 * Creates a trace with SQL query and verifies expand_spans tag is present
 */

$api_url = getenv('API_URL') ?: 'http://localhost:8081';
$mysql_host = getenv('MYSQL_HOST') ?: 'mysql-test';
$mysql_port = getenv('MYSQL_PORT') ?: 3306;
$mysql_database = getenv('MYSQL_DATABASE') ?: 'test_db';
$mysql_user = getenv('MYSQL_USER') ?: 'test_user';
$mysql_password = getenv('MYSQL_PASSWORD') ?: 'test_password';
$mysql_root_password = getenv('MYSQL_ROOT_PASSWORD') ?: 'root_password';

echo "=== Simple Span Expansion Test ===\n\n";

// Wait for MySQL
$max_attempts = 30;
$attempt = 0;
$connected = false;

while ($attempt < $max_attempts && !$connected) {
    try {
        $test_conn = @new mysqli($mysql_host, 'root', $mysql_root_password, '', $mysql_port);
        if (!$test_conn->connect_error) {
            $connected = true;
            $test_conn->close();
        }
    } catch (Exception $e) {
        // Continue
    }
    if (!$connected) {
        sleep(1);
        $attempt++;
    }
}

if (!$connected) {
    die("ERROR: Could not connect to MySQL\n");
}

// Set expand_spans to 1
ini_set('opa.expand_spans', '1');
echo "Set opa.expand_spans = 1\n";
echo "Current value: " . ini_get('opa.expand_spans') . "\n\n";

// Create a unique identifier for this test
$test_id = uniqid('test_', true);
echo "Test ID: $test_id\n\n";

// Execute SQL query to generate a span
echo "Executing SQL query...\n";
$conn = new mysqli($mysql_host, $mysql_user, $mysql_password, $mysql_database, $mysql_port);
if (!$conn->connect_error) {
    $conn->query("SELECT '$test_id' as test_id, NOW() as timestamp");
    $conn->close();
    echo "SQL query executed\n";
}

// Wait for span to be sent
echo "Waiting for span to be processed (5 seconds)...\n";
sleep(5);

// Get latest trace
echo "Fetching latest trace from API...\n";
$ch = curl_init("$api_url/api/traces?limit=1");
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch, CURLOPT_TIMEOUT, 10);
$response = curl_exec($ch);
$http_code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
curl_close($ch);

if ($http_code != 200) {
    die("ERROR: Failed to get traces (HTTP $http_code)\n");
}

$data = json_decode($response, true);
if (!$data || !isset($data['traces']) || count($data['traces']) == 0) {
    die("ERROR: No traces found\n");
}

$trace_id = $data['traces'][0]['trace_id'];
echo "Found trace ID: $trace_id\n\n";

// Get full trace
echo "Fetching full trace...\n";
$ch = curl_init("$api_url/api/traces/$trace_id");
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch, CURLOPT_TIMEOUT, 10);
$response = curl_exec($ch);
$http_code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
curl_close($ch);

if ($http_code != 200) {
    die("ERROR: Failed to get trace (HTTP $http_code)\n");
}

$trace = json_decode($response, true);
if (!$trace) {
    die("ERROR: Failed to parse trace\n");
}

// Validate trace
echo "=== Validation ===\n\n";
$span_count = count($trace['spans'] ?? []);
echo "Span count: $span_count\n";

$root_span = $trace['root'] ?? null;
if (!$root_span) {
    die("ERROR: No root span found\n");
}

// Check expand_spans tag
$has_expand_spans = isset($root_span['tags']['expand_spans']);
$expand_spans_value = $root_span['tags']['expand_spans'] ?? null;

echo "Has expand_spans tag: " . ($has_expand_spans ? 'yes' : 'no') . "\n";
if ($has_expand_spans) {
    echo "expand_spans value: " . var_export($expand_spans_value, true) . "\n";
}

// Check for call stack
$has_stack = isset($root_span['stack']) && is_array($root_span['stack']) && count($root_span['stack']) > 0;
echo "Has call stack: " . ($has_stack ? 'yes' : 'no') . "\n";
if ($has_stack) {
    echo "Call stack size: " . count($root_span['stack']) . "\n";
}

echo "\n=== Result ===\n";
if ($has_expand_spans && $expand_spans_value === true) {
    echo "✓ expand_spans tag is present and correct\n";
    exit(0);
} else {
    echo "✗ expand_spans tag is missing or incorrect\n";
    if (!$has_expand_spans) {
        echo "  - Tag is missing\n";
    } else {
        echo "  - Value is: " . var_export($expand_spans_value, true) . " (expected: true)\n";
    }
    exit(1);
}
