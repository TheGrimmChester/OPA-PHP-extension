#!/bin/bash
set -euo pipefail

# E2E Test for Multiple Spans Feature
# Validates that PHP extension sends multiple span messages and agent stores them correctly

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if docker-compose is available
if ! command -v docker-compose &> /dev/null; then
    DOCKER_COMPOSE="docker compose"
else
    DOCKER_COMPOSE="docker-compose"
fi

# Check if agent is running
log_info "Checking if agent is available..."
if ! curl -s http://localhost:8081/api/traces?limit=1 > /dev/null 2>&1; then
    log_error "Agent is not available at http://localhost:8081"
    log_info "Please start the agent first: docker-compose up -d agent"
    exit 1
fi

# Check if MySQL is running
log_info "Checking if MySQL is available..."
if ! docker exec mysql-test mysqladmin ping -h localhost --silent 2>/dev/null; then
    log_warn "MySQL container not found, starting it..."
    $DOCKER_COMPOSE -f docker-compose.test.yml up -d mysql-test
    log_info "Waiting for MySQL to be ready..."
    sleep 5
fi

log_info "Running PHP test to generate multiple spans..."
echo ""

# Run the PHP test
test_output=$($DOCKER_COMPOSE -f docker-compose.test.yml run --rm \
    --entrypoint /usr/local/bin/docker-entrypoint-custom.sh \
    -e OPA_ENABLED=1 \
    -e OPA_SOCKET_PATH=opa-agent:9090 \
    -e OPA_EXPAND_SPANS=1 \
    -e OPA_SAMPLING_RATE=1.0 \
    -e OPA_FULL_CAPTURE_THRESHOLD_MS=0 \
    -e OPA_STACK_DEPTH=50 \
    -e OPA_COLLECT_INTERNAL_FUNCTIONS=1 \
    -e API_URL=http://opa-agent:8080 \
    -e MYSQL_HOST=mysql-test \
    -e MYSQL_PORT=3306 \
    -e MYSQL_DATABASE=test_db \
    -e MYSQL_USER=test_user \
    -e MYSQL_PASSWORD=test_password \
    -e MYSQL_ROOT_PASSWORD=root_password \
    php php /var/www/html/tests/test_newrelic_multiple_spans_e2e.php 2>&1)

# Filter out log noise
echo "$test_output" | grep -v "timestamp\|level\|message\|fields\|ERROR.*Failed to connect" || true

# Check if test passed
if echo "$test_output" | grep -q "ALL TESTS PASSED"; then
    log_info "✓ Multiple spans E2E test PASSED"
    exit 0
else
    log_error "✗ Multiple spans E2E test FAILED"
    echo "$test_output" | tail -20
    exit 1
fi
