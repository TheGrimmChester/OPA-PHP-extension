#!/bin/bash
# Helper script to debug core dumps from host
# Usage: ./debug_core.sh [container_name]

CONTAINER_NAME=${1:-opa-php-debug}

echo "=== Finding core dumps in container ==="
docker exec $CONTAINER_NAME ls -lah /var/log/core-dumps/ 2>/dev/null || echo "No core dumps found"

echo ""
echo "=== Latest core dump analysis ==="
docker exec $CONTAINER_NAME /usr/local/bin/debug_core.sh

echo ""
echo "=== To debug interactively, run: ==="
echo "docker exec -it $CONTAINER_NAME /usr/local/bin/debug_interactive.sh /app/tests/e2e/curl/curl_profiling_test.php"

