<?php
/**
 * Test script to validate environment variable overrides
 * 
 * This script tests that OPA_* environment variables override INI settings
 * at runtime for both CLI and web modes.
 * 
 * Usage:
 *   # Test with environment variables
 *   OPA_ENABLED=0 OPA_SAMPLING_RATE=0.5 OPA_SERVICE=test-service php test_env_override.php
 * 
 *   # Test with different values
 *   OPA_ENABLED=1 OPA_SAMPLING_RATE=1.0 OPA_SOCKET_PATH=agent:9090 php test_env_override.php
 */

echo "=== OpenProfilingAgent Environment Variable Override Test ===\n\n";

// Check if extension is loaded
if (!extension_loaded('opa')) {
    echo "❌ ERROR: OPA extension is not loaded\n";
    echo "Please ensure the extension is installed and enabled.\n";
    exit(1);
}

echo "✓ OPA extension is loaded\n\n";

// Get current INI settings (before environment variable override)
echo "--- INI Settings (from php.ini) ---\n";
$ini_settings = [
    'opa.enabled' => ini_get('opa.enabled'),
    'opa.sampling_rate' => ini_get('opa.sampling_rate'),
    'opa.socket_path' => ini_get('opa.socket_path'),
    'opa.service' => ini_get('opa.service'),
    'opa.organization_id' => ini_get('opa.organization_id'),
    'opa.project_id' => ini_get('opa.project_id'),
    'opa.stack_depth' => ini_get('opa.stack_depth'),
    'opa.buffer_size' => ini_get('opa.buffer_size'),
    'opa.debug_log' => ini_get('opa.debug_log'),
];

foreach ($ini_settings as $key => $value) {
    echo sprintf("  %-30s = %s\n", $key, $value !== false ? $value : '(not set)');
}

echo "\n--- Environment Variables ---\n";
$env_vars = [
    'OPA_ENABLED',
    'OPA_SAMPLING_RATE',
    'OPA_SOCKET_PATH',
    'OPA_SERVICE',
    'OPA_ORGANIZATION_ID',
    'OPA_PROJECT_ID',
    'OPA_STACK_DEPTH',
    'OPA_BUFFER_SIZE',
    'OPA_DEBUG_LOG',
];

$env_set = false;
foreach ($env_vars as $env_var) {
    $value = getenv($env_var);
    if ($value !== false) {
        echo sprintf("  %-30s = %s\n", $env_var, $value);
        $env_set = true;
    }
}

if (!$env_set) {
    echo "  (no OPA_* environment variables set)\n";
}

echo "\n--- Runtime Values (after override) ---\n";

// Get extension info using php --ri
$extension_info = shell_exec('php --ri opa 2>&1');
if ($extension_info) {
    // Parse key values from extension info
    $lines = explode("\n", $extension_info);
    $runtime_values = [];
    foreach ($lines as $line) {
        if (preg_match('/^\s*(\w+(?:\.\w+)*)\s*=>\s*(.+)$/', $line, $matches)) {
            $key = trim($matches[1]);
            $value = trim($matches[2]);
            if (strpos($key, 'opa.') === 0) {
                $runtime_values[$key] = $value;
            }
        }
    }
    
    // Show key settings
    $key_settings = [
        'opa.enabled',
        'opa.sampling_rate',
        'opa.socket_path',
        'opa.service',
        'opa.organization_id',
        'opa.project_id',
        'opa.stack_depth',
        'opa.buffer_size',
        'opa.debug_log',
    ];
    
    foreach ($key_settings as $key) {
        if (isset($runtime_values[$key])) {
            echo sprintf("  %-30s = %s\n", $key, $runtime_values[$key]);
        } else {
            // Try to get from ini_get as fallback
            $value = ini_get($key);
            echo sprintf("  %-30s = %s\n", $key, $value !== false ? $value : '(not set)');
        }
    }
} else {
    echo "  (could not get extension info)\n";
}

