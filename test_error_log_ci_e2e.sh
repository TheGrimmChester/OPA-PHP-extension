#!/bin/bash
set -euo pipefail

# End-to-End Test for CI: Error and Log Tracking
# Tests error and log tracking functionality
#
# Usage:
#   ./test_error_log_ci_e2e.sh [--verbose]
#
# Environment variables:
#   API_URL: Agent API URL (default: http://localhost:8081)
#   CLICKHOUSE_HOST: ClickHouse host (default: clickhouse)
#   CLICKHOUSE_PORT: ClickHouse port (default: 9000)
#   CLICKHOUSE_HTTP_PORT: ClickHouse HTTP port (default: 8123)
#   CI_MODE: Set to '1' for CI mode (structured output)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="${PROJECT_ROOT:-$(cd "${SCRIPT_DIR}/.." && pwd)}"
PHP_EXTENSION_DIR="${SCRIPT_DIR}"

# Configuration
API_URL="${API_URL:-http://localhost:8081}"
CLICKHOUSE_HOST="${CLICKHOUSE_HOST:-clickhouse}"
CLICKHOUSE_PORT="${CLICKHOUSE_PORT:-9000}"
CLICKHOUSE_HTTP_PORT="${CLICKHOUSE_HTTP_PORT:-8123}"
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

