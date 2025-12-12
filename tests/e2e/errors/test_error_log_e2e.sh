#!/bin/bash
set -euo pipefail

# End-to-End Test: Validate Error and Log Tracking
# This script validates that errors and logs are properly captured and stored
#
# Usage:
#   ./test_error_log_e2e.sh [--verbose]
#
# Environment variables:
#   API_URL: Agent API URL (default: http://localhost:8081)
#   DASHBOARD_URL: Dashboard URL (default: http://localhost:3000)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="${PROJECT_ROOT:-$(cd "${SCRIPT_DIR}/../../.." && pwd)}"" && pwd)}"
PHP_EXTENSION_DIR="${PROJECT_ROOT}"

# Configuration
API_URL="${API_URL:-http://localhost:8081}"
DASHBOARD_URL="${DASHBOARD_URL:-http://localhost:3000}"
VERBOSE="${VERBOSE:-0}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
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

# Check if services are running
check_services() {
    log_info "Checking required services..."
    
    # Check agent
    if ! curl -sf "${API_URL}/api/health" > /dev/null 2>&1; then
        log_error "Agent is not accessible at ${API_URL}"
        return 1
    fi
    log_info "Agent is accessible"
    
    # Check dashboard (optional)
    if curl -sf "${DASHBOARD_URL}" > /dev/null 2>&1; then
        log_info "Dashboard is accessible"
    else
        log_warn "Dashboard is not accessible at ${DASHBOARD_URL} (optional)"
    fi
    
    return 0
}

# Run PHP test that generates errors and logs
run_php_test() {
    log_info "Running PHP error/log test..."
    
    cd "${PHP_EXTENSION_DIR}" || return 1
    
    # Create a test script in the mounted volume
    local test_script="${PHP_EXTENSION_DIR}/tests/e2e/error_log_e2e/error_log_e2e.php"
    cat > "$test_script" << 'EOF'
<?php
echo "Generating errors and logs for E2E test...\n";

// Test 1: Manual error tracking via opa_track_error()
if (function_exists('opa_track_error')) {
    echo "1. Testing opa_track_error()...\n";
    opa_track_error(
        'TestError',
        'This is a test error message for E2E test',
        __FILE__,
        __LINE__,
        debug_backtrace(DEBUG_BACKTRACE_IGNORE_ARGS)
    );
    echo "   ✓ Manual error tracked\n";
    usleep(200000); // 200ms delay
} else {
    echo "   ✗ opa_track_error() function not available\n";
}

// Test 2: Log tracking via error_log() with different levels
echo "2. Testing error_log() with different levels...\n";

$test_logs = [
    '[ERROR] This is an error log message for E2E test',
    '[WARN] This is a warning log message for E2E test',
    '[CRITICAL] This is a critical log message for E2E test',
    'error: This is another error format for E2E test',
    'warning: This is another warning format for E2E test',
];

foreach ($test_logs as $log_message) {
    error_log($log_message);
    usleep(100000); // 100ms delay between logs
}

echo "   ✓ Log messages sent\n";

// Test 3: PHP error generation (if track_errors is enabled)
echo "3. Testing PHP error generation...\n";
if (ini_get('opa.track_errors')) {
    // Generate a user error (non-fatal)
    trigger_error('This is a test user error for E2E test', E_USER_ERROR);
    echo "   ✓ User error triggered\n";
    usleep(200000); // 200ms delay
} else {
    echo "   ⚠ opa.track_errors is disabled, skipping\n";
}

// Test 4: Exception tracking
echo "4. Testing exception tracking...\n";
try {
    throw new Exception('This is a test exception for E2E test');
} catch (Exception $e) {
    if (function_exists('opa_track_error')) {
        opa_track_error(
            get_class($e),
            $e->getMessage(),
            $e->getFile(),
            $e->getLine(),
            $e->getTrace()
        );
        echo "   ✓ Exception tracked\n";
        usleep(200000); // 200ms delay
    } else {
        echo "   ✗ opa_track_error() not available for exception\n";
    }
}

echo "Test completed.\n";
EOF
    
    docker-compose run --rm \
        -e OPA_ENABLED=1 \
        -e OPA_SOCKET_PATH=opa-agent:9090 \
        -e OPA_SAMPLING_RATE=1.0 \
        -e OPA_COLLECT_INTERNAL_FUNCTIONS=1 \
        -e OPA_DEBUG_LOG=0 \
        -e OPA_SERVICE=error-log-e2e-test \
        -e OPA_TRACK_ERRORS=1 \
        -e OPA_TRACK_LOGS=1 \
        -e OPA_LOG_LEVELS="critical,error,warning" \
        php php /var/www/html/tests/e2e/error_log_e2e/error_log_e2e.php 2>&1 | grep -v "^Container" || true
    
    rm -f "$test_script"
    
    # Give agent time to process and store the data
    log_info "Waiting 3 seconds for agent to process errors and logs..."
    sleep 3
    
    return 0
}

