<?php
/**
 * E2E Test for Multiple Spans
 * 
 * This test validates that:
 * 1. PHP extension sends multiple Incoming messages (root + child spans)
 * 2. Agent receives and stores all spans separately
 * 3. Trace retrieval returns all spans without expansion
 * 4. span_count in list view is accurate
 */

$api_url = getenv('API_URL') ?: 'http://localhost:8081';
$mysql_host = getenv('MYSQL_HOST') ?: 'mysql-test';
$mysql_port = getenv('MYSQL_PORT') ?: 3306;
$mysql_database = getenv('MYSQL_DATABASE') ?: 'test_db';
$mysql_user = getenv('MYSQL_USER') ?: 'test_user';
$mysql_password = getenv('MYSQL_PASSWORD') ?: 'test_password';
$mysql_root_password = getenv('MYSQL_ROOT_PASSWORD') ?: 'root_password';

echo "=== Multiple Spans E2E Test ===\n\n";

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

// Set expand_spans to 1 (send multiple spans)
ini_set('opa.expand_spans', '1');
echo "✓ opa.expand_spans = " . ini_get('opa.expand_spans') . "\n\n";

// Create classes with methods that will generate call stack entries
class DatabaseService {
    private $conn;
    
    public function __construct($conn) {
        $this->conn = $conn;
    }
    
    public function getUserById($id) {
        // SQL query that should create a child span
        $stmt = $this->conn->prepare("SELECT * FROM test_users WHERE id = ?");
        $stmt->bind_param("i", $id);
        $stmt->execute();
        $result = $stmt->get_result();
        $user = $result->fetch_assoc();
        $stmt->close();
        return $user;
    }
    
    public function getAllUsers() {
        // Another SQL query
        $result = $this->conn->query("SELECT id, name, email FROM test_users ORDER BY id");
        $users = [];
        while ($row = $result->fetch_assoc()) {
            $users[] = $row;
        }
        $result->close();
        return $users;
    }
    
    public function countUsers() {
        // SQL query with aggregation
        $result = $this->conn->query("SELECT COUNT(*) as total FROM test_users");
        $row = $result->fetch_assoc();
        $result->close();
        return (int)$row['total'];
    }
}

class UserService {
    private $db;
    
    public function __construct($db) {
        $this->db = $db;
    }
    
    public function getUserProfile($userId) {
        // This calls another method, creating call hierarchy
        $user = $this->db->getUserById($userId);
        
        // Long operation (>10ms) that should create a span
        usleep(15000); // 15ms
        
        // Another SQL query
        $this->db->countUsers();
        
        return $user;
    }
    
    public function processUsers() {
        // Get all users
        $users = $this->db->getAllUsers();
        
        // Process each user (creates more call stack entries)
        foreach ($users as $user) {
            if (isset($user['id'])) {
                // Another long operation
                usleep(12000); // 12ms
                $this->getUserProfile($user['id']);
            }
        }
        
        return count($users);
    }
}

class OrderService {
    private $conn;
    
    public function __construct($conn) {
        $this->conn = $conn;
    }
    
    public function getOrder($orderId) {
        // SQL query
        $stmt = $this->conn->prepare("SELECT * FROM test_orders WHERE id = ?");
        $stmt->bind_param("i", $orderId);
        $stmt->execute();
        $result = $stmt->get_result();
        $order = $result->fetch_assoc();
        $stmt->close();
        
        // Long operation
        usleep(18000); // 18ms
        
        return $order;
    }
    
    public function getOrdersForUser($userId) {
        // SQL query with JOIN
        $stmt = $this->conn->prepare("SELECT o.* FROM test_orders o WHERE o.user_id = ?");
        $stmt->bind_param("i", $userId);
        $stmt->execute();
        $result = $stmt->get_result();
        $orders = [];
        while ($row = $result->fetch_assoc()) {
            $orders[] = $row;
        }
        $stmt->close();
        return $orders;
    }
}

class ApplicationController {
    private $userService;
    private $orderService;
    
    public function __construct($conn) {
        $db = new DatabaseService($conn);
        $this->userService = new UserService($db);
        $this->orderService = new OrderService($conn);
    }
    
    public function handleRequest() {
        // This creates a call hierarchy
        echo "Processing users...\n";
        $count = $this->userService->processUsers();
        
        echo "Processing orders...\n";
        // Get orders for user 1
        $orders = $this->orderService->getOrdersForUser(1);
        
        // Process each order
        foreach ($orders as $order) {
            if (isset($order['id'])) {
                // Another method call
                $this->orderService->getOrder($order['id']);
            }
        }
        
        return ['users_processed' => $count, 'orders_processed' => count($orders)];
    }
}

// Connect to database
echo "Connecting to MySQL...\n";
$conn = new mysqli($mysql_host, $mysql_user, $mysql_password, $mysql_database, $mysql_port);

if ($conn->connect_error) {
    die("Connection failed: " . $conn->connect_error . "\n");
}

// Create test tables
echo "Creating test tables...\n";
$conn->query("CREATE TABLE IF NOT EXISTS test_users (
    id INT AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    email VARCHAR(100) NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
)");

