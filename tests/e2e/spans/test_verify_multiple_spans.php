<?php
/**
 * Verification Test for Multiple Spans
 * 
 * This test generates a trace with a proper call stack and SQL queries
 * to verify that multiple spans are created when expand_spans=1
 */

$api_url = getenv('API_URL') ?: 'http://localhost:8081';
$mysql_host = getenv('MYSQL_HOST') ?: 'mysql-test';
$mysql_port = getenv('MYSQL_PORT') ?: 3306;
$mysql_database = getenv('MYSQL_DATABASE') ?: 'test_db';
$mysql_user = getenv('MYSQL_USER') ?: 'test_user';
$mysql_password = getenv('MYSQL_PASSWORD') ?: 'test_password';
$mysql_root_password = getenv('MYSQL_ROOT_PASSWORD') ?: 'root_password';

echo "=== Multiple Spans Verification Test ===\n\n";

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
echo "✓ opa.expand_spans = " . ini_get('opa.expand_spans') . "\n\n";

// Create a class with methods to generate call hierarchy
class UserService {
    private $conn;
    
    public function __construct($conn) {
        $this->conn = $conn;
    }
    
    public function getUser($id) {
        // SQL query that should create a child span
        $stmt = $this->conn->prepare("SELECT * FROM test_users WHERE id = ?");
        $stmt->bind_param("i", $id);
        $stmt->execute();
        $result = $stmt->get_result();
        $user = $result->fetch_assoc();
        $stmt->close();
        
        // Long operation (>10ms) that should create a span
        usleep(15000); // 15ms
        
        return $user;
    }
    
    public function getUsers() {
        // Another SQL query
        $result = $this->conn->query("SELECT * FROM test_users LIMIT 10");
        $users = [];
        while ($row = $result->fetch_assoc()) {
            $users[] = $row;
        }
        $result->close();
        return $users;
    }
}

// Connect to database
$conn = new mysqli($mysql_host, $mysql_user, $mysql_password, $mysql_database, $mysql_port);

if ($conn->connect_error) {
    die("Connection failed: " . $conn->connect_error . "\n");
}

// Create test table
$conn->query("CREATE TABLE IF NOT EXISTS test_users (
    id INT AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    email VARCHAR(100) NOT NULL
)");

// Insert test data
$conn->query("INSERT IGNORE INTO test_users (name, email) VALUES 
    ('John Doe', 'john@example.com'),
    ('Jane Smith', 'jane@example.com')");

echo "Executing operations that should create multiple spans...\n";

// Create service and call methods (creates call hierarchy)
$service = new UserService($conn);
$users = $service->getUsers(); // Should create a span with SQL
$user = $service->getUser(1);   // Should create another span with SQL + long operation

// Additional SQL query
$conn->query("SELECT COUNT(*) as total FROM test_users");

$conn->close();

echo "Operations completed!\n\n";

// Wait for span to be processed
echo "Waiting for span to be processed (5 seconds)...\n";
sleep(5);

// Get latest trace
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

// Validate
echo "=== Validation Results ===\n\n";
$span_count = count($trace['spans'] ?? []);
echo "Total spans: $span_count\n";

$root_span = $trace['root'] ?? null;
if (!$root_span) {
    die("ERROR: No root span found\n");
}

$expand_spans = $root_span['tags']['expand_spans'] ?? null;
$stack_size = count($root_span['stack'] ?? []);
$sql_count = count($root_span['sql'] ?? []);
$children_count = count($root_span['children'] ?? []);

echo "expand_spans tag: " . var_export($expand_spans, true) . "\n";
echo "Call stack size: $stack_size\n";
echo "SQL queries in root: $sql_count\n";
echo "Child spans: $children_count\n\n";

// Check if we have multiple spans
if ($span_count > 1) {
    echo "✓ SUCCESS: Multiple spans created!\n";
    echo "\nSpan breakdown:\n";
    foreach ($trace['spans'] as $span) {
        $name = $span['name'] ?? 'unknown';
        $parent = $span['parent_id'] ?? 'root';
        $sql = count($span['sql'] ?? []);
        $children = count($span['children'] ?? []);
        echo "  - $name (parent: $parent, SQL: $sql, children: $children)\n";
    }
    exit(0);
} else {
    echo "✗ FAILED: Only 1 span found\n";
    if ($stack_size == 0) {
        echo "  Reason: Call stack is empty (nothing to expand)\n";
        echo "  This may be normal if the collector didn't capture function calls\n";
    } else {
        echo "  Reason: Call stack exists but expansion didn't work\n";
    }
    exit(1);
}