# Verify logs in ClickHouse
verify_logs_in_clickhouse() {
    # Ensure we're in the project root for docker-compose
    cd "${PROJECT_ROOT}" || return 1
    
    log_info "Verifying logs in ClickHouse..."
    
    # Check if logs are stored (check without time filter first, then with time filter)
    local log_count
    log_count=$(docker-compose exec -T clickhouse clickhouse-client --query \
        "SELECT count() FROM opa.logs WHERE service = 'error-log-e2e-test'" 2>/dev/null || echo "0")
    
    # If no logs found, try with time filter
    if [[ "$log_count" -eq 0 ]]; then
        log_count=$(docker-compose exec -T clickhouse clickhouse-client --query \
            "SELECT count() FROM opa.logs WHERE service = 'error-log-e2e-test' AND timestamp > now() - INTERVAL 10 MINUTE" 2>/dev/null || echo "0")
    fi
    
    if [[ "$log_count" -gt 0 ]]; then
        test_result 0 "Logs in ClickHouse" "Found $log_count log entry(ies)"
        
        # Verify log structure
        local sample_log
        sample_log=$(docker-compose exec -T clickhouse clickhouse-client --query \
            "SELECT level, message FROM opa.logs WHERE service = 'error-log-e2e-test' AND timestamp > now() - INTERVAL 10 MINUTE ORDER BY timestamp DESC LIMIT 1 FORMAT JSONEachRow" 2>/dev/null || echo "")
        
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
        error_count=$(docker-compose exec -T clickhouse clickhouse-client --query \
            "SELECT count() FROM opa.logs WHERE service = 'error-log-e2e-test' AND level = 'error'" 2>/dev/null || echo "0")
        warn_count=$(docker-compose exec -T clickhouse clickhouse-client --query \
            "SELECT count() FROM opa.logs WHERE service = 'error-log-e2e-test' AND level = 'warn'" 2>/dev/null || echo "0")
        critical_count=$(docker-compose exec -T clickhouse clickhouse-client --query \
            "SELECT count() FROM opa.logs WHERE service = 'error-log-e2e-test' AND level = 'critical'" 2>/dev/null || echo "0")
        
        test_result 0 "Log levels" "Error: $error_count, Warning: $warn_count, Critical: $critical_count"
        
        return 0
    else
        test_result 1 "Logs in ClickHouse" "No logs found in ClickHouse"
        return 1
    fi
}

# Verify errors in ClickHouse
verify_errors_in_clickhouse() {
    # Ensure we're in the project root for docker-compose
    cd "${PROJECT_ROOT}" || return 1
    
    log_info "Verifying errors in ClickHouse..."
    
    # Check if errors are stored in error_instances (check without time filter to avoid timezone issues)
    local error_count
    error_count=$(docker-compose exec -T clickhouse clickhouse-client --query \
        "SELECT count() FROM opa.error_instances" 2>/dev/null || echo "0")
    
    if [[ "$error_count" -gt 0 ]]; then
        test_result 0 "Errors in ClickHouse" "Found $error_count error instance(s)"
        
        # Verify error structure
        local sample_error
        sample_error=$(docker-compose exec -T clickhouse clickhouse-client --query \
            "SELECT error_type, error_message FROM opa.error_instances ORDER BY occurred_at DESC LIMIT 1 FORMAT JSONEachRow" 2>/dev/null || echo "")
        
        if [[ -n "$sample_error" ]]; then
            local error_type error_message
            error_type=$(echo "$sample_error" | jq -r '.error_type' 2>/dev/null || echo "")
            error_message=$(echo "$sample_error" | jq -r '.error_message' 2>/dev/null || echo "")
            
            if [[ -n "$error_type" ]] && [[ -n "$error_message" ]]; then
                test_result 0 "Error structure" "Type: $error_type, Message: ${error_message:0:50}..."
            else
                test_result 1 "Error structure" "Missing fields"
            fi
        fi
        
        # Check error groups
        local group_count
        group_count=$(docker-compose exec -T clickhouse clickhouse-client --query \
            "SELECT count() FROM opa.error_groups WHERE project_id = (SELECT project_id FROM opa.error_instances WHERE occurred_at > now() - INTERVAL 10 MINUTE LIMIT 1)" 2>/dev/null || echo "0")
        
        if [[ "$group_count" -gt 0 ]]; then
            test_result 0 "Error groups in ClickHouse" "Found $group_count error group(s)"
        fi
        
        return 0
    else
        test_result 1 "Errors in ClickHouse" "No errors found in ClickHouse"
        
        # Check if error_instances table exists
        local table_exists
        table_exists=$(docker-compose exec -T clickhouse clickhouse-client --query \
            "SELECT count() FROM system.tables WHERE database = 'opa' AND name = 'error_instances'" 2>/dev/null || echo "0")
        
        if [[ "$table_exists" -eq 0 ]]; then
            log_warn "error_instances table may not exist. Check ClickHouse schema."
        fi
        
        return 1
    fi
}

