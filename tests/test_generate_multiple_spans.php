<?php
/**
 * Test to Generate Multiple Spans with Call Stack
 * 
 * This test uses PHP classes and objects to create a proper call hierarchy
 * that will be captured in the call stack and expanded into multiple spans.
 */

$mysql_host = getenv('MYSQL_HOST') ?: 'mysql-test';
$mysql_port = getenv('MYSQL_PORT') ?: 3306;
$mysql_database = getenv('MYSQL_DATABASE') ?: 'test_db';
$mysql_user = getenv('MYSQL_USER') ?: 'test_user';
$mysql_password = getenv('MYSQL_PASSWORD') ?: 'test_password';
$mysql_root_password = getenv('MYSQL_ROOT_PASSWORD') ?: 'root_password';

echo "=== Generating Multiple Spans Test ===\n\n";

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
echo "  - Multiple SQL queries (each should create a child span)\n";
echo "  - Multiple function calls (creating call hierarchy)\n";
echo "  - Long operations >10ms (should create child spans)\n";
echo "  - Total: Multiple spans in the trace\n";
echo "\nWaiting 5 seconds for trace to be processed...\n";
sleep(5);
