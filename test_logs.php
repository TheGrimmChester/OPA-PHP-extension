<?php
// Test script to verify logs are sent to ClickHouse
echo "Testing log tracking...\n";

// Check configuration
echo "OPA enabled: " . ini_get('opa.enabled') . "\n";
echo "Track logs: " . ini_get('opa.track_logs') . "\n";
echo "Log levels: " . ini_get('opa.log_levels') . "\n";
echo "Service: " . ini_get('opa.service') . "\n";

// Force some log messages by making HTTP requests that will trigger log_error/log_warn/log_info
echo "\nMaking HTTP request to trigger logs...\n";

$ch = curl_init('http://httpbin.org/status/404');
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_exec($ch);
$httpCode = curl_getinfo($ch, CURLINFO_HTTP_CODE);
curl_close($ch);

echo "HTTP request completed with status: $httpCode\n";
echo "This should have triggered log_warn() for 404 status\n";

// Wait a bit for logs to be sent
sleep(2);

echo "\nTest completed. Check ClickHouse logs table.\n";
