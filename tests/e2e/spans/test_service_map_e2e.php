<?php
/**
 * Service Map End-to-End Test Script
 * 
 * Tests HTTP calls, Redis operations, SQL queries, and cache operations
 * for service map visualization
 */

// Get configuration from environment or use defaults
$mock_url = getenv('MOCK_SERVER_URL') ?: 'http://mock-http-server:8888';
$mysql_host = getenv('MYSQL_HOST') ?: 'mysql-test';
$mysql_port = getenv('MYSQL_PORT') ?: 3306;
$mysql_database = getenv('MYSQL_DATABASE') ?: 'test_db';
$mysql_user = getenv('MYSQL_USER') ?: 'test_user';
$mysql_password = getenv('MYSQL_PASSWORD') ?: 'test_password';
$mysql_root_password = getenv('MYSQL_ROOT_PASSWORD') ?: 'root_password';
$redis_host = getenv('REDIS_HOST') ?: 'redis-test';
$redis_port = getenv('REDIS_PORT') ?: 6379;

echo "=== Service Map E2E Test ===\n\n";

echo "Configuration:\n";
echo "  Mock HTTP Server: $mock_url\n";
echo "  MySQL Host: $mysql_host:$mysql_port\n";
echo "  Redis Host: $redis_host:$redis_port\n\n";

$test_results = [];

// ============================================
// 1. HTTP Calls
// ============================================
echo "=== Testing HTTP Calls ===\n\n";

// Test 1: GET request with 200 status
$ch = curl_init("$mock_url/status/200");
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch, CURLOPT_TIMEOUT, 10);
$result = curl_exec($ch);
$http_code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
curl_close($ch);
$test_results[] = ['status' => $http_code == 200, 'name' => 'HTTP GET 200', 'code' => $http_code];

// Test 2: GET request with 404 status
$ch = curl_init("$mock_url/status/404");
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch, CURLOPT_TIMEOUT, 10);
$result = curl_exec($ch);
$http_code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
curl_close($ch);
$test_results[] = ['status' => $http_code == 404, 'name' => 'HTTP GET 404', 'code' => $http_code];

// Test 3: GET request with 500 status
$ch = curl_init("$mock_url/status/500");
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch, CURLOPT_TIMEOUT, 10);
$result = curl_exec($ch);
$http_code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
curl_close($ch);
$test_results[] = ['status' => $http_code == 500, 'name' => 'HTTP GET 500', 'code' => $http_code];

// Test 4: POST request
$ch = curl_init("$mock_url/status/201");
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch, CURLOPT_POST, true);
curl_setopt($ch, CURLOPT_POSTFIELDS, json_encode(['test' => 'data']));
curl_setopt($ch, CURLOPT_HTTPHEADER, ['Content-Type: application/json']);
curl_setopt($ch, CURLOPT_TIMEOUT, 10);
$result = curl_exec($ch);
$http_code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
curl_close($ch);
$test_results[] = ['status' => $http_code == 201, 'name' => 'HTTP POST 201', 'code' => $http_code];

// Test 5: Large response
$ch = curl_init("$mock_url/status/200?size=5120");
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch, CURLOPT_TIMEOUT, 10);
$result = curl_exec($ch);
$http_code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
$size = strlen($result);
curl_close($ch);
$test_results[] = ['status' => $http_code == 200 && $size >= 5000, 'name' => 'HTTP Large Response', 'code' => $http_code, 'size' => $size];

echo "HTTP tests completed!\n\n";

// ============================================
// 2. Redis Operations
// ============================================
echo "=== Testing Redis Operations ===\n\n";

try {
    // Check if Redis extension is available
    if (!extension_loaded('redis')) {
        echo "Redis extension not available, skipping Redis tests\n\n";
    } else {
        $redis = new Redis();
        $connected = false;
        $max_attempts = 10;
        $attempt = 0;
        
        // Wait for Redis to be ready
        while ($attempt < $max_attempts && !$connected) {
            try {
                if ($redis->connect($redis_host, $redis_port, 2.0)) {
                    $connected = true;
                    echo "Connected to Redis!\n\n";
                }
            } catch (Exception $e) {
                // Continue waiting
            }
            if (!$connected) {
                sleep(1);
                $attempt++;
            }
        }
        
        if ($connected) {
            // Test SET
            $result = $redis->set('test_key_1', 'test_value_1');
            $test_results[] = ['status' => $result, 'name' => 'Redis SET', 'result' => $result];
            
            // Test GET
            $value = $redis->get('test_key_1');
            $test_results[] = ['status' => $value === 'test_value_1', 'name' => 'Redis GET', 'value' => $value];
            
            // Test SET with expiration
            $result = $redis->setex('test_key_2', 60, 'test_value_2');
            $test_results[] = ['status' => $result, 'name' => 'Redis SETEX', 'result' => $result];
            
            // Test DEL
            $result = $redis->del('test_key_1');
            $test_results[] = ['status' => $result >= 0, 'name' => 'Redis DEL', 'result' => $result];
            
            // Test EXISTS
            $result = $redis->exists('test_key_2');
            $test_results[] = ['status' => $result >= 0, 'name' => 'Redis EXISTS', 'result' => $result];
            
            // Test HSET/HGET (hash operations)
            $result = $redis->hset('test_hash', 'field1', 'value1');
            $test_results[] = ['status' => $result >= 0, 'name' => 'Redis HSET', 'result' => $result];
            
            $value = $redis->hget('test_hash', 'field1');
            $test_results[] = ['status' => $value === 'value1', 'name' => 'Redis HGET', 'value' => $value];
            
            // Cleanup
            $redis->del('test_key_2', 'test_hash');
            $redis->close();
            
            echo "Redis tests completed!\n\n";
        } else {
            echo "Could not connect to Redis after $max_attempts attempts\n\n";
        }
    }
} catch (Exception $e) {
    echo "ERROR in Redis tests: " . $e->getMessage() . "\n\n";
}