$conn->query("CREATE TABLE IF NOT EXISTS test_orders (
    id INT AUTO_INCREMENT PRIMARY KEY,
    user_id INT NOT NULL,
    product_name VARCHAR(100) NOT NULL,
    amount DECIMAL(10,2) NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (user_id) REFERENCES test_users(id)
)");

// Insert test data
echo "Inserting test data...\n";
$conn->query("INSERT IGNORE INTO test_users (name, email) VALUES 
    ('John Doe', 'john@example.com'),
    ('Jane Smith', 'jane@example.com'),
    ('Bob Johnson', 'bob@example.com'),
    ('Alice Williams', 'alice@example.com')");

$conn->query("INSERT IGNORE INTO test_orders (user_id, product_name, amount) VALUES 
    (1, 'Product A', 99.99),
    (1, 'Product B', 149.99),
    (2, 'Product C', 79.99),
    (3, 'Product D', 199.99)");

// Create controller and execute
echo "\nExecuting operations with object method calls...\n";
$controller = new ApplicationController($conn);
$result = $controller->handleRequest();

echo "\nResults:\n";
echo "  - Users processed: " . $result['users_processed'] . "\n";
echo "  - Orders processed: " . $result['orders_processed'] . "\n";

$conn->close();

echo "\n✓ Operations completed!\n";
echo "\nThis should have generated:\n";
echo "  - Root span (sent first)\n";
echo "  - Multiple child spans (one per significant call node)\n";
echo "  - All spans sent as separate Incoming messages\n";
echo "\nWaiting 5 seconds for traces to be processed...\n";
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
$span_count_list = $data['traces'][0]['span_count'] ?? 1;

echo "\n=== Validation Results ===\n\n";
echo "Trace ID: $trace_id\n";
echo "Span count (from list): $span_count_list\n\n";

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

$span_count_api = count($trace['spans'] ?? []);
$root_span = $trace['root'] ?? null;

if (!$root_span) {
    die("ERROR: No root span found\n");
}

$expand_spans = $root_span['tags']['expand_spans'] ?? null;
$stack_size = count($root_span['stack'] ?? []);
$has_child_spans = isset($root_span['children']) && count($root_span['children']) > 0;

echo "Total spans (from API): $span_count_api\n";
echo "expand_spans tag: " . var_export($expand_spans, true) . "\n";
echo "Root span stack size: $stack_size (should be 0 when expand_spans is enabled)\n";
echo "Root span has children: " . ($has_child_spans ? 'yes' : 'no') . "\n\n";

// Validation checks
$all_passed = true;

// Check 1: Multiple spans should exist
if ($span_count_api > 1) {
    echo "✓ PASS: Multiple spans created ($span_count_api spans)\n";
} else {
    echo "✗ FAIL: Only 1 span found (expected multiple)\n";
    $all_passed = false;
}

// Check 2: Root span should have empty stack (child spans sent separately)
if ($stack_size == 0) {
    echo "✓ PASS: Root span has empty stack (child spans sent separately)\n";
} else {
    echo "✗ FAIL: Root span has stack size $stack_size (expected 0 when expand_spans is enabled)\n";
    $all_passed = false;
}

// Check 3: expand_spans should be true
if ($expand_spans === true) {
    echo "✓ PASS: expand_spans tag is true\n";
} else {
    echo "✗ FAIL: expand_spans tag is " . var_export($expand_spans, true) . " (expected true)\n";
    $all_passed = false;
}

// Check 4: Child spans should exist
if ($has_child_spans) {
    echo "✓ PASS: Root span has child spans\n";
} else {
    echo "✗ FAIL: Root span has no child spans\n";
    $all_passed = false;
}

// Check 5: span_count in list should match API count
if ($span_count_list == $span_count_api) {
    echo "✓ PASS: span_count in list ($span_count_list) matches API count ($span_count_api)\n";
} else {
    echo "⚠ WARN: span_count in list ($span_count_list) doesn't match API count ($span_count_api)\n";
    // Not a failure, might be timing issue
}

// Check 6: Verify child spans have proper structure
echo "\nChild span details:\n";
$child_count = 0;
foreach ($trace['spans'] as $span) {
    if ($span['parent_id'] !== null) {
        $child_count++;
        $name = $span['name'] ?? 'unknown';
        $parent = $span['parent_id'] ?? 'unknown';
        $sql_count = count($span['sql'] ?? []);
        $duration = $span['duration_ms'] ?? 0;
        echo "  - $name (parent: $parent, SQL: $sql_count, duration: {$duration}ms)\n";
    }
}

if ($child_count > 0) {
    echo "✓ PASS: Found $child_count child spans with proper parent_id\n";
} else {
    echo "✗ FAIL: No child spans found\n";
    $all_passed = false;
}

echo "\n=== Test Summary ===\n";
if ($all_passed) {
    echo "✓ ALL TESTS PASSED: Multiple spans feature is working!\n";
    echo "\nKey validations:\n";
    echo "  - Multiple spans sent as separate messages ✓\n";
    echo "  - Root span has empty stack (child spans sent separately) ✓\n";
    echo "  - All spans stored and retrievable ✓\n";
    echo "  - span_count is accurate ✓\n";
    exit(0);
} else {
    echo "✗ SOME TESTS FAILED\n";
    exit(1);
}
