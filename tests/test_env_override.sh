#!/bin/bash
# Test script to validate environment variable overrides
# This script runs the PHP test with various environment variable combinations

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_FILE="$SCRIPT_DIR/test_env_override.php"

if [ ! -f "$TEST_FILE" ]; then
    echo "Error: Test file not found: $TEST_FILE"
    exit 1
fi

echo "=========================================="
echo "Environment Variable Override Tests"
echo "=========================================="
echo ""

# Test 1: No environment variables (should use INI defaults)
echo "Test 1: No environment variables (INI defaults)"
echo "----------------------------------------"
php "$TEST_FILE"
echo ""

# Test 2: OPA_ENABLED=0 (should disable profiling)
echo "Test 2: OPA_ENABLED=0 (should disable profiling)"
echo "----------------------------------------"
OPA_ENABLED=0 php "$TEST_FILE"
echo ""

# Test 3: OPA_ENABLED=1 (should enable profiling)
echo "Test 3: OPA_ENABLED=1 (should enable profiling)"
echo "----------------------------------------"
OPA_ENABLED=1 php "$TEST_FILE"
echo ""

# Test 4: OPA_ENABLE=1 (should enable profiling, overrides OPA_ENABLED)
echo "Test 4: OPA_ENABLE=1 (should enable profiling)"
echo "----------------------------------------"
OPA_ENABLE=1 php "$TEST_FILE"
echo ""

# Test 5: Multiple environment variables
echo "Test 5: Multiple environment variables"
echo "----------------------------------------"
OPA_ENABLED=1 \
OPA_SAMPLING_RATE=0.5 \
OPA_SERVICE=test-service \
OPA_ORGANIZATION_ID=test-org \
OPA_PROJECT_ID=test-project \
OPA_STACK_DEPTH=15 \
OPA_BUFFER_SIZE=32768 \
OPA_DEBUG_LOG=1 \
php "$TEST_FILE"
echo ""

# Test 6: OPA_ENABLE overrides OPA_ENABLED
echo "Test 6: OPA_ENABLE=1 overrides OPA_ENABLED=0"
echo "----------------------------------------"
OPA_ENABLED=0 OPA_ENABLE=1 php "$TEST_FILE"
echo ""

echo "=========================================="
echo "All tests completed"
echo "=========================================="

