#!/bin/bash
# Test script to run environment variable override tests in Docker
# This builds the extension in Docker and runs the tests

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ROOT_DIR="$(cd "$EXT_DIR/.." && pwd)"

echo "Building PHP extension in Docker..."
cd "$ROOT_DIR"

# Check if Docker is available
if ! command -v docker &> /dev/null; then
    echo "Error: Docker is not available"
    exit 1
fi

# Build the extension using Docker
echo "Building extension..."
docker build -t opa-php-test -f "$EXT_DIR/docker/Dockerfile" \
    --build-arg PHP_VERSION=8.4 \
    "$EXT_DIR" || {
    echo "Error: Docker build failed"
    exit 1
}

echo ""
echo "Running environment variable override tests..."
echo ""

# Run the test script in the container
docker run --rm \
    -v "$EXT_DIR/tests:/tests:ro" \
    -w /tests \
    opa-php-test \
    bash -c "
        # Test 1: No environment variables
        echo 'Test 1: No environment variables (INI defaults)'
        echo '----------------------------------------'
        php test_env_override.php
        echo ''
        
        # Test 2: OPA_ENABLED=0
        echo 'Test 2: OPA_ENABLED=0 (should disable profiling)'
        echo '----------------------------------------'
        OPA_ENABLED=0 php test_env_override.php
        echo ''
        
        # Test 3: OPA_ENABLED=1
        echo 'Test 3: OPA_ENABLED=1 (should enable profiling)'
        echo '----------------------------------------'
        OPA_ENABLED=1 php test_env_override.php
        echo ''
        
        # Test 4: OPA_ENABLE=1
        echo 'Test 4: OPA_ENABLE=1 (should enable profiling)'
        echo '----------------------------------------'
        OPA_ENABLE=1 php test_env_override.php
        echo ''
        
        # Test 5: Multiple environment variables
        echo 'Test 5: Multiple environment variables'
        echo '----------------------------------------'
        OPA_ENABLED=1 \
        OPA_SAMPLING_RATE=0.5 \
        OPA_SERVICE=test-service \
        OPA_ORGANIZATION_ID=test-org \
        OPA_PROJECT_ID=test-project \
        OPA_STACK_DEPTH=15 \
        OPA_BUFFER_SIZE=32768 \
        OPA_DEBUG_LOG=1 \
        php test_env_override.php
        echo ''
        
        # Test 6: OPA_ENABLE overrides OPA_ENABLED
        echo 'Test 6: OPA_ENABLE=1 overrides OPA_ENABLED=0'
        echo '----------------------------------------'
        OPA_ENABLED=0 OPA_ENABLE=1 php test_env_override.php
        echo ''
        
        echo '=========================================='
        echo 'All tests completed'
        echo '=========================================='
    "

echo ""
echo "Tests completed successfully!"

