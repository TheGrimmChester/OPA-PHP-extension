<?php
/**
 * Standalone PDO E2E Test
 * Tests PDO queries to verify db_system, db_host, and db_dsn are captured
 */

$mysql_host = getenv('MYSQL_HOST') ?: 'mysql-test';
$mysql_port = getenv('MYSQL_PORT') ?: 3306;
$mysql_database = getenv('MYSQL_DATABASE') ?: 'test_db';
$mysql_user = getenv('MYSQL_USER') ?: 'test_user';
$mysql_password = getenv('MYSQL_PASSWORD') ?: 'test_password';

echo "=== PDO E2E Test ===\n\n";

try {
    // Test PDO connection
    $dsn = "mysql:host=$mysql_host;port=$mysql_port;dbname=$mysql_database";
    echo "Connecting to: $dsn\n";
    
    $pdo = new PDO($dsn, $mysql_user, $mysql_password, [
        PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
        PDO::ATTR_TIMEOUT => 5
    ]);
    
    echo "Connected successfully!\n\n";
    
    // Test 1: PDO::query()
    echo "Test 1: PDO::query()\n";
    $stmt = $pdo->query("SELECT 1 as test");
    $result = $stmt->fetch(PDO::FETCH_ASSOC);
    echo "  Result: " . ($result['test'] == 1 ? "PASS" : "FAIL") . "\n\n";
    
    // Test 2: PDOStatement::execute() with prepare
    echo "Test 2: PDOStatement::execute()\n";
    $stmt = $pdo->prepare("SELECT 2 as test");
    $stmt->execute();
    $result = $stmt->fetch(PDO::FETCH_ASSOC);
    echo "  Result: " . ($result['test'] == 2 ? "PASS" : "FAIL") . "\n\n";
    
    // Test 3: Multiple queries
    echo "Test 3: Multiple PDO queries\n";
    $pdo->query("SELECT 3 as test");
    $stmt = $pdo->prepare("SELECT 4 as test");
    $stmt->execute();
    echo "  Multiple queries executed\n\n";
    
    $pdo = null;
    echo "All PDO tests completed!\n";
    echo "Check ClickHouse for db_system, db_host, and db_dsn in SQL data\n";
    
} catch (PDOException $e) {
    echo "PDO Error: " . $e->getMessage() . "\n";
    exit(1);
} catch (Exception $e) {
    echo "Error: " . $e->getMessage() . "\n";
    exit(1);
}
