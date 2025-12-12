#!/bin/bash
set -euo pipefail

# E2E Test for HTTP Request and Response Size Tracking
# Validates that request_size and response_size are tracked and stored in ClickHouse

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

API_URL="${API_URL:-http://localhost:8081}"
VERBOSE="${VERBOSE:-0}"

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

test_result() {
    local passed=$1
    local test_name="$2"
    local details="${3:-}"
    
    if [[ $passed -eq 0 ]]; then
        log_info "✓ $test_name"
        if [[ -n "$details" ]]; then
            echo "    $details"
        fi
        return 0
    else
        log_error "✗ $test_name"
        if [[ -n "$details" ]]; then
            echo "    $details"
        fi
        return 1
    fi
}

# Check if docker-compose is available
if ! command -v docker-compose &> /dev/null; then
    DOCKER_COMPOSE="docker compose"
else
    DOCKER_COMPOSE="docker-compose"
fi

# Check services
check_services() {
    log_info "Checking if agent is available..."
    if ! curl -sf "${API_URL}/api/traces?limit=1" > /dev/null 2>&1; then
        log_error "Agent is not available at ${API_URL}"
        log_info "Please start the agent first: docker-compose up -d agent"
        return 1
    fi
    
    log_info "Checking if ClickHouse is available..."
    if ! docker exec clickhouse clickhouse-client --query "SELECT 1" > /dev/null 2>&1; then
        log_warn "ClickHouse container not found or not accessible"
        log_info "Please ensure ClickHouse is running: docker-compose up -d clickhouse"
    fi
    
    return 0
}

