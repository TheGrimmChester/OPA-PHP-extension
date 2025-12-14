#!/bin/bash
set -euo pipefail

# End-to-End Test for CI: HTTP Request Profiling with Mock Server
# Tests various status codes, response times, and sizes
#
# Usage:
#   ./test_curl_ci_e2e.sh [--verbose]
#
# Environment variables:
#   API_URL: Agent API URL (default: http://localhost:8081)
#   MOCK_SERVER_PORT: Mock HTTP server port (default: 8888)
#   CI_MODE: Set to '1' for CI mode (structured output)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Source common helpers for path detection
if [[ -f "${SCRIPT_DIR}/helpers/common.sh" ]]; then
    source "${SCRIPT_DIR}/helpers/common.sh"
else
    # Fallback if common.sh not available
    PHP_EXTENSION_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
    if [[ -d "${PHP_EXTENSION_DIR}/../agent" ]] && [[ -d "${PHP_EXTENSION_DIR}/../clickhouse" ]]; then
        PROJECT_ROOT="${PHP_EXTENSION_DIR}/.."
    else
        PROJECT_ROOT="${PHP_EXTENSION_DIR}"
    fi
    export PROJECT_ROOT
    export PHP_EXTENSION_DIR
fi

# Configuration
# API_URL is set by common.sh if sourced, otherwise use environment-aware default
if [[ -z "${API_URL:-}" ]]; then
    if [[ -n "${DOCKER_CONTAINER:-}" ]] || [[ -f /.dockerenv ]]; then
        API_URL="http://agent:8080"
    else
        API_URL="http://localhost:8081"
    fi
fi
MOCK_SERVER_PORT="${MOCK_SERVER_PORT:-8888}"
MOCK_SERVER_URL="http://localhost:${MOCK_SERVER_PORT}"
CI_MODE="${CI_MODE:-0}"
VERBOSE="${VERBOSE:-0}"

# Colors (disabled in CI mode)
if [[ "$CI_MODE" -eq 1 ]]; then
    RED=''
    GREEN=''
    YELLOW=''
    NC=''