echo "\n--- Test Results ---\n";

// Test 1: Check if OPA_ENABLE environment variable works
$opa_enable = getenv('OPA_ENABLE');
if ($opa_enable !== false) {
    echo "Testing OPA_ENABLE override...\n";
    $expected_enabled = ($opa_enable === '1' || strtolower($opa_enable) === 'true') ? '1' : '0';
    $actual_enabled = ini_get('opa.enabled');
    
    if ($actual_enabled == $expected_enabled) {
        echo "  ✓ OPA_ENABLE override works: enabled = $actual_enabled\n";
    } else {
        echo "  ❌ OPA_ENABLE override failed: expected $expected_enabled, got $actual_enabled\n";
    }
} else {
    echo "  (OPA_ENABLE not set, skipping test)\n";
}

// Test 2: Check if OPA_ENABLED environment variable overrides INI
$opa_enabled_env = getenv('OPA_ENABLED');
if ($opa_enabled_env !== false) {
    echo "Testing OPA_ENABLED override...\n";
    $expected = $opa_enabled_env;
    $actual = ini_get('opa.enabled');
    
    if ($actual == $expected) {
        echo "  ✓ OPA_ENABLED override works: enabled = $actual\n";
    } else {
        echo "  ❌ OPA_ENABLED override failed: expected $expected, got $actual\n";
    }
} else {
    echo "  (OPA_ENABLED not set, skipping test)\n";
}

// Test 3: Check if OPA_SAMPLING_RATE overrides INI
$opa_sampling_rate = getenv('OPA_SAMPLING_RATE');
if ($opa_sampling_rate !== false) {
    echo "Testing OPA_SAMPLING_RATE override...\n";
    $expected = (float)$opa_sampling_rate;
    $actual = (float)ini_get('opa.sampling_rate');
    
    if (abs($actual - $expected) < 0.001) {
        echo "  ✓ OPA_SAMPLING_RATE override works: rate = $actual\n";
    } else {
        echo "  ❌ OPA_SAMPLING_RATE override failed: expected $expected, got $actual\n";
    }
} else {
    echo "  (OPA_SAMPLING_RATE not set, skipping test)\n";
}

// Test 4: Check if OPA_SERVICE overrides INI
$opa_service = getenv('OPA_SERVICE');
if ($opa_service !== false) {
    echo "Testing OPA_SERVICE override...\n";
    $expected = $opa_service;
    $actual = ini_get('opa.service');
    
    if ($actual === $expected) {
        echo "  ✓ OPA_SERVICE override works: service = $actual\n";
    } else {
        echo "  ❌ OPA_SERVICE override failed: expected '$expected', got '$actual'\n";
    }
} else {
    echo "  (OPA_SERVICE not set, skipping test)\n";
}

// Test 5: Check if profiling is actually enabled/disabled
echo "\nTesting profiling status...\n";
if (function_exists('opa_is_enabled')) {
    $is_enabled = opa_is_enabled();
    $ini_enabled = ini_get('opa.enabled');
    
    echo "  opa_is_enabled() = " . ($is_enabled ? 'true' : 'false') . "\n";
    echo "  opa.enabled (INI) = $ini_enabled\n";
    
    // Check if they match (they should)
    if (($is_enabled && $ini_enabled == '1') || (!$is_enabled && $ini_enabled == '0')) {
        echo "  ✓ Profiling status matches INI setting\n";
    } else {
        echo "  ⚠ Profiling status does not match INI setting (this may be expected if OPA_ENABLE is set)\n";
    }
} else {
    echo "  (opa_is_enabled() function not available)\n";
}

echo "\n=== Test Complete ===\n";
echo "\nTo test with environment variables, run:\n";
echo "  OPA_ENABLED=0 OPA_SAMPLING_RATE=0.5 OPA_SERVICE=test-service php test_env_override.php\n";
echo "  OPA_ENABLE=1 OPA_SAMPLING_RATE=1.0 php test_env_override.php\n";

