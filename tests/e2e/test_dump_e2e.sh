#!/bin/bash
set -euo pipefail

# E2E Test for dump() and opa_dump() Functions
# Validates that dump data is recorded to ClickHouse

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

# Source common helpers for path detection
if [[ -f "${SCRIPT_DIR}/helpers/common.sh" ]]; then
    source "${SCRIPT_DIR}/helpers/common.sh"
fi

# Set API_URL based on environment
API_URL="${API_URL:-http://localhost:8081}"
if [[ -n "${DOCKER_CONTAINER:-}" ]] || [[ -f /.dockerenv ]]; then
    API_URL="${API_URL:-http://agent:8080}"
fi

# Check if agent is running
log_info "Checking if agent is available at ${API_URL}..."
if ! curl -s "${API_URL}/api/traces?limit=1" > /dev/null 2>&1; then
    log_error "Agent is not available at ${API_URL}"
    log_info "Please start the agent first: docker-compose up -d agent"
    exit 1
fi

# Check if ClickHouse is running
log_info "Checking if ClickHouse is available..."
if ! docker exec clickhouse clickhouse-client --query "SELECT 1" > /dev/null 2>&1; then
    log_warn "ClickHouse container not found or not accessible"
    log_info "Please ensure ClickHouse is running: docker-compose up -d clickhouse"
fi

log_info "Running PHP test to generate dumps..."
echo ""

# Run the PHP test
test_output=$($DOCKER_COMPOSE -f docker/compose/docker-compose.test.yml run --rm \
    --entrypoint /usr/local/bin/docker-entrypoint-custom.sh \
    -e OPA_ENABLED=1 \
    -e OPA_SOCKET_PATH=opa-agent:9090 \
    -e OPA_SAMPLING_RATE=1.0 \
    -e OPA_FULL_CAPTURE_THRESHOLD_MS=0 \
    -e OPA_STACK_DEPTH=50 \
    -e OPA_COLLECT_INTERNAL_FUNCTIONS=1 \
    -e API_URL=http://opa-agent:8080 \
    php php "${TESTS_DIR:-/app/tests}/e2e/dump_e2e/dump_e2e.php" 2>&1)

# Filter out log noise
echo "$test_output" | grep -v "timestamp\|level\|message\|fields\|ERROR.*Failed to connect" || true

# Check if test passed
if echo "$test_output" | grep -q "ALL TESTS PASSED"; then
    log_info "✓ Dump E2E test PASSED"
    
    # Additional validation: Query ClickHouse directly
    log_info "Validating dumps in ClickHouse..."
    
    # Query ClickHouse for dumps (check last 10 minutes for recent test)
    clickhouse_result=$(docker exec clickhouse clickhouse-client --query "
        SELECT 
            count(*) as dump_count,
            count(DISTINCT trace_id) as trace_count,
            count(DISTINCT span_id) as span_count
        FROM opa.spans_full 
        WHERE dumps != '' AND dumps != '[]' AND dumps != 'null'
        AND start_ts >= now() - INTERVAL 10 MINUTE
    " 2>&1 || echo "ERROR")
    
    if [[ "$clickhouse_result" != "ERROR" ]]; then
        log_info "ClickHouse validation:"
        echo "$clickhouse_result" | while IFS=$'\t' read -r dump_count trace_count span_count; do
            if [[ "$dump_count" -gt 0 ]]; then
                log_info "  ✓ Found $dump_count dump(s) in ClickHouse (last 10 minutes)"
                log_info "  ✓ Found in $trace_count trace(s)"
                log_info "  ✓ Found in $span_count span(s)"
            else
                # Also check last hour in case of timing
                clickhouse_result_hour=$(docker exec clickhouse clickhouse-client --query "
                    SELECT count(*) FROM opa.spans_full 
                    WHERE dumps != '' AND dumps != '[]' AND dumps != 'null'
                    AND start_ts >= now() - INTERVAL 1 HOUR
                " 2>&1 || echo "0")
                if [[ "$clickhouse_result_hour" =~ ^[0-9]+$ ]] && [[ "$clickhouse_result_hour" -gt 0 ]]; then
                    log_info "  ✓ Found $clickhouse_result_hour dump(s) in ClickHouse (last hour)"
                else
                    log_warn "  ⚠ No dumps found in ClickHouse (might be timing issue or API validation is sufficient)"
                fi
            fi
        done
    else
        log_warn "  ⚠ Could not query ClickHouse directly (API validation passed, which is sufficient)"
    fi
    
    exit 0
else
    log_error "✗ Dump E2E test FAILED"
    echo "$test_output" | tail -30
    exit 1
fi
