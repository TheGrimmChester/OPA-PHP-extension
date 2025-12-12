#!/bin/bash
# Helper script to create test endpoint in writable volume
# This avoids heredoc escaping issues

mkdir -p /var/www/html/tests/apps/symfony/public

cat > /var/www/html/tests/apps/symfony/public/test_http_errors.php << 'ENDOFFILE'
<?php
$status = isset($_GET["status"]) ? (int)$_GET["status"] : 200;
if ($status < 100 || $status > 599) $status = 500;
http_response_code($status);
header("Content-Type: application/json");
echo json_encode([
    "status" => $status,
    "message" => "Test error response",
    "timestamp" => time(),
    "method" => $_SERVER["REQUEST_METHOD"] ?? "GET",
    "uri" => $_SERVER["REQUEST_URI"] ?? "/",
], JSON_PRETTY_PRINT);
ENDOFFILE

# Verify the file
if php -l /var/www/html/tests/apps/symfony/public/test_http_errors.php > /dev/null 2>&1; then
    echo "Test endpoint created successfully"
    exit 0
else
    echo "Failed to create valid test endpoint"
    exit 1
fi

