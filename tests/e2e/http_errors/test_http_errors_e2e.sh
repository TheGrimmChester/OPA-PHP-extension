#!/bin/bash
set -euo pipefail

# End-to-End Test: HTTP Error Status Codes (4xx and 5xx)
# Tests that HTTP requests with error status codes are properly tracked
#
# Usage:
#   ./test_http_errors_e2e.sh [--verbose]
#
# Environment variables:
#   API_URL: Agent API URL (default: http://localhost:8081)
#   BASE_URL: Base URL for test endpoint (default: http://localhost:8088)
#   SERVICE_NAME: Service name for test (default: http-errors-test)

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
# Detect if running in Docker container (use common.sh if available, otherwise detect here)
if [[ -n "${IN_CONTAINER:-}" ]]; then
    # Already set by common.sh
    :
elif [[ -f /.dockerenv ]] || [[ -n "${DOCKER_CONTAINER:-}" ]]; then
    API_URL="${API_URL:-http://agent:8080}"
    BASE_URL="${BASE_URL:-http://nginx-test}"
else
    API_URL="${API_URL:-http://localhost:8081}"
    BASE_URL="${BASE_URL:-http://localhost:8090}"
fi
SERVICE_NAME="${SERVICE_NAME:-http-errors-test}"
VERBOSE="${VERBOSE:-0}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Test counters
TESTS_PASSED=0
TESTS_FAILED=0

# Logging
log_info() {
    if [[ "$VERBOSE" -eq 1 ]] || [[ "${1:-}" == "force" ]]; then
        echo -e "${GREEN}[INFO]${NC} $*"
    fi
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $*" >&2
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $*" >&2
}

log_test() {
    echo -e "${BLUE}[TEST]${NC} $*"
}

# Test result
test_result() {
    local status=$1
    local test_name=$2
    local details="${3:-}"
    
    if [[ $status -eq 0 ]]; then
        echo -e "${GREEN}✓${NC} $test_name"
        if [[ -n "$details" ]]; then
            echo "  $details"
        fi
        ((TESTS_PASSED++)) || true
    else
        echo -e "${RED}✗${NC} $test_name"
        if [[ -n "$details" ]]; then
            echo -e "  ${RED}ERROR:${NC} $details"
        fi
        ((TESTS_FAILED++)) || true
    fi
}

# Setup test endpoint
setup_test_endpoint() {
    local endpoint_file="/home/xorix/myapm/tests/apps/symfony/public/test_http_errors.php"
    
    if [[ ! -f "$endpoint_file" ]]; then
        log_warn "Test endpoint not found at $endpoint_file"
        log_info "Creating test endpoint..."
        
        cat > "$endpoint_file" << 'ENDPOINT_EOF'
<?php
/**
 * Test endpoint for HTTP error status codes
 * Returns the status code specified in the 'status' query parameter
 */
$status = isset($_GET['status']) ? (int)$_GET['status'] : 200;
if ($status < 100 || $status > 599) $status = 500;
http_response_code($status);
header("Content-Type: application/json");
echo json_encode([
    "status" => $status,
    "message" => "Test error response",
    "timestamp" => time(),
    "method" => $_SERVER["REQUEST_METHOD"] ?? "GET",
    "uri" => $_SERVER["REQUEST_URI"] ?? "/",
], JSON_PRETTY_PRINT);
ENDPOINT_EOF
        
        if [[ -f "$endpoint_file" ]]; then
            log_info "Test endpoint created successfully"
        else
            log_error "Failed to create test endpoint. Please create it manually:"
            log_error "  sudo cp /tmp/test_http_errors.php $endpoint_file"
            return 1
        fi
    else
        log_info "Test endpoint already exists"
    fi
    
    return 0
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
    
    # Setup test endpoint
    if ! setup_test_endpoint; then
        log_warn "Could not setup test endpoint, but continuing..."
    fi
    
    # Check test endpoint
    if ! curl -sf "${BASE_URL}/test_http_errors.php?status=200" > /dev/null 2>&1; then
        log_warn "Test endpoint may not be accessible at ${BASE_URL}/test_http_errors.php"
        log_warn "Make sure the test endpoint is available and the web server is running"
        log_warn "You may need to create it manually:"
        log_warn "  sudo cp /tmp/test_http_errors.php /home/xorix/myapm/tests/apps/symfony/public/test_http_errors.php"
    else
        log_info "Test endpoint is accessible"
    fi
    
    return 0
}

# Run PHP test
run_php_test() {
    log_info "Running PHP HTTP errors test..."
    
    cd "${PHP_EXTENSION_DIR}" || return 1
    
    SERVICE_NAME="${SERVICE_NAME}" \
    BASE_URL="${BASE_URL}" \
    php -d opa.enabled=1 \
        -d opa.agent_url="${API_URL}" \
        -d opa.service="${SERVICE_NAME}" \
        "${SCRIPT_DIR}/test_http_errors_e2e.php"
    
    return $?
}

# Wait for HTTP requests to be processed
wait_for_http_requests() {
    log_info "Waiting for HTTP requests to be processed..."
    
    local max_attempts=30
    local attempt=0
    
    while [[ $attempt -lt $max_attempts ]]; do
        # Check for 4xx errors
        local count_4xx=$(curl -sf "${API_URL}/api/http-calls?service=${SERVICE_NAME}" 2>/dev/null | \
            jq -r '.http_calls[] | select(.status_code >= 400 and .status_code < 500) | .status_code' 2>/dev/null | wc -l || echo "0")
        
        # Check for 5xx errors
        local count_5xx=$(curl -sf "${API_URL}/api/http-calls?service=${SERVICE_NAME}" 2>/dev/null | \
            jq -r '.http_calls[] | select(.status_code >= 500) | .status_code' 2>/dev/null | wc -l || echo "0")
        
        if [[ "$count_4xx" -gt 0 ]] || [[ "$count_5xx" -gt 0 ]]; then
            log_info "Found HTTP error requests: 4xx=$count_4xx, 5xx=$count_5xx"
            return 0
        fi
        
        sleep 1
        ((attempt++)) || true
    done
    
    log_warn "No HTTP error requests found after waiting"
    return 1
}

