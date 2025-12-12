<?php
/**
 * E2E Test: HTTP Error Status Codes (4xx and 5xx)
 * 
 * This test generates HTTP requests that result in 4xx and 5xx status codes
 * to verify that error HTTP requests are properly tracked and displayed.
 */

$serviceName = getenv('SERVICE_NAME') ?: 'http-errors-test';
$baseUrl = getenv('BASE_URL') ?: 'http://localhost:8088';

echo "=== HTTP Errors E2E Test ===\n";
echo "Service: {$serviceName}\n";
echo "Base URL: {$baseUrl}\n\n";

// Try to create a simple test endpoint if it doesn't exist
// This will be handled by the web server or we'll use a mock approach
$testEndpoint = "{$baseUrl}/test_http_errors.php";

$errors = [];

// Determine endpoint path based on base URL structure
$endpointPath = "/apps/symfony/public/test_http_errors.php";

// Test 4xx errors
echo "Testing 4xx errors...\n";

// 400 Bad Request
try {
    $ch = curl_init("{$baseUrl}{$endpointPath}?status=400");
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_TIMEOUT, 5);
    $response = curl_exec($ch);
    $httpCode = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    curl_close($ch);
    
    if ($httpCode == 400) {
        echo "✓ 400 Bad Request\n";
    } else {
        echo "✗ 400 Bad Request (got {$httpCode})\n";
        $errors[] = "400 test failed";
    }
} catch (Exception $e) {
    echo "✗ 400 Bad Request: {$e->getMessage()}\n";
    $errors[] = "400 test exception";
}

// 401 Unauthorized
try {
    $ch = curl_init("{$baseUrl}{$endpointPath}?status=401");
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_TIMEOUT, 5);
    $response = curl_exec($ch);
    $httpCode = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    curl_close($ch);
    
    if ($httpCode == 401) {
        echo "✓ 401 Unauthorized\n";
    } else {
        echo "✗ 401 Unauthorized (got {$httpCode})\n";
        $errors[] = "401 test failed";
    }
} catch (Exception $e) {
    echo "✗ 401 Unauthorized: {$e->getMessage()}\n";
    $errors[] = "401 test exception";
}

// 403 Forbidden
try {
    $ch = curl_init("{$baseUrl}{$endpointPath}?status=403");
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_TIMEOUT, 5);
    $response = curl_exec($ch);
    $httpCode = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    curl_close($ch);
    
    if ($httpCode == 403) {
        echo "✓ 403 Forbidden\n";
    } else {
        echo "✗ 403 Forbidden (got {$httpCode})\n";
        $errors[] = "403 test failed";
    }
} catch (Exception $e) {
    echo "✗ 403 Forbidden: {$e->getMessage()}\n";
    $errors[] = "403 test exception";
}

// 404 Not Found
try {
    $ch = curl_init("{$baseUrl}{$endpointPath}?status=404");
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_TIMEOUT, 5);
    $response = curl_exec($ch);
    $httpCode = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    curl_close($ch);
    
    if ($httpCode == 404) {
        echo "✓ 404 Not Found\n";
    } else {
        echo "✗ 404 Not Found (got {$httpCode})\n";
        $errors[] = "404 test failed";
    }
} catch (Exception $e) {
    echo "✗ 404 Not Found: {$e->getMessage()}\n";
    $errors[] = "404 test exception";
}

// 422 Unprocessable Entity
try {
    $ch = curl_init("{$baseUrl}{$endpointPath}?status=422");
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_TIMEOUT, 5);
    $response = curl_exec($ch);
    $httpCode = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    curl_close($ch);
    
    if ($httpCode == 422) {
        echo "✓ 422 Unprocessable Entity\n";
    } else {
        echo "✗ 422 Unprocessable Entity (got {$httpCode})\n";
        $errors[] = "422 test failed";
    }
} catch (Exception $e) {
    echo "✗ 422 Unprocessable Entity: {$e->getMessage()}\n";
    $errors[] = "422 test exception";
}

