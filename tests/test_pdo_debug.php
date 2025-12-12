<?php
/**
 * Simple PDO debug test to check if hooks are working
 */

echo "=== PDO Debug Test ===\n\n";

// Enable error reporting
error_reporting(E_ALL);
ini_set('display_errors', 1);

// Check if PDO is available
if (!extension_loaded('pdo')) {
    echo "ERROR: PDO extension is not loaded\n";
    exit(1);
}

echo "PDO extension is loaded\n";

// Check if opa extension is loaded
if (!extension_loaded('opa')) {
    echo "ERROR: OPA extension is not loaded\n";
    exit(1);
}

echo "OPA extension is loaded\n";

// Try to connect to a test database (will fail, but we just want to see if hooks are called)
try {
    echo "\nAttempting PDO connection...\n";
    $pdo = new PDO('mysql:host=localhost;dbname=test', 'user', 'pass');
    echo "Connection successful (unexpected!)\n";
} catch (PDOException $e) {
    echo "Connection failed (expected): " . $e->getMessage() . "\n";
}

// Try a query (will fail, but hook should be called)
try {
    echo "\nAttempting PDO query...\n";
    $stmt = $pdo->query("SELECT 1");
    echo "Query successful (unexpected!)\n";
} catch (PDOException $e) {
    echo "Query failed (expected): " . $e->getMessage() . "\n";
}

echo "\n=== Test Complete ===\n";
echo "Check agent logs and ClickHouse to see if SQL profiling data was captured\n";
