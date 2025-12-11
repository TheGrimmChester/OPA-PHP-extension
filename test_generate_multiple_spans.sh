#!/bin/bash
set -euo pipefail

# Test script to generate a trace with multiple spans using object method calls

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
$DOCKER_COMPOSE -f docker-compose.test.yml run --rm \
    --entrypoint /usr/local/bin/docker-entrypoint-custom.sh \
    -e OPA_ENABLED=1 \
    -e OPA_SOCKET_PATH=opa-agent:9090 \
    -e OPA_EXPAND_SPANS=1 \
    -e OPA_SAMPLING_RATE=1.0 \
    -e OPA_FULL_CAPTURE_THRESHOLD_MS=0 \
    -e OPA_STACK_DEPTH=50 \
    -e OPA_COLLECT_INTERNAL_FUNCTIONS=1 \
    -e MYSQL_HOST=mysql-test \
    -e MYSQL_PORT=3306 \
    -e MYSQL_DATABASE=test_db \
    -e MYSQL_USER=test_user \
    -e MYSQL_PASSWORD=test_password \
    -e MYSQL_ROOT_PASSWORD=root_password \
    php php /var/www/html/tests/test_generate_multiple_spans.php 2>&1 | grep -v "timestamp\|level\|message\|fields" || true

echo ""
log_info "Waiting for trace to be stored in ClickHouse..."
sleep 5

# Get the latest trace
log_info "Fetching latest trace..."
TRACE_ID=$(curl -s "http://localhost:8081/api/traces?limit=1" | jq -r '.traces[0].trace_id' 2>/dev/null)

if [ -z "$TRACE_ID" ] || [ "$TRACE_ID" = "null" ]; then
    log_error "No trace found"
    exit 1
fi

log_info "Found trace ID: $TRACE_ID"
echo ""

# Get full trace details
TRACE_DATA=$(curl -s "http://localhost:8081/api/traces/$TRACE_ID" 2>/dev/null)

SPAN_COUNT=$(echo "$TRACE_DATA" | jq '.spans | length' 2>/dev/null)
STACK_SIZE=$(echo "$TRACE_DATA" | jq '.root.stack | length' 2>/dev/null)
SQL_COUNT=$(echo "$TRACE_DATA" | jq '.root.sql | length' 2>/dev/null)
CHILDREN_COUNT=$(echo "$TRACE_DATA" | jq '.root.children | length' 2>/dev/null)
EXPAND_SPANS=$(echo "$TRACE_DATA" | jq '.root.tags.expand_spans' 2>/dev/null)

echo "=== Trace Analysis ==="
echo ""
echo "Trace ID: $TRACE_ID"
echo "Total spans: $SPAN_COUNT"
echo "Call stack size: $STACK_SIZE"
echo "SQL queries in root: $SQL_COUNT"
echo "Child spans: $CHILDREN_COUNT"
echo "expand_spans: $EXPAND_SPANS"
echo ""

if [ "$SPAN_COUNT" -gt 1 ]; then
    log_info "✓ SUCCESS: Multiple spans created!"
    echo ""
    echo "Span breakdown:"
    echo "$TRACE_DATA" | jq -r '.spans[] | "  - \(.name) (ID: \(.span_id), parent: \(.parent_id // "root"), SQL: \(.sql | length), duration: \(.duration_ms)ms)"' 2>/dev/null
    
    if [ "$STACK_SIZE" -gt 0 ]; then
        echo ""
        log_info "Call stack was captured and expanded into child spans"
    else
        echo ""
        log_warn "Call stack is empty, but multiple spans were still created"
    fi
else
    log_error "✗ FAILED: Only 1 span found"
    echo ""
    if [ "$STACK_SIZE" -eq 0 ]; then
        log_warn "Reason: Call stack is empty"
        log_info "The PHP extension may not be capturing call stacks properly"
        log_info "Try checking:"
        log_info "  - opa.collect_internal_functions=1"
        log_info "  - opa.stack_depth setting"
        log_info "  - PHP extension is properly loaded"
    else
        log_warn "Reason: Call stack exists but expansion didn't work"
        log_info "Check the expand_spans flag and agent code"
    fi
    exit 1
fi

echo ""
log_info "View full trace at: http://localhost:8081/api/traces/$TRACE_ID"
echo ""
log_info "Test completed successfully!"
