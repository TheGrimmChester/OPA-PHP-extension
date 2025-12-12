<?php
/**
 * Web Request Test for Span Expansion
 * This simulates a web request with SQL queries to generate call stack
 */

$mysql_host = getenv('MYSQL_HOST') ?: 'mysql-test';
$mysql_port = getenv('MYSQL_PORT') ?: 3306;
$mysql_database = getenv('MYSQL_DATABASE') ?: 'test_db';
$mysql_user = getenv('MYSQL_USER') ?: 'test_user';
$mysql_password = getenv('MYSQL_PASSWORD') ?: 'test_password';

// Set expand_spans
ini_set('opa.expand_spans', '1');

// Functions that create call hierarchy
function getUserData($conn, $id) {
    $stmt = $conn->prepare("SELECT * FROM test_users WHERE id = ?");
    $stmt->bind_param("i", $id);
    $stmt->execute();
    $result = $stmt->get_result();
    $user = $result->fetch_assoc();
    $stmt->close();
    return $user;
}

function processUser($conn, $id) {
    // Long operation that should create a span
    usleep(20000); // 20ms
    return getUserData($conn, $id);
}

// Connect and execute
$conn = new mysqli($mysql_host, $mysql_user, $mysql_password, $mysql_database, $mysql_port);
if (!$conn->connect_error) {
    // Create table
    $conn->query("CREATE TABLE IF NOT EXISTS test_users (id INT PRIMARY KEY, name VARCHAR(100))");
    $conn->query("INSERT IGNORE INTO test_users VALUES (1, 'Test User')");
    
    // Execute function calls
    $user = processUser($conn, 1);
    
    // More SQL queries
    $conn->query("SELECT COUNT(*) FROM test_users");
    $conn->query("SELECT * FROM test_users LIMIT 5");
    
    $conn->close();
}

echo "Request completed\n";