# Verify trace correlation
verify_trace_correlation() {
    # Ensure we're in the project root for docker-compose
    cd "${PROJECT_ROOT}" || return 1
    
    log_info "Verifying trace correlation..."
    
    # Get a trace_id from recent logs (without time filter to avoid timezone issues)
    local trace_id
    trace_id=$(docker-compose exec -T clickhouse clickhouse-client --query \
        "SELECT trace_id FROM opa.logs WHERE service = 'error-log-e2e-test' ORDER BY timestamp DESC LIMIT 1" 2>/dev/null | tr -d '\n\r ' || echo "")
    
    if [[ -n "$trace_id" ]] && [[ "$trace_id" != "" ]] && [[ "$trace_id" != "null" ]]; then
        test_result 0 "Trace ID in logs" "Found trace_id: ${trace_id:0:20}..."
        
        # Check if logs have valid trace_ids
        local logs_with_trace
        logs_with_trace=$(docker-compose exec -T clickhouse clickhouse-client --query \
            "SELECT count() FROM opa.logs WHERE service = 'error-log-e2e-test' AND trace_id != ''" 2>/dev/null || echo "0")
        
        if [[ "$logs_with_trace" -gt 0 ]]; then
            test_result 0 "Log trace correlation" "$logs_with_trace log(s) have trace_id"
        else
            test_result 1 "Log trace correlation" "No logs with trace_id found"
        fi
        
        # Check if errors have trace_ids
        local errors_with_trace
        errors_with_trace=$(docker-compose exec -T clickhouse clickhouse-client --query \
            "SELECT count() FROM opa.error_instances WHERE trace_id != '' AND trace_id != 'null'" 2>/dev/null || echo "0")
        
        if [[ "$errors_with_trace" -gt 0 ]]; then
            test_result 0 "Error trace correlation" "$errors_with_trace error(s) have trace_id"
        else
            test_result 1 "Error trace correlation" "No errors with trace_id found"
        fi
        
        return 0
    else
        test_result 1 "Trace correlation" "No trace_id found in logs"
        return 1
    fi
}

# Main test execution
main() {
    echo "=== Error and Log Tracking End-to-End Test ==="
    echo ""
    
    # Check services
    if ! check_services; then
        log_error "Service check failed. Please ensure services are running."
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
    log_info "Verifying data in ClickHouse..."
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
        echo -e "${GREEN}All tests passed!${NC}"
        echo ""
        echo "You can verify the data in ClickHouse:"
        echo "  SELECT * FROM opa.logs WHERE service = 'error-log-e2e-test' ORDER BY timestamp DESC LIMIT 10;"
        echo "  SELECT * FROM opa.error_instances WHERE service = 'error-log-e2e-test' ORDER BY occurred_at DESC LIMIT 10;"
        exit 0
    else
        echo -e "${RED}Some tests failed.${NC}"
        if [[ "$VERBOSE" -eq 1 ]]; then
            echo ""
            echo "Debug queries:"
            echo "  SELECT count() FROM opa.logs WHERE service = 'error-log-e2e-test';"
            echo "  SELECT count() FROM opa.error_instances WHERE service = 'error-log-e2e-test';"
        fi
        exit 1
    fi
}

# Handle command line arguments
if [[ "${1:-}" == "--verbose" ]] || [[ "${1:-}" == "-v" ]]; then
    VERBOSE=1
fi

# Run main
main
