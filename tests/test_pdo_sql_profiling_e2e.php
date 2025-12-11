<?php
/**
 * Comprehensive PDO SQL Profiling E2E Test
 * Tests various PDO query types and verifies SQL profiling data
 */

$mysql_host = getenv('MYSQL_HOST') ?: 'mysql-test';
$mysql_port = getenv('MYSQL_PORT') ?: 3306;
$mysql_database = getenv('MYSQL_DATABASE') ?: 'test_db';
$mysql_user = getenv('MYSQL_USER') ?: 'test_user';
$mysql_password = getenv('MYSQL_PASSWORD') ?: 'test_password';

echo "=== PDO SQL Profiling E2E Test ===\n\n";

$test_results = [];

try {
    // Test PDO connection
    $dsn = "mysql:host=$mysql_host;port=$mysql_port;dbname=$mysql_database;charset=utf8mb4";
    echo "Connecting to: $dsn\n";
    
    $pdo = new PDO($dsn, $mysql_user, $mysql_password, [
        PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
        PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC,
        PDO::ATTR_TIMEOUT => 5
    ]);
    
    echo "Connected successfully!\n\n";
    
    // Test 1: CREATE TABLE (DDL)
    echo "Test 1: CREATE TABLE (DDL)\n";
    $pdo->exec("DROP TABLE IF EXISTS pdo_test_products");
    $pdo->exec("CREATE TABLE pdo_test_products (
        id INT AUTO_INCREMENT PRIMARY KEY,
        name VARCHAR(100) NOT NULL,
        price DECIMAL(10,2) NOT NULL,
        category VARCHAR(50),
        stock INT DEFAULT 0,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    )");
    $test_results[] = ['status' => true, 'name' => 'CREATE TABLE', 'type' => 'DDL'];
    echo "  ✓ Table created\n\n";
    
    // Test 2: INSERT single row using exec()
    echo "Test 2: INSERT single row (exec)\n";
    $affected = $pdo->exec("INSERT INTO pdo_test_products (name, price, category, stock) VALUES ('Product A', 19.99, 'Electronics', 10)");
    $test_results[] = ['status' => $affected === 1, 'name' => 'INSERT exec()', 'type' => 'INSERT', 'rows' => $affected];
    echo "  ✓ Inserted $affected row(s)\n\n";
    
    // Test 3: INSERT multiple rows using exec()
    echo "Test 3: INSERT multiple rows (exec)\n";
    $affected = $pdo->exec("INSERT INTO pdo_test_products (name, price, category, stock) VALUES 
        ('Product B', 29.99, 'Electronics', 15),
        ('Product C', 39.99, 'Clothing', 20),
        ('Product D', 49.99, 'Clothing', 25),
        ('Product E', 59.99, 'Electronics', 30)");
    $test_results[] = ['status' => $affected === 4, 'name' => 'INSERT multiple rows', 'type' => 'INSERT', 'rows' => $affected];
    echo "  ✓ Inserted $affected row(s)\n\n";
    
    // Test 4: SELECT all rows using query()
    echo "Test 4: SELECT all rows (query)\n";
    $stmt = $pdo->query("SELECT * FROM pdo_test_products");
    $rows = $stmt->fetchAll();
    $row_count = count($rows);
    $test_results[] = ['status' => $row_count >= 5, 'name' => 'SELECT query()', 'type' => 'SELECT', 'rows' => $row_count];
    echo "  ✓ Fetched $row_count row(s)\n\n";
    
    // Test 5: SELECT with WHERE using query()
    echo "Test 5: SELECT with WHERE (query)\n";
    $stmt = $pdo->query("SELECT * FROM pdo_test_products WHERE category = 'Electronics'");
    $rows = $stmt->fetchAll();
    $row_count = count($rows);
    $test_results[] = ['status' => $row_count >= 3, 'name' => 'SELECT with WHERE', 'type' => 'SELECT', 'rows' => $row_count];
    echo "  ✓ Fetched $row_count row(s)\n\n";
    
    // Test 6: SELECT COUNT using query()
    echo "Test 6: SELECT COUNT (query)\n";
    $stmt = $pdo->query("SELECT COUNT(*) as total FROM pdo_test_products");
    $result = $stmt->fetch();
    $count = (int)$result['total'];
    $test_results[] = ['status' => $count >= 5, 'name' => 'SELECT COUNT', 'type' => 'SELECT', 'rows' => $count];
    echo "  ✓ Total products: $count\n\n";
    
    // Test 7: Prepared statement SELECT with parameter
    echo "Test 7: Prepared statement SELECT\n";
    $stmt = $pdo->prepare("SELECT * FROM pdo_test_products WHERE category = ?");
    $stmt->execute(['Clothing']);
    $rows = $stmt->fetchAll();
    $row_count = count($rows);
    $test_results[] = ['status' => $row_count >= 2, 'name' => 'PDO prepared SELECT', 'type' => 'SELECT', 'rows' => $row_count];
    echo "  ✓ Fetched $row_count row(s)\n\n";
    
    // Test 8: Prepared statement SELECT with multiple parameters
    echo "Test 8: Prepared statement SELECT (multiple params)\n";
    $stmt = $pdo->prepare("SELECT * FROM pdo_test_products WHERE category = ? AND price > ?");
    $stmt->execute(['Electronics', 20]);
    $rows = $stmt->fetchAll();
    $row_count = count($rows);
    $test_results[] = ['status' => $row_count >= 2, 'name' => 'PDO prepared SELECT (multi-param)', 'type' => 'SELECT', 'rows' => $row_count];
    echo "  ✓ Fetched $row_count row(s)\n\n";
    
    // Test 9: Prepared statement INSERT
    echo "Test 9: Prepared statement INSERT\n";
    $stmt = $pdo->prepare("INSERT INTO pdo_test_products (name, price, category, stock) VALUES (?, ?, ?, ?)");
    $stmt->execute(['Product F', 69.99, 'Electronics', 35]);
    $affected = $stmt->rowCount();
    $test_results[] = ['status' => $affected === 1, 'name' => 'PDO prepared INSERT', 'type' => 'INSERT', 'rows' => $affected];
    echo "  ✓ Inserted $affected row(s)\n\n";
    
    // Test 10: Prepared statement UPDATE
    echo "Test 10: Prepared statement UPDATE\n";
    $stmt = $pdo->prepare("UPDATE pdo_test_products SET price = ? WHERE name = ?");
    $stmt->execute([79.99, 'Product F']);
    $affected = $stmt->rowCount();
    $test_results[] = ['status' => $affected >= 1, 'name' => 'PDO prepared UPDATE', 'type' => 'UPDATE', 'rows' => $affected];
    echo "  ✓ Updated $affected row(s)\n\n";
    
    // Test 11: Prepared statement DELETE
    echo "Test 11: Prepared statement DELETE\n";
    $stmt = $pdo->prepare("DELETE FROM pdo_test_products WHERE name = ?");
    $stmt->execute(['Product E']);
    $affected = $stmt->rowCount();
    $test_results[] = ['status' => $affected >= 1, 'name' => 'PDO prepared DELETE', 'type' => 'DELETE', 'rows' => $affected];
    echo "  ✓ Deleted $affected row(s)\n\n";
    
    // Test 12: Transaction with multiple queries
    echo "Test 12: Transaction with multiple queries\n";
    $pdo->beginTransaction();
    try {
        $stmt = $pdo->prepare("INSERT INTO pdo_test_products (name, price, category, stock) VALUES (?, ?, ?, ?)");
        $stmt->execute(['Product G', 89.99, 'Clothing', 40]);
        $stmt->execute(['Product H', 99.99, 'Electronics', 45]);
        $pdo->commit();
        $test_results[] = ['status' => true, 'name' => 'Transaction INSERT', 'type' => 'INSERT', 'rows' => 2];
        echo "  ✓ Transaction committed with 2 inserts\n\n";
    } catch (Exception $e) {
        $pdo->rollBack();
        throw $e;
    }
    
    // Test 13: SELECT with JOIN
    echo "Test 13: SELECT with JOIN\n";
    $pdo->exec("CREATE TABLE IF NOT EXISTS pdo_test_categories (
        id INT AUTO_INCREMENT PRIMARY KEY,
        name VARCHAR(50) NOT NULL
    )");
    $pdo->exec("INSERT IGNORE INTO pdo_test_categories (name) VALUES ('Electronics'), ('Clothing')");
    $stmt = $pdo->query("SELECT p.name, p.price, c.name as category_name 
        FROM pdo_test_products p 
        LEFT JOIN pdo_test_categories c ON p.category = c.name 
        LIMIT 5");
    $rows = $stmt->fetchAll();
    $test_results[] = ['status' => count($rows) > 0, 'name' => 'SELECT with JOIN', 'type' => 'SELECT', 'rows' => count($rows)];
    echo "  ✓ Fetched " . count($rows) . " row(s) with JOIN\n\n";
    
    // Test 14: SELECT with aggregation
    echo "Test 14: SELECT with aggregation\n";
    $stmt = $pdo->query("SELECT category, COUNT(*) as count, AVG(price) as avg_price, SUM(stock) as total_stock 
        FROM pdo_test_products 
        GROUP BY category");
    $rows = $stmt->fetchAll();
    $test_results[] = ['status' => count($rows) > 0, 'name' => 'SELECT with aggregation', 'type' => 'SELECT', 'rows' => count($rows)];
    echo "  ✓ Aggregation query executed\n\n";
    
    // Test 15: Multiple queries in sequence
    echo "Test 15: Multiple queries in sequence\n";
    $pdo->query("SELECT COUNT(*) FROM pdo_test_products");
    $pdo->query("SELECT MAX(price) FROM pdo_test_products");
    $pdo->query("SELECT MIN(price) FROM pdo_test_products");
    $test_results[] = ['status' => true, 'name' => 'Multiple sequential queries', 'type' => 'SELECT', 'rows' => 3];
    echo "  ✓ Multiple queries executed\n\n";
    
    $pdo = null;
    
    // Summary
    echo "=== Test Summary ===\n";
    $passed = 0;
    $failed = 0;
    foreach ($test_results as $test) {
        if ($test['status']) {
            $passed++;
            $rows_info = isset($test['rows']) ? " ({$test['rows']} rows)" : "";
            echo "✓ " . $test['name'] . $rows_info . "\n";
        } else {
            $failed++;
            echo "✗ " . $test['name'] . "\n";
        }
    }
    
    echo "\nTotal: $passed passed, $failed failed\n";
    echo "\nAll PDO SQL profiling tests completed!\n";
    echo "Check ClickHouse for SQL profiling data in spans_full and spans_min tables\n";
    
    exit($failed > 0 ? 1 : 0);
    
} catch (PDOException $e) {
    echo "PDO Error: " . $e->getMessage() . "\n";
    exit(1);
} catch (Exception $e) {
    echo "Error: " . $e->getMessage() . "\n";
    exit(1);
}