# Verify HTTP errors in API
verify_http_errors_via_api() {
    log_test "Verifying HTTP errors via API..."
    
    local response
    response=$(curl -sf "${API_URL}/api/http-calls?service=${SERVICE_NAME}" 2>/dev/null)
    
    if [[ -z "$response" ]]; then
        test_result 1 "API Response" "Empty response from API"
        return 1
    fi
    
    # Check for 4xx errors
    local errors_4xx
    errors_4xx=$(echo "$response" | jq -r '.http_calls[] | select(.status_code >= 400 and .status_code < 500) | .status_code' 2>/dev/null | sort -u || echo "")
    
    # Check for 5xx errors
    local errors_5xx
    errors_5xx=$(echo "$response" | jq -r '.http_calls[] | select(.status_code >= 500) | .status_code' 2>/dev/null | sort -u || echo "")
    
    local has_4xx=false
    local has_5xx=false
    
    if [[ -n "$errors_4xx" ]]; then
        has_4xx=true
        log_info "Found 4xx errors: $(echo $errors_4xx | tr '\n' ' ')"
    fi
    
    if [[ -n "$errors_5xx" ]]; then
        has_5xx=true
        log_info "Found 5xx errors: $(echo $errors_5xx | tr '\n' ' ')"
    fi
    
    if [[ "$has_4xx" == true ]] && [[ "$has_5xx" == true ]]; then
        test_result 0 "HTTP Errors Verification" "Found both 4xx and 5xx errors"
        return 0
    elif [[ "$has_4xx" == true ]]; then
        test_result 0 "HTTP Errors Verification" "Found 4xx errors (no 5xx found)"
        return 0
    elif [[ "$has_5xx" == true ]]; then
        test_result 0 "HTTP Errors Verification" "Found 5xx errors (no 4xx found)"
        return 0
    else
        test_result 1 "HTTP Errors Verification" "No error status codes found"
        return 1
    fi
}

# Verify specific error codes
verify_specific_error_codes() {
    log_test "Verifying specific error codes..."
    
    local response
    response=$(curl -sf "${API_URL}/api/http-calls?service=${SERVICE_NAME}" 2>/dev/null)
    
    if [[ -z "$response" ]]; then
        test_result 1 "Specific Error Codes" "Empty response from API"
        return 1
    fi
    
    # Expected error codes
    local expected_4xx=(400 401 403 404 422)
    local expected_5xx=(500 502 503 504)
    
    local found_codes
    found_codes=$(echo "$response" | jq -r '.http_calls[] | .status_code' 2>/dev/null | sort -u || echo "")
    
    local missing_codes=()
    
    # Check 4xx codes
    for code in "${expected_4xx[@]}"; do
        if ! echo "$found_codes" | grep -q "^${code}$"; then
            missing_codes+=("4xx:$code")
        fi
    done
    
    # Check 5xx codes
    for code in "${expected_5xx[@]}"; do
        if ! echo "$found_codes" | grep -q "^${code}$"; then
            missing_codes+=("5xx:$code")
        fi
    done
    
    if [[ ${#missing_codes[@]} -eq 0 ]]; then
        test_result 0 "Specific Error Codes" "All expected error codes found"
        return 0
    else
        test_result 1 "Specific Error Codes" "Missing codes: ${missing_codes[*]}"
        return 1
    fi
}

# Main test execution
main() {
    echo "=== HTTP Errors E2E Test ==="
    echo ""
    echo "Service: ${SERVICE_NAME}"
    echo "Agent URL: ${API_URL}"
    echo "Base URL: ${BASE_URL}"
    echo ""
    
    # Check services
    if ! check_services; then
        log_error "Service check failed"
        exit 1
    fi
    
    echo ""
    log_test "Running HTTP errors test..."
    echo ""
    
    # Run PHP test
    if run_php_test; then
        test_result 0 "PHP Test Execution" "Test script executed successfully"
    else
        test_result 1 "PHP Test Execution" "Test script execution failed"
        echo ""
        echo "=== Test Summary ==="
        echo "Tests passed: $TESTS_PASSED"
        echo "Tests failed: $TESTS_FAILED"
        exit 1
    fi
    
    echo ""
    log_info "Waiting for HTTP requests to be processed..."
    
    # Wait for data
    if ! wait_for_http_requests; then
        test_result 1 "Wait for HTTP Requests" "No error requests found after waiting"
    else
        test_result 0 "Wait for HTTP Requests" "Error requests found"
    fi
    
    echo ""
    
    # Verify errors via API
    verify_http_errors_via_api
    echo ""
    
    # Verify specific error codes
    verify_specific_error_codes
    echo ""
    
    # Final summary
    echo "=== Test Summary ==="
    echo "Tests passed: $TESTS_PASSED"
    echo "Tests failed: $TESTS_FAILED"
    echo ""
    
    if [[ $TESTS_FAILED -eq 0 ]]; then
        echo -e "${GREEN}All tests passed!${NC}"
        exit 0
    else
        echo -e "${RED}Some tests failed!${NC}"
        exit 1
    fi
}

# Run main function
main "$@"