// ============================================
// 3. SQL/Database Operations
// ============================================
echo "=== Testing SQL/Database Operations ===\n\n";

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
    echo "ERROR: Could not connect to MySQL after $max_attempts attempts\n\n";
} else {
    try {
        $mysqli = new mysqli($mysql_host, $mysql_user, $mysql_password, $mysql_database, $mysql_port);
        
        if ($mysqli->connect_error) {
            echo "MySQLi Connection failed: " . $mysqli->connect_error . "\n\n";
        } else {
            // CREATE TABLE
            mysqli_query($mysqli, "DROP TABLE IF EXISTS service_map_test");
            mysqli_query($mysqli, "CREATE TABLE service_map_test (
                id INT AUTO_INCREMENT PRIMARY KEY,
                name VARCHAR(100) NOT NULL,
                value TEXT,
                created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
            )");
            $test_results[] = ['status' => true, 'name' => 'SQL CREATE TABLE'];
            
            // INSERT
            mysqli_query($mysqli, "INSERT INTO service_map_test (name, value) VALUES ('test1', 'value1')");
            mysqli_query($mysqli, "INSERT INTO service_map_test (name, value) VALUES ('test2', 'value2')");
            $test_results[] = ['status' => true, 'name' => 'SQL INSERT'];
            
            // SELECT
            $result = mysqli_query($mysqli, "SELECT * FROM service_map_test");
            $row_count = mysqli_num_rows($result);
            mysqli_free_result($result);
            $test_results[] = ['status' => $row_count >= 2, 'name' => 'SQL SELECT', 'count' => $row_count];
            
            // UPDATE
            mysqli_query($mysqli, "UPDATE service_map_test SET value = 'updated_value' WHERE name = 'test1'");
            $test_results[] = ['status' => true, 'name' => 'SQL UPDATE'];
            
            // SELECT with WHERE
            $result = mysqli_query($mysqli, "SELECT COUNT(*) as total FROM service_map_test WHERE name = 'test1'");
            $row = mysqli_fetch_assoc($result);
            mysqli_free_result($result);
            $test_results[] = ['status' => $row['total'] == 1, 'name' => 'SQL SELECT WHERE', 'count' => $row['total']];
            
            $mysqli->close();
            echo "MySQLi SQL tests completed!\n\n";
            
            // Test PDO connection to verify db_system and db_host are captured
            try {
                $dsn = "mysql:host=$mysql_host;port=$mysql_port;dbname=$mysql_database";
                $pdo = new PDO($dsn, $mysql_user, $mysql_password, [
                    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION
                ]);
                
                // Execute a simple query via PDO
                $stmt = $pdo->prepare("SELECT COUNT(*) as total FROM service_map_test");
                $stmt->execute();
                $row = $stmt->fetch(PDO::FETCH_ASSOC);
                $test_results[] = ['status' => isset($row['total']), 'name' => 'PDO SELECT', 'count' => $row['total'] ?? 0];
                
                // Execute another query to ensure multiple queries are captured
                $stmt = $pdo->query("SELECT * FROM service_map_test LIMIT 1");
                $result = $stmt->fetch(PDO::FETCH_ASSOC);
                $test_results[] = ['status' => !empty($result), 'name' => 'PDO Query', 'result' => !empty($result)];
                
                $pdo = null; // Close connection
                echo "PDO SQL tests completed!\n\n";
            } catch (Exception $e) {
                echo "PDO test error: " . $e->getMessage() . "\n\n";
            }
        }
    } catch (Exception $e) {
        echo "ERROR in SQL tests: " . $e->getMessage() . "\n\n";
    }
}

// ============================================
// 4. Cache Operations (APCu)
// ============================================
echo "=== Testing Cache Operations (APCu) ===\n\n";

if (function_exists('apcu_store')) {
    // Test APCu store
    $result = apcu_store('cache_key_1', 'cache_value_1', 60);
    $test_results[] = ['status' => $result, 'name' => 'APCu STORE', 'result' => $result];
    
    // Test APCu fetch
    $value = apcu_fetch('cache_key_1');
    $test_results[] = ['status' => $value === 'cache_value_1', 'name' => 'APCu FETCH', 'value' => $value];
    
    // Test APCu delete
    $result = apcu_delete('cache_key_1');
    $test_results[] = ['status' => $result, 'name' => 'APCu DELETE', 'result' => $result];
    
    // Test APCu exists
    $result = apcu_exists('cache_key_1');
    $test_results[] = ['status' => $result === false, 'name' => 'APCu EXISTS (after delete)', 'result' => $result];
    
    echo "Cache tests completed!\n\n";
} else {
    echo "APCu extension not available, skipping cache tests\n\n";
}

// ============================================
// Summary
// ============================================
echo "=== Test Summary ===\n";
$passed = 0;
$failed = 0;
foreach ($test_results as $test) {
    if ($test['status']) {
        $passed++;
        echo "✓ " . $test['name'];
        if (isset($test['code'])) {
            echo " (HTTP " . $test['code'] . ")";
        }
        if (isset($test['count'])) {
            echo " (count: " . $test['count'] . ")";
        }
        echo "\n";
    } else {
        $failed++;
        echo "✗ " . $test['name'];
        if (isset($test['code'])) {
            echo " (HTTP " . $test['code'] . ")";
        }
        echo "\n";
    }
}

echo "\nTest Summary: $passed passed, $failed failed\n";
exit($failed > 0 ? 1 : 0);
