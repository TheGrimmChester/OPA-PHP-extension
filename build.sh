#!/bin/bash
set -e

# Build script for OpenProfilingAgent PHP extension
# Usage: ./build.sh [PHP_VERSION]
# Example: ./build.sh 8.4

PHP_VERSION=${1:-8.4}
EXT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="$EXT_DIR/../dist"

echo "Building OpenProfilingAgent extension for PHP $PHP_VERSION..."

# Check if PHP is available
if ! command -v phpize &> /dev/null; then
    echo "Error: phpize not found. Please install PHP development packages."
    exit 1
fi

# Check for required libraries
if ! pkg-config --exists liblz4; then
    echo "Warning: liblz4 not found. Compression will be disabled."
fi

# Clean previous build
cd "$EXT_DIR"
make clean 2>/dev/null || true
phpize --clean 2>/dev/null || true

# Build
phpize
./configure --enable-opa
make

# Create dist directory
mkdir -p "$DIST_DIR/php$PHP_VERSION"

# Copy .so file
if [ -f "modules/opa.so" ]; then
    cp "modules/opa.so" "$DIST_DIR/php$PHP_VERSION/opa.so"
    echo "✓ Extension built successfully: $DIST_DIR/php$PHP_VERSION/opa.so"
else
    echo "Error: Extension file not found"
    exit 1
fi

# Show extension info
echo ""
echo "Extension information:"
php -m | grep opa || echo "Extension not loaded (this is normal if not installed)"

