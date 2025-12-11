<?php
/**
 * Test to generate a trace with multiple spans
 * This creates a function call hierarchy with SQL queries
 */

$mysql_host = getenv('MYSQL_HOST') ?: 'mysql-test';
$mysql_port = getenv('MYSQL_PORT') ?: 3306;
$mysql_database = getenv('MYSQL_DATABASE') ?: 'test_db';
$mysql_user = getenv('MYSQL_USER') ?: 'test_user';
$mysql_password = getenv('MYSQL_PASSWORD') ?: 'test_password';
$mysql_root_password = getenv('MYSQL_ROOT_PASSWORD') ?: 'root_password';

echo "=== Generating Trace with Multiple Spans ===\n\n";

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
echo "opa.expand_spans = " . ini_get('opa.expand_spans') . "\n\n";

// Create functions that will generate call stack entries
function queryUsers($conn) {
    // This should create a call stack entry with SQL
    $result = $conn->query("SELECT id, name FROM test_users LIMIT 10");
    if ($result) {
        $rows = [];
        while ($row = $result->fetch_assoc()) {
            $rows[] = $row;
        }
        $result->close();
        return $rows;
    }
    return [];
}

function getUserById($conn, $id) {
    // Another SQL query
    $stmt = $conn->prepare("SELECT * FROM test_users WHERE id = ?");
    $stmt->bind_param("i", $id);
    $stmt->execute();
    $result = $stmt->get_result();
    $user = $result->fetch_assoc();
    $stmt->close();
    return $user;
}

function processUsers($conn) {
    // This function calls other functions, creating a call hierarchy
    $users = queryUsers($conn);
    
    // Process each user (creates more call stack entries)
    foreach ($users as $user) {
        if (isset($user['id'])) {
            // Long operation (>10ms) that should create a span
            usleep(15000); // 15ms
            getUserById($conn, $user['id']);
        }
    }
    
    return count($users);
}

// Connect to database
echo "Connecting to MySQL...\n";
$conn = new mysqli($mysql_host, $mysql_user, $mysql_password, $mysql_database, $mysql_port);

if ($conn->connect_error) {
    die("Connection failed: " . $conn->connect_error . "\n");
}

// Create test table if it doesn't exist
$conn->query("CREATE TABLE IF NOT EXISTS test_users (
    id INT AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    email VARCHAR(100) NOT NULL
)");

// Insert test data
$conn->query("INSERT IGNORE INTO test_users (name, email) VALUES 
    ('John Doe', 'john@example.com'),
    ('Jane Smith', 'jane@example.com'),
    ('Bob Johnson', 'bob@example.com')");

echo "Executing function calls with SQL queries...\n";

// This should generate a call stack with multiple SQL queries
$count = processUsers($conn);

echo "Processed $count users\n";

$conn->close();

echo "\nTrace generated! Check the API for multiple spans.\n";
echo "Trace should have:\n";
echo "  - Root span with expand_spans=true\n";
echo "  - Call stack with SQL queries\n";
echo "  - Multiple child spans (one per SQL query + long operations)\n";