// Test 5xx errors
echo "\nTesting 5xx errors...\n";

// 500 Internal Server Error
try {
    $ch = curl_init("{$baseUrl}{$endpointPath}?status=500");
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_TIMEOUT, 5);
    $response = curl_exec($ch);
    $httpCode = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    curl_close($ch);
    
    if ($httpCode == 500) {
        echo "✓ 500 Internal Server Error\n";
    } else {
        echo "✗ 500 Internal Server Error (got {$httpCode})\n";
        $errors[] = "500 test failed";
    }
} catch (Exception $e) {
    echo "✗ 500 Internal Server Error: {$e->getMessage()}\n";
    $errors[] = "500 test exception";
}

// 502 Bad Gateway
try {
    $ch = curl_init("{$baseUrl}{$endpointPath}?status=502");
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_TIMEOUT, 5);
    $response = curl_exec($ch);
    $httpCode = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    curl_close($ch);
    
    if ($httpCode == 502) {
        echo "✓ 502 Bad Gateway\n";
    } else {
        echo "✗ 502 Bad Gateway (got {$httpCode})\n";
        $errors[] = "502 test failed";
    }
} catch (Exception $e) {
    echo "✗ 502 Bad Gateway: {$e->getMessage()}\n";
    $errors[] = "502 test exception";
}

// 503 Service Unavailable
try {
    $ch = curl_init("{$baseUrl}{$endpointPath}?status=503");
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_TIMEOUT, 5);
    $response = curl_exec($ch);
    $httpCode = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    curl_close($ch);
    
    if ($httpCode == 503) {
        echo "✓ 503 Service Unavailable\n";
    } else {
        echo "✗ 503 Service Unavailable (got {$httpCode})\n";
        $errors[] = "503 test failed";
    }
} catch (Exception $e) {
    echo "✗ 503 Service Unavailable: {$e->getMessage()}\n";
    $errors[] = "503 test exception";
}

// 504 Gateway Timeout
try {
    $ch = curl_init("{$baseUrl}{$endpointPath}?status=504");
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_TIMEOUT, 5);
    $response = curl_exec($ch);
    $httpCode = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    curl_close($ch);
    
    if ($httpCode == 504) {
        echo "✓ 504 Gateway Timeout\n";
    } else {
        echo "✗ 504 Gateway Timeout (got {$httpCode})\n";
        $errors[] = "504 test failed";
    }
} catch (Exception $e) {
    echo "✗ 504 Gateway Timeout: {$e->getMessage()}\n";
    $errors[] = "504 test exception";
}

// Test POST request with 4xx error
echo "\nTesting POST request with 4xx error...\n";
try {
    $ch = curl_init("{$baseUrl}{$endpointPath}?status=400");
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_POST, true);
    curl_setopt($ch, CURLOPT_POSTFIELDS, json_encode(['test' => 'data']));
    curl_setopt($ch, CURLOPT_HTTPHEADER, ['Content-Type: application/json']);
    curl_setopt($ch, CURLOPT_TIMEOUT, 5);
    $response = curl_exec($ch);
    $httpCode = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    curl_close($ch);
    
    if ($httpCode == 400) {
        echo "✓ POST 400 Bad Request\n";
    } else {
        echo "✗ POST 400 Bad Request (got {$httpCode})\n";
        $errors[] = "POST 400 test failed";
    }
} catch (Exception $e) {
    echo "✗ POST 400 Bad Request: {$e->getMessage()}\n";
    $errors[] = "POST 400 test exception";
}

// Summary
echo "\n=== Test Summary ===\n";
if (empty($errors)) {
    echo "✓ All tests passed!\n";
    exit(0);
} else {
    echo "✗ Some tests failed:\n";
    foreach ($errors as $error) {
        echo "  - {$error}\n";
    }
    exit(1);
}

