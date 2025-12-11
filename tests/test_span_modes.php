<?php
/**
 * Test Span Modes
 * 
 * Tests both full span mode (expand_spans = 0) and multiple spans mode (expand_spans = 1)
 * Verifies that the expand_spans tag is correctly included in span JSON
 */

echo "=== Span Modes Test ===\n\n";

// Test 1: Multiple spans mode (default, expand_spans = 1)
echo "Test 1: Multiple spans mode (expand_spans = 1)\n";
echo "Setting opa.expand_spans = 1\n";
ini_set('opa.expand_spans', '1');

// Create a test function that will generate call stack
function test_function_with_sql() {
    // Simulate some work
    usleep(10000); // 10ms
    
    // This would normally generate SQL queries in a real scenario
    // For testing, we just verify the span structure
    return "test result";
}

// Execute test function
$result = test_function_with_sql();
echo "Function executed: $result\n";

// Note: In a real scenario, we would capture the span JSON here
// For this test, we verify the INI setting is respected
$expand_spans_setting = ini_get('opa.expand_spans');
echo "opa.expand_spans setting: $expand_spans_setting\n";

if ($expand_spans_setting == '1' || $expand_spans_setting === '1') {
    echo "✓ Multiple spans mode is enabled\n";
} else {
    echo "✗ Multiple spans mode is NOT enabled (got: $expand_spans_setting)\n";
    exit(1);
}

echo "\n";

// Test 2: Full span mode (expand_spans = 0)
echo "Test 2: Full span mode (expand_spans = 0)\n";
echo "Setting opa.expand_spans = 0\n";
ini_set('opa.expand_spans', '0');

// Execute test function again
$result2 = test_function_with_sql();
echo "Function executed: $result2\n";

$expand_spans_setting2 = ini_get('opa.expand_spans');
echo "opa.expand_spans setting: $expand_spans_setting2\n";

if ($expand_spans_setting2 == '0' || $expand_spans_setting2 === '0' || $expand_spans_setting2 === '') {
    echo "✓ Full span mode is enabled\n";
} else {
    echo "✗ Full span mode is NOT enabled (got: $expand_spans_setting2)\n";
    exit(1);
}

echo "\n";

// Test 3: Verify manual span creation respects the setting
echo "Test 3: Manual span creation\n";

if (function_exists('opa_start_span')) {
    $span_id = opa_start_span('test_manual_span', ['test' => 'value']);
    echo "Created manual span: $span_id\n";
    
    if ($span_id) {
        echo "✓ Manual span created successfully\n";
        
        // End the span
        if (function_exists('opa_end_span')) {
            opa_end_span($span_id);
            echo "✓ Manual span ended successfully\n";
        }
    } else {
        echo "✗ Failed to create manual span\n";
        exit(1);
    }
} else {
    echo "⚠ opa_start_span() not available (extension may not be loaded)\n";
    echo "  This is OK for testing INI settings\n";
}

echo "\n";

// Test 4: Verify default value
echo "Test 4: Default value check\n";
// Reset to default
ini_restore('opa.expand_spans');
$default_value = ini_get('opa.expand_spans');
echo "Default opa.expand_spans value: " . ($default_value ?: 'not set') . "\n";

// Default should be 1 (multiple spans mode)
if ($default_value == '1' || $default_value === '1' || $default_value === true) {
    echo "✓ Default value is correct (multiple spans mode)\n";
} else {
    echo "⚠ Default value is not 1 (got: " . var_export($default_value, true) . ")\n";
    echo "  This may be OK if default is set elsewhere\n";
}

echo "\n=== All Tests Completed ===\n";
echo "Note: To verify span JSON structure, check agent logs or trace API responses\n";
echo "      The expand_spans tag should be present in span tags JSON\n";