# Wait for trace to appear in ClickHouse
wait_for_trace() {
    local max_attempts=30
    local attempt=0
    
    while [[ $attempt -lt $max_attempts ]]; do
        # Look for HTTP requests (method != CLI)
        local query_result=$(docker exec clickhouse clickhouse-client --query "
            SELECT trace_id FROM opa.spans_full 
            WHERE JSONExtractString(tags, 'http_request', 'method') != 'CLI'
              AND JSONExtractString(tags, 'http_request', 'method') != ''
              AND start_ts >= now() - INTERVAL 2 MINUTE 
            ORDER BY start_ts DESC 
            LIMIT 1
        " 2>/dev/null | tr -d '\n\r ' || echo "")
        
        # If no HTTP trace found, try any recent trace
        if [[ -z "$query_result" ]] || [[ "$query_result" == "" ]]; then
            query_result=$(docker exec clickhouse clickhouse-client --query "
                SELECT trace_id FROM opa.spans_full 
                WHERE start_ts >= now() - INTERVAL 2 MINUTE 
                ORDER BY start_ts DESC 
                LIMIT 1
            " 2>/dev/null | tr -d '\n\r ' || echo "")
        fi
        
        if [[ -n "${query_result:-}" ]] && [[ "${query_result:-}" != "" ]] && [[ "${query_result:-}" != "null" ]]; then
            local trace_len=${#query_result}
            if [[ $trace_len -gt 5 ]]; then
                echo "$query_result"
                return 0
            fi
        fi
        
        sleep 1
        ((attempt++)) || true
        if [[ "$VERBOSE" -eq 1 ]]; then
            echo -n "."
        fi
    done
    
    if [[ "$VERBOSE" -eq 1 ]]; then
        echo ""
    fi
    return 1
}

# Verify HTTP sizes in trace
verify_http_sizes() {
    local trace_id="$1"
    
    cd "${PROJECT_ROOT}" || return 1
    
    log_info "Verifying HTTP sizes in trace $trace_id..."
    
    # Get trace from API
    local trace_json
    trace_json=$(curl -sf "${API_URL}/api/traces/${trace_id}/full" 2>/dev/null)
    
    if [[ -z "$trace_json" ]]; then
        test_result 1 "Get trace from API" "Failed to fetch trace"
        return 1
    fi
    
    test_result 0 "Get trace from API" "Trace fetched successfully"
    
    # Extract request_size and response_size from root span tags
    local request_size
    local response_size
    
    request_size=$(echo "$trace_json" | jq -r '.root.tags.http_request.request_size // empty' 2>/dev/null || echo "")
    response_size=$(echo "$trace_json" | jq -r '.root.tags.http_response.response_size // empty' 2>/dev/null || echo "")
    
    local all_passed=0
    
    # Verify request_size is present
    if [[ -n "$request_size" ]] && [[ "$request_size" != "null" ]] && [[ "$request_size" != "" ]]; then
        local request_size_int
        request_size_int=$(echo "$request_size" | tr -d ' ' || echo "0")
        if [[ "$request_size_int" =~ ^[0-9]+$ ]] && [[ "$request_size_int" -gt 0 ]]; then
            test_result 0 "Request size present" "request_size=$request_size bytes"
        else
            test_result 1 "Request size invalid" "request_size=$request_size (not a positive number)"
            all_passed=1
        fi
    else
        test_result 1 "Request size missing" "request_size not found in tags.http_request"
        all_passed=1
    fi
    
    # Verify response_size is present
    if [[ -n "$response_size" ]] && [[ "$response_size" != "null" ]] && [[ "$response_size" != "" ]]; then
        local response_size_int
        response_size_int=$(echo "$response_size" | tr -d ' ' || echo "0")
        if [[ "$response_size_int" =~ ^[0-9]+$ ]] && [[ "$response_size_int" -gt 0 ]]; then
            test_result 0 "Response size present" "response_size=$response_size bytes"
        else
            test_result 1 "Response size invalid" "response_size=$response_size (not a positive number)"
            all_passed=1
        fi
    else
        test_result 1 "Response size missing" "response_size not found in tags.http_response"
        all_passed=1
    fi
    
    # Verify sizes in ClickHouse
    log_info "Verifying sizes in ClickHouse..."
    
    local clickhouse_request_size
    local clickhouse_response_size
    
    clickhouse_request_size=$(docker exec clickhouse clickhouse-client --query "
        SELECT JSONExtractString(tags, 'http_request', 'request_size')
        FROM opa.spans_full
        WHERE trace_id = '${trace_id}'
        LIMIT 1
    " 2>/dev/null | tr -d '\n\r ' || echo "")
    
    clickhouse_response_size=$(docker exec clickhouse clickhouse-client --query "
        SELECT JSONExtractString(tags, 'http_response', 'response_size')
        FROM opa.spans_full
        WHERE trace_id = '${trace_id}'
        LIMIT 1
    " 2>/dev/null | tr -d '\n\r ' || echo "")
    
    if [[ -n "$clickhouse_request_size" ]] && [[ "$clickhouse_request_size" != "" ]] && [[ "$clickhouse_request_size" != "null" ]]; then
        if [[ "$clickhouse_request_size" =~ ^[0-9]+$ ]] && [[ "$clickhouse_request_size" -gt 0 ]]; then
            test_result 0 "Request size in ClickHouse" "request_size=$clickhouse_request_size bytes"
            # Verify it matches API value
            if [[ "$clickhouse_request_size" == "$request_size_int" ]]; then
                test_result 0 "Request size matches" "API and ClickHouse values match"
            else
                test_result 1 "Request size mismatch" "API: $request_size_int, ClickHouse: $clickhouse_request_size"
                all_passed=1
            fi
        else
            test_result 1 "Request size invalid in ClickHouse" "request_size=$clickhouse_request_size"
            all_passed=1
        fi
    else
        test_result 1 "Request size missing in ClickHouse" "No request_size found"
        all_passed=1
    fi
    
    if [[ -n "$clickhouse_response_size" ]] && [[ "$clickhouse_response_size" != "" ]] && [[ "$clickhouse_response_size" != "null" ]]; then
        if [[ "$clickhouse_response_size" =~ ^[0-9]+$ ]] && [[ "$clickhouse_response_size" -gt 0 ]]; then
            test_result 0 "Response size in ClickHouse" "response_size=$clickhouse_response_size bytes"
            # Verify it matches API value
            if [[ "$clickhouse_response_size" == "$response_size_int" ]]; then
                test_result 0 "Response size matches" "API and ClickHouse values match"
            else
                test_result 1 "Response size mismatch" "API: $response_size_int, ClickHouse: $clickhouse_response_size"
                all_passed=1
            fi
        else
            test_result 1 "Response size invalid in ClickHouse" "response_size=$clickhouse_response_size"
            all_passed=1
        fi
    else
        test_result 1 "Response size missing in ClickHouse" "No response_size found"
        all_passed=1
    fi
    
    return $all_passed
}

# Main test execution
main() {
    echo "=== HTTP Request/Response Size E2E Test ==="
    echo ""
    
    # Check services
    if ! check_services; then
        log_error "Service check failed"
        exit 1
    fi
    
    log_info "Starting HTTP server (Nginx + PHP-FPM)..."
    cd "${PROJECT_ROOT}" || exit 1
    
    # Start services in background
    $DOCKER_COMPOSE -f docker/compose/docker-compose.test.yml up -d nginx php 2>&1 | grep -v "Creating\|Starting\|Created\|Started" || true
    
    # Wait for services to be ready
    log_info "Waiting for services to be ready..."
    max_wait=30
    wait_count=0
    while ! curl -sf "http://localhost:8090/health" > /dev/null 2>&1; do
        if [ $wait_count -ge $max_wait ]; then
            log_error "Nginx did not become ready in time"
            exit 1
        fi
        sleep 1
        wait_count=$((wait_count + 1))
    done
    
    log_info "HTTP server is ready"
    echo ""
    
    log_info "Making HTTP requests to test request/response sizes..."
    
    # Make GET request with query string
    log_info "Making HTTP GET request with query string..."
    test_output=$(curl -s -X GET "http://localhost:8090/e2e/http_sizes/test_http_sizes_e2e.php?test=get&param1=value1&param2=value2" \
        -H "User-Agent: E2E-Test/1.0" \
        -H "X-Test-Header: test-value" 2>&1)
    
    if [[ -n "$test_output" ]]; then
        log_info "✓ GET request successful"
        echo "  Response: $(echo "$test_output" | head -1 | cut -c1-80)..."
    else
        log_warn "⚠ GET request failed or returned empty"
    fi
    echo ""
    
    # Make POST request with body
    log_info "Making HTTP POST request with body..."
    POST_DATA='{"test":"post","data":"'$(python3 -c "print('X' * 200)")'"}'
    test_output=$(curl -s -X POST "http://localhost:8090/e2e/http_sizes/test_http_sizes_e2e.php" \
        -H "Content-Type: application/json" \
        -H "User-Agent: E2E-Test/1.0" \
        -d "$POST_DATA" 2>&1)
    
    if [[ -n "$test_output" ]]; then
        log_info "✓ POST request successful"
        echo "  Response: $(echo "$test_output" | head -1 | cut -c1-80)..."
    else
        log_warn "⚠ POST request failed or returned empty"
    fi
    echo ""
    
    # Filter out log noise
    echo "$test_output" | grep -v "timestamp\|level\|message\|fields\|ERROR.*Failed to connect" || true
    
    echo ""
    log_info "Waiting for trace to appear in ClickHouse..."
    
    # Wait for trace to appear
    trace_id=$(wait_for_trace)
    
    if [[ -z "$trace_id" ]]; then
        log_error "Failed to find trace in ClickHouse"
        exit 1
    fi
    
    log_info "Found trace: $trace_id"
    echo ""
    
    # Verify HTTP sizes
    if verify_http_sizes "$trace_id"; then
        echo ""
        log_info "✓ ALL TESTS PASSED: HTTP sizes are being tracked correctly!"
        exit 0
    else
        echo ""
        log_error "✗ SOME TESTS FAILED: HTTP sizes validation failed"
        exit 1
    fi
}

# Run main function
main "$@"