else
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[1;33m'
    NC='\033[0m'
fi

# Test counters
TESTS_PASSED=0
TESTS_FAILED=0
MOCK_SERVER_PID=""

# Set docker-compose file path
DOCKER_COMPOSE_FILE="${PROJECT_ROOT}/docker-compose.test.yml"

# Cleanup function
cleanup() {
    local exit_code=$?
    cd "${PROJECT_ROOT}" || true
    
    # Stop mock server container (only if not in CI container)
    if [[ -z "${DOCKER_CONTAINER:-}" ]]; then
        docker-compose -f "${DOCKER_COMPOSE_FILE}" down > /dev/null 2>&1 || true
    fi
    
    return $exit_code
}

trap cleanup EXIT INT TERM

# Logging functions
log_info() {
    if [[ "$VERBOSE" -eq 1 ]] || [[ "$CI_MODE" -eq 1 ]]; then
        echo -e "${GREEN}[INFO]${NC} $*"
    fi
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $*" >&2
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $*" >&2
}

# Test result tracking
test_result() {
    local status=$1
    local test_name=$2
    local details="${3:-}"
    
    if [[ $status -eq 0 ]]; then
        if [[ "$CI_MODE" -eq 1 ]]; then
            echo "::notice title=Test Passed::$test_name"
        else
            echo -e "${GREEN}✓${NC} $test_name"
        fi
        if [[ -n "$details" ]]; then
            echo "  $details"
        fi
        ((TESTS_PASSED++)) || true
    else
        if [[ "$CI_MODE" -eq 1 ]]; then
            echo "::error title=Test Failed::$test_name: $details"
        else
            echo -e "${RED}✗${NC} $test_name"
        fi
        if [[ -n "$details" ]]; then
            echo -e "  ${RED}ERROR:${NC} $details"
        fi
        ((TESTS_FAILED++)) || true
    fi
}

# Start mock HTTP server
start_mock_server() {
    # Skip if already in container with services running
    if [[ -n "${DOCKER_CONTAINER:-}" ]]; then
        log_info "Running in container, checking if mock server is accessible..."
        if curl -sf "http://mock-http-server:8888/status/200" > /dev/null 2>&1; then
            log_info "Mock server is accessible"
            MOCK_SERVER_URL="http://mock-http-server:8888"
            return 0
        else
            log_warn "Mock server not accessible in container, may need to be started separately"
            return 1
        fi
    fi
    
    cd "${PROJECT_ROOT}" || return 1
    
    log_info "Starting mock HTTP server as Docker service..."
    
    # Start mock server using docker-compose
    docker-compose -f "${DOCKER_COMPOSE_FILE}" up -d mock-http-server 2>&1 | grep -v "Creating\|Created\|Starting\|Started" || true
    
    # Wait for server to be ready (check from host)
    local max_attempts=20
    local attempt=0
    while [[ $attempt -lt $max_attempts ]]; do
        if curl -sf "http://localhost:8888/status/200" > /dev/null 2>&1; then
            log_info "Mock server is ready"
            MOCK_SERVER_URL="http://mock-http-server:8888"
            return 0
        fi
        sleep 0.5
        ((attempt++)) || true
    done
    
    log_error "Mock server failed to start"
    if [[ "$VERBOSE" -eq 1 ]]; then
        docker-compose -f "${DOCKER_COMPOSE_FILE}" logs mock-http-server 2>&1 | tail -20
    fi
    return 1
}

# Check if services are running
check_services() {
    log_info "Checking required services..."
    
    # Check agent
    if ! curl -sf "${API_URL}/api/health" > /dev/null 2>&1; then
        log_error "Agent is not accessible at ${API_URL}"
        return 1
    fi
    log_info "Agent is accessible"
    
    return 0
}

# Run PHP test with various HTTP scenarios
run_php_test() {
    log_info "Running PHP curl test with mock server..."
    
    cd "${PHP_EXTENSION_DIR}" || return 1
    
    # Create comprehensive test script
    local test_script="${PHP_EXTENSION_DIR}/tests/e2e/curl/test_curl_ci_e2e.php"
    cat > "$test_script" << EOF
<?php
echo "Testing curl_exec with various scenarios...\n";

\$mock_url = getenv('MOCK_SERVER_URL') ?: 'http://localhost:8888';
\$test_results = [];

// Test 1: 200 OK with small response
\$ch = curl_init("\$mock_url/status/200?size=100");
curl_setopt(\$ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt(\$ch, CURLOPT_TIMEOUT, 10);
\$result = curl_exec(\$ch);
\$http_code = curl_getinfo(\$ch, CURLINFO_HTTP_CODE);
curl_close(\$ch);
\$test_results[] = ['status' => \$http_code == 200, 'name' => '200 OK small', 'code' => \$http_code];

// Test 2: 404 Not Found
\$ch = curl_init("\$mock_url/status/404");
curl_setopt(\$ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt(\$ch, CURLOPT_TIMEOUT, 10);
\$result = curl_exec(\$ch);
\$http_code = curl_getinfo(\$ch, CURLINFO_HTTP_CODE);
curl_close(\$ch);
\$test_results[] = ['status' => \$http_code == 404, 'name' => '404 Not Found', 'code' => \$http_code];

// Test 3: 500 Internal Server Error
\$ch = curl_init("\$mock_url/status/500");
curl_setopt(\$ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt(\$ch, CURLOPT_TIMEOUT, 10);
\$result = curl_exec(\$ch);
\$http_code = curl_getinfo(\$ch, CURLINFO_HTTP_CODE);
curl_close(\$ch);
\$test_results[] = ['status' => \$http_code == 500, 'name' => '500 Server Error', 'code' => \$http_code];

// Test 4: 201 Created (POST)
\$ch = curl_init("\$mock_url/status/201?status=201");
curl_setopt(\$ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt(\$ch, CURLOPT_POST, true);
curl_setopt(\$ch, CURLOPT_POSTFIELDS, json_encode(['test' => 'data']));
curl_setopt(\$ch, CURLOPT_HTTPHEADER, ['Content-Type: application/json']);
curl_setopt(\$ch, CURLOPT_TIMEOUT, 10);
\$result = curl_exec(\$ch);
\$http_code = curl_getinfo(\$ch, CURLINFO_HTTP_CODE);
curl_close(\$ch);
\$test_results[] = ['status' => \$http_code == 201, 'name' => '201 Created (POST)', 'code' => \$http_code];

// Test 5: Delayed response (200ms)
\$ch = curl_init("\$mock_url/status/200?delay=0.2&size=500");
curl_setopt(\$ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt(\$ch, CURLOPT_TIMEOUT, 10);
\$start = microtime(true);
\$result = curl_exec(\$ch);
\$duration = (microtime(true) - \$start) * 1000;
\$http_code = curl_getinfo(\$ch, CURLINFO_HTTP_CODE);
curl_close(\$ch);
\$test_results[] = ['status' => \$http_code == 200 && \$duration >= 180, 'name' => 'Delayed response (200ms)', 'code' => \$http_code, 'duration' => \$duration];

// Test 6: Large response (10KB)
\$ch = curl_init("\$mock_url/status/200?size=10240");
curl_setopt(\$ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt(\$ch, CURLOPT_TIMEOUT, 10);
\$result = curl_exec(\$ch);
\$http_code = curl_getinfo(\$ch, CURLINFO_HTTP_CODE);
\$size = strlen(\$result);
curl_close(\$ch);
\$test_results[] = ['status' => \$http_code == 200 && \$size >= 10000, 'name' => 'Large response (10KB)', 'code' => \$http_code, 'size' => \$size];

// Test 7: 301 Redirect
\$ch = curl_init("\$mock_url/status/301");
curl_setopt(\$ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt(\$ch, CURLOPT_FOLLOWLOCATION, false);
curl_setopt(\$ch, CURLOPT_TIMEOUT, 10);
\$result = curl_exec(\$ch);
\$http_code = curl_getinfo(\$ch, CURLINFO_HTTP_CODE);
curl_close(\$ch);
\$test_results[] = ['status' => \$http_code == 301, 'name' => '301 Redirect', 'code' => \$http_code];

// Test 8: 403 Forbidden
\$ch = curl_init("\$mock_url/status/403");
curl_setopt(\$ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt(\$ch, CURLOPT_TIMEOUT, 10);
\$result = curl_exec(\$ch);
\$http_code = curl_getinfo(\$ch, CURLINFO_HTTP_CODE);
curl_close(\$ch);
\$test_results[] = ['status' => \$http_code == 403, 'name' => '403 Forbidden', 'code' => \$http_code];

// Summary
\$passed = 0;
\$failed = 0;
foreach (\$test_results as \$test) {
    if (\$test['status']) {
        \$passed++;
        echo "✓ " . \$test['name'] . " (HTTP " . \$test['code'] . ")\n";
    } else {
        \$failed++;
        echo "✗ " . \$test['name'] . " (HTTP " . \$test['code'] . ")\n";
    }
}

echo "\nTest Summary: \$passed passed, \$failed failed\n";
exit(\$failed > 0 ? 1 : 0);
EOF
    
    log_info "Using mock server URL for container: $MOCK_SERVER_URL"
    
    MOCK_SERVER_URL="$MOCK_SERVER_URL" docker-compose -f "${DOCKER_COMPOSE_FILE}" run --rm \
        -e OPA_ENABLED=1 \
        -e OPA_SOCKET_PATH=opa-agent:9090 \
        -e OPA_SAMPLING_RATE=1.0 \
        -e OPA_COLLECT_INTERNAL_FUNCTIONS=1 \
        -e OPA_DEBUG_LOG=0 \
        -e OPA_CURL_CAPTURE_BODY=1 \
        -e OPA_SERVICE=curl-ci-test \
        -e MOCK_SERVER_URL="$MOCK_SERVER_URL" \
        php php "${TESTS_DIR:-/app/tests}/e2e/curl/test_curl_ci_e2e.php" 2>&1 | grep -v "^Container" || true
    
    local php_exit_code=$?
    rm -f "$test_script"
    
    # Give agent time to process
    sleep 2
    
    return $php_exit_code
}

# Wait for trace to be stored
wait_for_trace() {
    cd "${PROJECT_ROOT}" || return 1
    
    log_info "Waiting for trace to be stored..."
    
    local max_attempts=20
    local attempt=0
    
    while [[ $attempt -lt $max_attempts ]]; do
        local trace_id=""
        local query_result
        query_result=$(docker-compose exec -T clickhouse clickhouse-client --query \
            "SELECT trace_id FROM opa.spans_min WHERE service = 'curl-ci-test' ORDER BY start_ts DESC LIMIT 1" 2>&1)
        
        if [[ $? -eq 0 ]] && [[ -n "$query_result" ]] && [[ ! "$query_result" =~ "not running" ]]; then
            trace_id=$(echo "$query_result" | tr -d '\n\r ' | head -1)
        fi
        
        if [[ -z "${trace_id:-}" ]] || [[ "${trace_id:-}" == "" ]] || [[ "${trace_id:-}" == "null" ]]; then
            query_result=$(docker-compose exec -T clickhouse clickhouse-client --query \
                "SELECT trace_id FROM opa.spans_min WHERE start_ts > now() - INTERVAL 2 MINUTE ORDER BY start_ts DESC LIMIT 1" 2>&1)
            if [[ $? -eq 0 ]] && [[ -n "$query_result" ]] && [[ ! "$query_result" =~ "not running" ]]; then
                trace_id=$(echo "$query_result" | tr -d '\n\r ' | head -1)
            fi
        fi
        
        if [[ -n "${trace_id:-}" ]] && [[ "${trace_id:-}" != "" ]] && [[ "${trace_id:-}" != "null" ]]; then
            local trace_len=${#trace_id}
            if [[ $trace_len -gt 5 ]]; then
                echo "$trace_id"
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

# Verify HTTP requests in trace
verify_http_requests() {
    local trace_id="$1"
    
    cd "${PROJECT_ROOT}" || return 1
    
    log_info "Verifying HTTP requests in trace $trace_id..."
    
    # Get trace from API
    local trace_json
    trace_json=$(curl -sf "${API_URL}/api/traces/${trace_id}/full" 2>/dev/null)
    
    if [[ -z "$trace_json" ]]; then
        test_result 1 "Get trace from API" "Failed to fetch trace"
        return 1
    fi
    
    test_result 0 "Get trace from API" "Trace fetched successfully"
    
    # Extract HTTP requests
    local http_requests
    http_requests=$(echo "$trace_json" | jq -r '[.spans[]?.http // .spans[]?.Http // empty | .[]?]' 2>/dev/null)
    
    if [[ -z "$http_requests" ]] || [[ "$http_requests" == "null" ]] || [[ "$http_requests" == "[]" ]]; then
        test_result 1 "HTTP requests in trace" "No HTTP requests found"
        return 1
    fi
    
    local request_count
    request_count=$(echo "$http_requests" | jq 'length' 2>/dev/null || echo "0")
    
    if [[ $request_count -lt 8 ]]; then
        test_result 1 "HTTP request count" "Expected at least 8 requests, found $request_count"
        return 1
    fi
    
    test_result 0 "HTTP request count" "Found $request_count HTTP request(s)"
    
    # Verify status codes
    local status_200 status_404 status_500 status_201 status_301 status_403
    status_200=$(echo "$http_requests" | jq '[.[] | select(.status_code == 200)] | length' 2>/dev/null || echo "0")
    status_404=$(echo "$http_requests" | jq '[.[] | select(.status_code == 404)] | length' 2>/dev/null || echo "0")
    status_500=$(echo "$http_requests" | jq '[.[] | select(.status_code == 500)] | length' 2>/dev/null || echo "0")
    status_201=$(echo "$http_requests" | jq '[.[] | select(.status_code == 201)] | length' 2>/dev/null || echo "0")
    status_301=$(echo "$http_requests" | jq '[.[] | select(.status_code == 301)] | length' 2>/dev/null || echo "0")
    status_403=$(echo "$http_requests" | jq '[.[] | select(.status_code == 403)] | length' 2>/dev/null || echo "0")
    
    local status_checks_passed=0
    if [[ $status_200 -gt 0 ]]; then
        ((status_checks_passed++)) || true
        test_result 0 "Status code 200" "Found $status_200 request(s)"
    else
        test_result 1 "Status code 200" "Not found"
    fi
    
    if [[ $status_404 -gt 0 ]]; then
        ((status_checks_passed++)) || true
        test_result 0 "Status code 404" "Found $status_404 request(s)"
    else
        test_result 1 "Status code 404" "Not found"
    fi
    
    if [[ $status_500 -gt 0 ]]; then
        ((status_checks_passed++)) || true
        test_result 0 "Status code 500" "Found $status_500 request(s)"
    else
        test_result 1 "Status code 500" "Not found"
    fi
    
    if [[ $status_201 -gt 0 ]]; then
        ((status_checks_passed++)) || true
        test_result 0 "Status code 201" "Found $status_201 request(s)"
    else
        test_result 1 "Status code 201" "Not found"
    fi
    
    # Verify response sizes
    local large_response
    large_response=$(echo "$http_requests" | jq '[.[] | select(.response_size >= 10000)] | length' 2>/dev/null || echo "0")
    
    if [[ $large_response -gt 0 ]]; then
        test_result 0 "Large response size" "Found $large_response request(s) with size >= 10KB"
    else
        test_result 1 "Large response size" "No requests with size >= 10KB found"
    fi
    
    # Verify timing (check for delayed response)
    local delayed_response
    delayed_response=$(echo "$http_requests" | jq '[.[] | select(.duration_ms >= 180)] | length' 2>/dev/null || echo "0")
    
    if [[ $delayed_response -gt 0 ]]; then
        test_result 0 "Delayed response timing" "Found $delayed_response request(s) with duration >= 180ms"
    else
        test_result 1 "Delayed response timing" "No requests with duration >= 180ms found"
    fi
    
    # Verify request structure
    local first_request
    first_request=$(echo "$http_requests" | jq '.[0]' 2>/dev/null)
    
    if [[ -n "$first_request" ]] && [[ "$first_request" != "null" ]]; then
        local has_url has_method has_status has_duration
        has_url=$(echo "$first_request" | jq -r 'has("url")' 2>/dev/null || echo "false")
        has_method=$(echo "$first_request" | jq -r 'has("method")' 2>/dev/null || echo "false")
        has_status=$(echo "$first_request" | jq -r 'has("status_code")' 2>/dev/null || echo "false")
        has_duration=$(echo "$first_request" | jq -r 'has("duration_ms")' 2>/dev/null || echo "false")
        
        if [[ "$has_url" == "true" ]] && [[ "$has_method" == "true" ]] && [[ "$has_status" == "true" ]] && [[ "$has_duration" == "true" ]]; then
            test_result 0 "HTTP request structure" "All required fields present"
        else
            test_result 1 "HTTP request structure" "Missing fields (url: $has_url, method: $has_method, status_code: $has_status, duration_ms: $has_duration)"
        fi
    fi
    
    return 0
}

# Main test execution
main() {
    echo "=== cURL CI End-to-End Test ==="
    echo ""
    
    # Check services
    if ! check_services; then
        log_error "Service check failed"
        exit 1
    fi
    
    # Start mock server
    if ! start_mock_server; then
        log_error "Failed to start mock server"
        exit 1
    fi
    
    echo ""
    echo "Running tests..."
    echo ""
    
    # Run PHP test
    if ! run_php_test; then
        log_error "PHP test failed"
        exit 1
    fi
    
    echo ""
    log_info "Waiting for trace to be stored..."
    
    # Wait for trace
    local trace_id
    trace_id=$(wait_for_trace)
    
    if [[ -z "$trace_id" ]]; then
        test_result 1 "Wait for trace" "No trace found after waiting"
        exit 1
    fi
    
    test_result 0 "Wait for trace" "Found trace: $trace_id"
    echo ""
    
    # Verify HTTP requests
    verify_http_requests "$trace_id"
    echo ""
    
    # Summary
    echo "=== Test Summary ==="
    echo "Tests passed: $TESTS_PASSED"
    echo "Tests failed: $TESTS_FAILED"
    echo ""
    
    if [[ $TESTS_FAILED -eq 0 ]]; then
        if [[ "$CI_MODE" -eq 1 ]]; then
            echo "::notice title=All Tests Passed::All $TESTS_PASSED tests passed"
        else
            echo -e "${GREEN}All tests passed!${NC}"
        fi
        exit 0
    else
        if [[ "$CI_MODE" -eq 1 ]]; then
            echo "::error title=Tests Failed::$TESTS_FAILED test(s) failed"
        else
            echo -e "${RED}Some tests failed.${NC}"
        fi
        exit 1
    fi
}

# Handle command line arguments
if [[ "${1:-}" == "--verbose" ]] || [[ "${1:-}" == "-v" ]]; then
    VERBOSE=1
fi

if [[ "${1:-}" == "--ci" ]] || [[ -n "${GITHUB_ACTIONS:-}" ]] || [[ -n "${CI:-}" ]]; then
    CI_MODE=1
fi

# Run main
main