# Cleanup function
cleanup() {
    local exit_code=$?
    cd "${PHP_EXTENSION_DIR}" || true
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

# Query ClickHouse
clickhouse_query() {
    local query="$1"
    docker run --rm --network opa_network \
        clickhouse/clickhouse-client:23.3 \
        --host "${CLICKHOUSE_HOST}" \
        --port "${CLICKHOUSE_PORT}" \
        --query "$query" 2>/dev/null || echo ""
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
    
    # Check ClickHouse
    if ! clickhouse_query "SELECT 1" > /dev/null 2>&1; then
        log_error "ClickHouse is not accessible"
        return 1
    fi
    log_info "ClickHouse is accessible"
    
    return 0
}

# Run PHP test that generates errors and logs
run_php_test() {
    log_info "Running PHP error/log test..."
    
    cd "${PHP_EXTENSION_DIR}" || return 1
    
    # Use existing test script
    local test_script="${PHP_EXTENSION_DIR}/tests/error_log_tracking_test.php"
    
    if [[ ! -f "$test_script" ]]; then
        log_error "Test script not found: $test_script"
        return 1
    fi
    
    docker run --rm --network opa_network \
        -e OPA_ENABLED=1 \
        -e OPA_SOCKET_PATH=opa-agent:9090 \
        -e OPA_SAMPLING_RATE=1.0 \
        -e OPA_COLLECT_INTERNAL_FUNCTIONS=1 \
        -e OPA_DEBUG_LOG=0 \
        -e OPA_SERVICE=error-log-ci-test \
        -e OPA_TRACK_ERRORS=1 \
        -e OPA_TRACK_LOGS=1 \
        -e OPA_LOG_LEVELS="critical,error,warning" \
        -v "${test_script}:/var/www/html/test.php:ro" \
        php-extension-test \
        php /var/www/html/test.php 2>&1 | grep -v "^Container" || true
    
    local php_exit_code=$?
    
    # Give agent time to process
    sleep 3
    
    return $php_exit_code
}

# Wait for data to be stored
wait_for_data() {
    log_info "Waiting for data to be stored..."
    
    local max_attempts=20
    local attempt=0
    
    while [[ $attempt -lt $max_attempts ]]; do
        local log_count
        log_count=$(clickhouse_query "SELECT count() FROM opa.logs WHERE service = 'error-log-ci-test'" | tr -d '\n\r ' || echo "0")
        
        if [[ -n "$log_count" ]] && [[ "$log_count" != "0" ]] && [[ "$log_count" != "" ]]; then
            log_info "Found $log_count log(s)"
            return 0
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

# Verify logs in ClickHouse
verify_logs_in_clickhouse() {
    log_info "Verifying logs in ClickHouse..."
    
    # Check if logs are stored
    local log_count
    log_count=$(clickhouse_query "SELECT count() FROM opa.logs WHERE service = 'error-log-ci-test'" | tr -d '\n\r ' || echo "0")
    
    if [[ -z "$log_count" ]] || [[ "$log_count" == "0" ]] || [[ "$log_count" == "" ]]; then
        test_result 1 "Logs in ClickHouse" "No logs found in ClickHouse"
        return 1
    fi
    
    test_result 0 "Logs in ClickHouse" "Found $log_count log entry(ies)"
    
    # Verify log structure
    local sample_log
    sample_log=$(clickhouse_query "SELECT level, message FROM opa.logs WHERE service = 'error-log-ci-test' ORDER BY timestamp DESC LIMIT 1 FORMAT JSONEachRow" | tr -d '\n\r' || echo "")
    
    if [[ -n "$sample_log" ]]; then
        local level message
        level=$(echo "$sample_log" | jq -r '.level' 2>/dev/null || echo "")
        message=$(echo "$sample_log" | jq -r '.message' 2>/dev/null || echo "")
        
        if [[ -n "$level" ]] && [[ -n "$message" ]]; then
            test_result 0 "Log structure" "Level: $level, Message: ${message:0:50}..."
        else
            test_result 1 "Log structure" "Missing fields (level: $level, message: ${message:0:30}...)"
        fi
    fi
    
    # Check for different log levels
    local error_count warn_count critical_count
    error_count=$(clickhouse_query "SELECT count() FROM opa.logs WHERE service = 'error-log-ci-test' AND level = 'error'" | tr -d '\n\r ' || echo "0")
    warn_count=$(clickhouse_query "SELECT count() FROM opa.logs WHERE service = 'error-log-ci-test' AND level = 'warn'" | tr -d '\n\r ' || echo "0")
    critical_count=$(clickhouse_query "SELECT count() FROM opa.logs WHERE service = 'error-log-ci-test' AND level = 'critical'" | tr -d '\n\r ' || echo "0")
    
    test_result 0 "Log levels" "Error: $error_count, Warning: $warn_count, Critical: $critical_count"
    
    return 0
}

# Verify errors in ClickHouse
verify_errors_in_clickhouse() {
    log_info "Verifying errors in ClickHouse..."
    
    # Check if errors are stored in error_instances
    local error_count
    error_count=$(clickhouse_query "SELECT count() FROM opa.error_instances" | tr -d '\n\r ' || echo "0")
    
    if [[ -z "$error_count" ]] || [[ "$error_count" == "0" ]] || [[ "$error_count" == "" ]]; then
        test_result 1 "Errors in ClickHouse" "No errors found in ClickHouse"
        
        # Check if error_instances table exists
        local table_exists
        table_exists=$(clickhouse_query "SELECT count() FROM system.tables WHERE database = 'opa' AND name = 'error_instances'" | tr -d '\n\r ' || echo "0")
        
        if [[ "$table_exists" == "0" ]]; then
            log_warn "error_instances table may not exist. Check ClickHouse schema."
        fi
        
        return 1
    fi
    
    test_result 0 "Errors in ClickHouse" "Found $error_count error instance(s)"
    
    # Verify error structure
    local sample_error
    sample_error=$(clickhouse_query "SELECT error_type, error_message FROM opa.error_instances ORDER BY occurred_at DESC LIMIT 1 FORMAT JSONEachRow" | tr -d '\n\r' || echo "")
    
    if [[ -n "$sample_error" ]]; then
        local error_type error_message
        error_type=$(echo "$sample_error" | jq -r '.error_type' 2>/dev/null || echo "")
        error_message=$(echo "$sample_error" | jq -r '.error_message' 2>/dev/null || echo "")
        
        if [[ -n "$error_type" ]] && [[ -n "$error_message" ]]; then
            test_result 0 "Error structure" "Type: $error_type, Message: ${error_message:0:50}..."
        else
            test_result 1 "Error structure" "Missing fields (type: $error_type, message: ${error_message:0:30}...)"
        fi
    fi
    
    # Check error groups
    local group_count
    group_count=$(clickhouse_query "SELECT count() FROM opa.error_groups" | tr -d '\n\r ' || echo "0")
    
    if [[ "$group_count" -gt 0 ]]; then
        test_result 0 "Error groups in ClickHouse" "Found $group_count error group(s)"
    fi
    
    return 0
}

# Verify trace correlation
verify_trace_correlation() {
    log_info "Verifying trace correlation..."
    
    # Get a trace_id from recent logs
    local trace_id
    trace_id=$(clickhouse_query "SELECT trace_id FROM opa.logs WHERE service = 'error-log-ci-test' ORDER BY timestamp DESC LIMIT 1" | tr -d '\n\r ' || echo "")
    
    if [[ -z "$trace_id" ]] || [[ "$trace_id" == "" ]] || [[ "$trace_id" == "null" ]]; then
        test_result 1 "Trace ID in logs" "No trace_id found in logs"
        return 1
    fi
    
    test_result 0 "Trace ID in logs" "Found trace_id: ${trace_id:0:20}..."
    
    # Check if logs have valid trace_ids
    local logs_with_trace
    logs_with_trace=$(clickhouse_query "SELECT count() FROM opa.logs WHERE service = 'error-log-ci-test' AND trace_id != ''" | tr -d '\n\r ' || echo "0")
    
    if [[ "$logs_with_trace" -gt 0 ]]; then
        test_result 0 "Log trace correlation" "$logs_with_trace log(s) have trace_id"
    else
        test_result 1 "Log trace correlation" "No logs with trace_id found"
    fi
    
    # Check if errors have valid trace_ids
    local errors_with_trace
    errors_with_trace=$(clickhouse_query "SELECT count() FROM opa.error_instances WHERE trace_id != '' AND trace_id != 'null'" | tr -d '\n\r ' || echo "0")
    
    if [[ "$errors_with_trace" -gt 0 ]]; then
        test_result 0 "Error trace correlation" "$errors_with_trace error(s) have trace_id"
    else
        test_result 1 "Error trace correlation" "No errors with trace_id found"
    fi
    
    return 0
}

# Main test execution
main() {
    echo "=== Error and Log Tracking CI End-to-End Test ==="
    echo ""
    
    # Check services
    if ! check_services; then
        log_error "Service check failed"
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
    log_info "Waiting for data to be stored..."
    
    # Wait for data
    if ! wait_for_data; then
        test_result 1 "Wait for data" "No data found after waiting"
        exit 1
    fi
    
    test_result 0 "Wait for data" "Data found in ClickHouse"
    echo ""
    
    # Verify logs
    verify_logs_in_clickhouse
    echo ""
    
    # Verify errors
    verify_errors_in_clickhouse
    echo ""
    
    # Verify trace correlation
    verify_trace_correlation
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
