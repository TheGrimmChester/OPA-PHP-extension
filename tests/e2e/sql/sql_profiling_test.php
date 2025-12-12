<?php
/**
 * SQL Profiling Test Script
 * 
 * Tests MySQLi and PDO SQL profiling hooks
 */

// Get database connection details from environment or use defaults
$mysql_host = getenv('MYSQL_HOST') ?: 'mysql';
$mysql_port = getenv('MYSQL_PORT') ?: 3306;
$mysql_database = getenv('MYSQL_DATABASE') ?: 'test_db';
$mysql_user = getenv('MYSQL_USER') ?: 'test_user';
$mysql_password = getenv('MYSQL_PASSWORD') ?: 'test_password';
$mysql_root_password = getenv('MYSQL_ROOT_PASSWORD') ?: 'root_password';

echo "=== SQL Profiling Test ===\n\n";

echo "Database Configuration:\n";
echo "  Host: $mysql_host\n";
echo "  Port: $mysql_port\n";
echo "  Database: $mysql_database\n";
echo "  User: $mysql_user\n\n";

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

// Test MySQLi
echo "=== Testing MySQLi ===\n\n";

try {
    $mysqli = new mysqli($mysql_host, $mysql_user, $mysql_password, $mysql_database, $mysql_port);
    
    if ($mysqli->connect_error) {
        die("MySQLi Connection failed: " . $mysqli->connect_error . "\n");
    }
    
    echo "1. Testing mysqli_query() with CREATE TABLE...\n";
    mysqli_query($mysqli, "DROP TABLE IF EXISTS test_users");
    mysqli_query($mysqli, "CREATE TABLE test_users (
        id INT AUTO_INCREMENT PRIMARY KEY,
        name VARCHAR(100) NOT NULL,
        email VARCHAR(100) NOT NULL,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    )");
    echo "   ✓ Table created\n\n";
    
    echo "2. Testing mysqli_query() with INSERT...\n";
    mysqli_query($mysqli, "INSERT INTO test_users (name, email) VALUES ('John Doe', 'john@example.com')");
    mysqli_query($mysqli, "INSERT INTO test_users (name, email) VALUES ('Jane Smith', 'jane@example.com')");
    echo "   ✓ Data inserted\n\n";
    
    echo "3. Testing mysqli_query() with SELECT (multiple rows)...\n";
    $result = mysqli_query($mysqli, "SELECT * FROM test_users");
    if ($result) {
        $row_count = mysqli_num_rows($result);
        echo "   ✓ Query executed, found $row_count rows\n";
        while ($row = mysqli_fetch_assoc($result)) {
            echo "     - User: {$row['name']} ({$row['email']})\n";
        }
        mysqli_free_result($result);
        echo "\n";
    }
    
    echo "3a. Testing mysqli_query() with SELECT COUNT...\n";
    $result = mysqli_query($mysqli, "SELECT COUNT(*) as total FROM test_users");
    if ($result) {
        $row = mysqli_fetch_assoc($result);
        echo "   ✓ Count query executed, total users: " . $row['total'] . "\n\n";
        mysqli_free_result($result);
    }
    
    echo "3b. Testing mysqli_query() with SELECT LIMIT (multiple rows)...\n";
    // Insert more test data
    mysqli_query($mysqli, "INSERT INTO test_users (name, email) VALUES 
        ('Alice Brown', 'alice@example.com'),
        ('Bob Johnson', 'bob@example.com'),
        ('Charlie Wilson', 'charlie@example.com'),
        ('Diana Davis', 'diana@example.com')");
    
    $result = mysqli_query($mysqli, "SELECT id, name, email FROM test_users ORDER BY id LIMIT 5");
    if ($result) {
        $row_count = mysqli_num_rows($result);
        echo "   ✓ Query executed, found $row_count rows\n";
        $rows = [];
        while ($row = mysqli_fetch_assoc($result)) {
            $rows[] = $row;
            echo "     - ID: {$row['id']}, Name: {$row['name']}, Email: {$row['email']}\n";
        }
        echo "   ✓ Fetched " . count($rows) . " rows successfully\n\n";
        mysqli_free_result($result);
    }
    
    echo "4. Testing mysqli_query() with UPDATE...\n";
    mysqli_query($mysqli, "UPDATE test_users SET email = 'john.doe@example.com' WHERE name = 'John Doe'");
    echo "   ✓ Update executed\n\n";
    
    $mysqli->close();
    echo "MySQLi tests completed!\n\n";
    
} catch (Exception $e) {
    echo "ERROR in MySQLi tests: " . $e->getMessage() . "\n\n";
}

// Test PDO
echo "=== Testing PDO ===\n\n";

try {
    $dsn = "mysql:host=$mysql_host;port=$mysql_port;dbname=$mysql_database;charset=utf8mb4";
    $pdo = new PDO($dsn, $mysql_user, $mysql_password, [
        PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
        PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC
    ]);
    
    echo "5. Testing PDO::query() with SELECT...\n";
    $stmt = $pdo->query("SELECT COUNT(*) as count FROM test_users");
    $result = $stmt->fetch();
    echo "   ✓ Query executed, found " . $result['count'] . " users\n\n";
    
    echo "6. Testing PDO::query() with INSERT...\n";
    $pdo->query("INSERT INTO test_users (name, email) VALUES ('Bob Johnson', 'bob@example.com')");
    echo "   ✓ Insert executed\n\n";
    
    echo "7. Testing PDO::query() with SELECT (fetch all - multiple rows)...\n";
    $stmt = $pdo->query("SELECT id, name, email FROM test_users ORDER BY id");
    $users = $stmt->fetchAll();
    echo "   ✓ Query executed, fetched " . count($users) . " users\n";
    foreach (array_slice($users, 0, 5) as $user) {
        echo "     - ID: {$user['id']}, Name: {$user['name']}, Email: {$user['email']}\n";
    }
    if (count($users) > 5) {
        echo "     ... and " . (count($users) - 5) . " more users\n";
    }
    echo "\n";
    
    echo "7a. Testing PDO::query() with SELECT WHERE (multiple matching rows)...\n";
    $stmt = $pdo->query("SELECT id, name, email FROM test_users WHERE email LIKE '%@example.com' ORDER BY name");
    $matching_users = $stmt->fetchAll();
    echo "   ✓ Query executed, found " . count($matching_users) . " users with @example.com email\n";
    foreach ($matching_users as $user) {
        echo "     - {$user['name']} ({$user['email']})\n";
    }
    echo "\n";
    
    echo "=== Testing PDO Prepared Statements ===\n\n";
    
    echo "8. Testing PDOStatement::execute() with prepared statement (single row)...\n";
    $stmt = $pdo->prepare("SELECT * FROM test_users WHERE id = ?");
    $stmt->execute([1]);
    $user = $stmt->fetch();
    if ($user) {
        echo "   ✓ Prepared statement executed, found user: " . $user['name'] . "\n\n";
    }
    
    echo "8a. Testing PDOStatement::execute() with prepared statement (multiple rows)...\n";
    // Note: LIMIT cannot be bound as parameter in MySQL, so we use a fixed limit
    $stmt = $pdo->prepare("SELECT id, name, email FROM test_users WHERE id > ? ORDER BY id LIMIT 5");
    $stmt->execute([2]);
    $users = $stmt->fetchAll();
    echo "   ✓ Prepared statement executed, found " . count($users) . " users with ID > 2\n";
    foreach ($users as $user) {
        echo "     - ID: {$user['id']}, Name: {$user['name']}\n";
    }
    echo "\n";
    
    echo "8b. Testing PDOStatement::execute() with IN clause (multiple rows)...\n";
    $stmt = $pdo->prepare("SELECT id, name, email FROM test_users WHERE id IN (?, ?, ?, ?) ORDER BY id");
    $stmt->execute([1, 3, 5, 7]);
    $selected_users = $stmt->fetchAll();
    echo "   ✓ Prepared statement with IN clause executed, found " . count($selected_users) . " users\n";
    foreach ($selected_users as $user) {
        echo "     - ID: {$user['id']}, Name: {$user['name']}, Email: {$user['email']}\n";
    }
    echo "\n";
    
    echo "9. Testing PDOStatement::execute() with INSERT...\n";
    $stmt = $pdo->prepare("INSERT INTO test_users (name, email) VALUES (?, ?)");
    $stmt->execute(['Alice Brown', 'alice@example.com']);
    echo "   ✓ Prepared INSERT executed\n\n";
    
    echo "10. Testing PDOStatement::execute() with UPDATE...\n";
    $stmt = $pdo->prepare("UPDATE test_users SET email = ? WHERE name = ?");
    $stmt->execute(['alice.brown@example.com', 'Alice Brown']);
    echo "   ✓ Prepared UPDATE executed\n\n";
    
    echo "PDO tests completed!\n\n";
    
} catch (PDOException $e) {
    echo "ERROR in PDO tests: " . $e->getMessage() . "\n\n";
} catch (Exception $e) {
    echo "ERROR in PDO tests: " . $e->getMessage() . "\n\n";
}


echo "\n=== Test Summary ===\n";
echo "All SQL profiling tests completed!\n";
echo "Check the output above for SQL profiling logs from the OPA extension.\n";
echo "Each query should show: [OPA SQL Profiling] <Type> Query: <SQL> | Time: <ms> | Rows: <count>\n";
echo "\nNote: SQL queries are logged to stdout AND sent to the OPA agent (if connected).\n";

