<?php
/**
 * Simple cURL test using aisenseapi.com
 */

echo "Testing curl_exec hook with https://aisenseapi.com/services/v1/ping\n\n";

$ch = curl_init("https://aisenseapi.com/services/v1/ping");
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch, CURLOPT_TIMEOUT, 10);

echo "Calling curl_exec...\n";
$result = curl_exec($ch);

if ($result === false) {
    echo "ERROR: curl_exec failed\n";
    echo "Error: " . curl_error($ch) . "\n";
} else {
    $http_code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    echo "SUCCESS: Result length: " . strlen($result) . " bytes\n";
    echo "HTTP Code: $http_code\n";
}

curl_close($ch);
echo "\nTest completed.\n";

