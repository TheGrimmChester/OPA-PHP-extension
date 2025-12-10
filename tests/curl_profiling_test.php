<?php
/**
 * cURL Profiling Test Script
 * 
 * Tests comprehensive cURL profiling including:
 * - Basic curl_exec profiling
 * - Request/response body capture
 * - Timing metrics (dns, connect, transfer times)
 * - Byte measurements (both curl_getinfo and system-level)
 * - Status codes and error handling
 * - Multiple request types (GET, POST, etc.)
 */

echo "=== cURL Profiling Test ===\n\n";

// Test configuration
$test_url = getenv('TEST_URL') ?: 'https://aisenseapi.com/services/v1/ping';
$test_timeout = 10;

echo "Test Configuration:\n";
echo "  Test URL: $test_url\n";
echo "  Timeout: $test_timeout seconds\n\n";

$tests_passed = 0;
$tests_failed = 0;

// Test result function
function test_result($passed, $test_name, $details = '') {
    global $tests_passed, $tests_failed;
    if ($passed) {
        echo "✓ $test_name\n";
        $tests_passed++;
        if ($details) {
            echo "  $details\n";
        }
    } else {
        echo "✗ $test_name\n";
        $tests_failed++;
        if ($details) {
            echo "  ERROR: $details\n";
        }
    }
}

// Test 1: Basic GET request
echo "Test 1: Basic GET request\n";
try {
    $ch = curl_init();
    curl_setopt($ch, CURLOPT_URL, $test_url);
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_TIMEOUT, $test_timeout);
    curl_setopt($ch, CURLOPT_HEADER, true);
    
    $response = curl_exec($ch);
    $http_code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    $error = curl_error($ch);
    
    curl_close($ch);
    
    $passed = ($http_code == 200 || $http_code == 301 || $http_code == 302) && empty($error);
    test_result($passed, "Basic GET request", "HTTP Code: $http_code" . ($error ? ", Error: $error" : ""));
} catch (Exception $e) {
    test_result(false, "Basic GET request", $e->getMessage());
}

// Test 2: POST request with body
echo "\nTest 2: POST request with body\n";
try {
    $post_data = json_encode(['test' => 'post_data', 'timestamp' => time()]);
    $ch = curl_init();
    curl_setopt($ch, CURLOPT_URL, "$test_url/post");
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_POST, true);
    curl_setopt($ch, CURLOPT_POSTFIELDS, $post_data);
    curl_setopt($ch, CURLOPT_HTTPHEADER, ['Content-Type: application/json']);
    curl_setopt($ch, CURLOPT_TIMEOUT, $test_timeout);
    
    $response = curl_exec($ch);
    $http_code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    $error = curl_error($ch);
    
    curl_close($ch);
    
    $passed = ($http_code == 200) && empty($error);
    test_result($passed, "POST request with body", "HTTP Code: $http_code, Body size: " . strlen($post_data) . " bytes");
} catch (Exception $e) {
    test_result(false, "POST request with body", $e->getMessage());
}

// Test 3: Request with custom headers
echo "\nTest 3: Request with custom headers\n";
try {
    $ch = curl_init();
    curl_setopt($ch, CURLOPT_URL, "$test_url/headers");
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_HTTPHEADER, [
        'X-Test-Header: test-value',
        'User-Agent: OPA-Curl-Test/1.0'
    ]);
    curl_setopt($ch, CURLOPT_TIMEOUT, $test_timeout);
    
    $response = curl_exec($ch);
    $http_code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    
    curl_close($ch);
    
    $passed = ($http_code == 200 || $http_code == 301 || $http_code == 302);
    test_result($passed, "Request with custom headers", "HTTP Code: $http_code");
} catch (Exception $e) {
    test_result(false, "Request with custom headers", $e->getMessage());
}

// Test 4: Request with timing metrics
echo "\nTest 4: Request with timing metrics\n";
try {
    $ch = curl_init();
    curl_setopt($ch, CURLOPT_URL, "$test_url/delay/1");
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_TIMEOUT, $test_timeout);
    
    $start_time = microtime(true);
    $response = curl_exec($ch);
    $end_time = microtime(true);
    
    $http_code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    $total_time = curl_getinfo($ch, CURLINFO_TOTAL_TIME);
    $dns_time = curl_getinfo($ch, CURLINFO_NAMELOOKUP_TIME);
    $connect_time = curl_getinfo($ch, CURLINFO_CONNECT_TIME);
    $starttransfer_time = curl_getinfo($ch, CURLINFO_STARTTRANSFER_TIME);
    
    curl_close($ch);
    
    $passed = ($http_code == 200 || $http_code == 301 || $http_code == 302) && $total_time > 0;
    $details = sprintf("HTTP Code: %d, Total: %.3fms, DNS: %.3fms, Connect: %.3fms, StartTransfer: %.3fms",
        $http_code, $total_time * 1000, $dns_time * 1000, $connect_time * 1000, $starttransfer_time * 1000);
    test_result($passed, "Request with timing metrics", $details);
} catch (Exception $e) {
    test_result(false, "Request with timing metrics", $e->getMessage());
}

// Test 5: Request with byte measurements
echo "\nTest 5: Request with byte measurements\n";
try {
    $ch = curl_init();
    curl_setopt($ch, CURLOPT_URL, "$test_url/bytes/1024");
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_TIMEOUT, $test_timeout);
    
    $response = curl_exec($ch);
    $http_code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    $size_upload = curl_getinfo($ch, CURLINFO_SIZE_UPLOAD);
    $size_download = curl_getinfo($ch, CURLINFO_SIZE_DOWNLOAD);
    
    curl_close($ch);
    
    $passed = ($http_code == 200) && ($size_download > 0);
    $details = sprintf("HTTP Code: %d, Upload: %d bytes, Download: %d bytes",
        $http_code, $size_upload, $size_download);
    test_result($passed, "Request with byte measurements", $details);
} catch (Exception $e) {
    test_result(false, "Request with byte measurements", $e->getMessage());
}

// Test 6: Multiple sequential requests
echo "\nTest 6: Multiple sequential requests\n";
try {
    $success_count = 0;
    for ($i = 0; $i < 3; $i++) {
        $ch = curl_init();
        curl_setopt($ch, CURLOPT_URL, "$test_url/get?request=$i");
        curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
        curl_setopt($ch, CURLOPT_TIMEOUT, $test_timeout);
        
        $response = curl_exec($ch);
        $http_code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
        curl_close($ch);
        
        if ($http_code == 200 || $http_code == 301 || $http_code == 302) {
            $success_count++;
        }
    }
    
    $passed = $success_count == 3;
    test_result($passed, "Multiple sequential requests", "Success: $success_count/3");
} catch (Exception $e) {
    test_result(false, "Multiple sequential requests", $e->getMessage());
}

// Summary
echo "\n=== Test Summary ===\n";
echo "Tests passed: $tests_passed\n";
echo "Tests failed: $tests_failed\n";
echo "\n";

if ($tests_failed > 0) {
    echo "Some tests failed. Check the output above for details.\n";
    exit(1);
} else {
    echo "All tests passed!\n";
    exit(0);
}

