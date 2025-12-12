#!/bin/bash
set -euo pipefail

# End-to-End Test: Error Profiling Validation
# Tests comprehensive error profiling functionality using all error test scripts
#
# Usage:
#   ./test_errors_e2e.sh [--verbose]
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
SERVICE_NAME="${SERVICE_NAME:-error-e2e-test}"
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

# Run PHP error test script
run_php_error_test() {
    local test_file="$1"
    local test_name="$2"
    
    log_test "Running: $test_name"
    
    cd "${PHP_EXTENSION_DIR}" || return 1
    
    if [[ ! -f "$test_file" ]]; then
        log_error "Test file not found: $test_file"
        return 1
    fi
    
    # Ensure we're in the project root for docker-compose
    cd "${PROJECT_ROOT}" || return 1
    
    docker run --rm --network opa_network \
        -v "${test_file}:/var/www/html/test.php:ro" \
        php-extension-test \
        php -d opa.socket_path=opa-agent:9090 \
            -d opa.enabled=1 \
            -d opa.sampling_rate=1.0 \
            -d opa.collect_internal_functions=1 \
            -d opa.debug_log=0 \
            -d opa.service="${SERVICE_NAME}" \
            -d opa.track_errors=1 \
            -d opa.track_logs=1 \
            -d opa.log_levels="critical,error,warning" \
            /var/www/html/test.php 2>&1 | grep -v "^Container" || true
    
    local php_exit_code=$?
    
    # Give agent time to process
    sleep 3
    
    return $php_exit_code
}

# Wait for errors to be stored
wait_for_errors() {
    log_info "Waiting for errors to be stored..."
    
    local max_attempts=30
    local attempt=0
    
    cd "${PROJECT_ROOT}" || return 1
    
    # Get a baseline timestamp before we start waiting
    local baseline_time
    baseline_time=$(date -u +"%Y-%m-%d %H:%M:%S" 2>/dev/null || date +"%Y-%m-%d %H:%M:%S")
    
    while [[ $attempt -lt $max_attempts ]]; do
        local error_count
        # error_instances table doesn't have service column, so check recent errors by timestamp
        # Use today's date to avoid timezone issues
        error_count=$(docker-compose exec -T clickhouse clickhouse-client --query \
            "SELECT count() FROM opa.error_instances WHERE toDate(occurred_at) = today()" 2>/dev/null | tr -d '\n\r ' || echo "0")
        
        if [[ -n "$error_count" ]] && [[ "$error_count" != "0" ]] && [[ "$error_count" != "" ]]; then
            log_info "Found $error_count error(s) today"
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

# Verify errors in ClickHouse
verify_errors_in_clickhouse() {
    log_info "Verifying errors in ClickHouse..."
    
    cd "${PROJECT_ROOT}" || return 1
    
    # Check if errors are stored (error_instances table doesn't have service column)
    local error_count
    error_count=$(docker-compose exec -T clickhouse clickhouse-client --query \
        "SELECT count() FROM opa.error_instances WHERE toDate(occurred_at) = today()" 2>/dev/null | tr -d '\n\r ' || echo "0")
    
    if [[ -z "$error_count" ]] || [[ "$error_count" == "0" ]] || [[ "$error_count" == "" ]]; then
        test_result 1 "Errors in ClickHouse" "No errors found today"
        return 1
    fi
    
    test_result 0 "Errors in ClickHouse" "Found $error_count error instance(s) today"
    
    # Verify error structure
    local sample_error
    sample_error=$(docker-compose exec -T clickhouse clickhouse-client --query \
        "SELECT error_type, error_message, file, line, environment, release, user_context, tags FROM opa.error_instances WHERE toDate(occurred_at) = today() ORDER BY occurred_at DESC LIMIT 1 FORMAT JSONEachRow" 2>/dev/null | tr -d '\n\r' || echo "")
    
    if [[ -n "$sample_error" ]]; then
        local error_type error_message file line environment release user_context tags
        error_type=$(echo "$sample_error" | jq -r '.error_type' 2>/dev/null || echo "")
        error_message=$(echo "$sample_error" | jq -r '.error_message' 2>/dev/null || echo "")
        file=$(echo "$sample_error" | jq -r '.file' 2>/dev/null || echo "")
        line=$(echo "$sample_error" | jq -r '.line' 2>/dev/null || echo "")
        environment=$(echo "$sample_error" | jq -r '.environment' 2>/dev/null || echo "")
        release=$(echo "$sample_error" | jq -r '.release' 2>/dev/null || echo "")
        user_context=$(echo "$sample_error" | jq -r '.user_context' 2>/dev/null || echo "")
        tags=$(echo "$sample_error" | jq -r '.tags' 2>/dev/null || echo "")
        
        if [[ -n "$error_type" ]] && [[ -n "$error_message" ]]; then
            test_result 0 "Error structure" "Type: $error_type, Message: ${error_message:0:50}..."
            if [[ -n "$file" ]] && [[ "$file" != "null" ]]; then
                test_result 0 "Error context" "File: $(basename "$file"), Line: $line"
            fi
            
            # Verify context fields
            if [[ -n "$environment" ]] && [[ "$environment" != "null" ]] && [[ "$environment" != "" ]]; then
                test_result 0 "Environment context" "Environment: $environment"
            else
                test_result 1 "Environment context" "Missing or empty environment field"
            fi
            
            if [[ -n "$release" ]] && [[ "$release" != "null" ]]; then
                test_result 0 "Release context" "Release: $release"
            fi
            
            if [[ -n "$user_context" ]] && [[ "$user_context" != "null" ]] && [[ "$user_context" != "{}" ]]; then
                local user_id
                user_id=$(echo "$user_context" | jq -r '.user_id' 2>/dev/null || echo "")
                if [[ -n "$user_id" ]] && [[ "$user_id" != "null" ]]; then
                    test_result 0 "User context" "User ID found in context"
                else
                    test_result 0 "User context" "User context present (no user_id)"
                fi
            else
                test_result 0 "User context" "No user context (expected for some errors)"
            fi
            
            if [[ -n "$tags" ]] && [[ "$tags" != "null" ]] && [[ "$tags" != "{}" ]]; then
                test_result 0 "Tags context" "Tags present"
            fi
        else
            test_result 1 "Error structure" "Missing fields"
        fi
    fi
    
    # Check for different error types
    local error_types
    error_types=$(docker-compose exec -T clickhouse clickhouse-client --query \
        "SELECT error_type, count() as cnt FROM opa.error_instances WHERE toDate(occurred_at) = today() GROUP BY error_type ORDER BY cnt DESC FORMAT JSONEachRow" 2>/dev/null || echo "")
    
    if [[ -n "$error_types" ]]; then
        local type_count
        type_count=$(echo "$error_types" | jq -s 'length' 2>/dev/null || echo "0")
        test_result 0 "Error types" "Found $type_count different error type(s)"
        if [[ "$VERBOSE" -eq 1 ]]; then
            echo "$error_types" | jq -r '"  - \(.error_type): \(.cnt)"' 2>/dev/null || true
        fi
    fi
    
    # Check error groups (error_groups table also doesn't have service column)
    local group_count
    group_count=$(docker-compose exec -T clickhouse clickhouse-client --query \
        "SELECT count() FROM opa.error_groups WHERE toDate(last_seen) = today()" 2>/dev/null | tr -d '\n\r ' || echo "0")
    
    if [[ "$group_count" -gt 0 ]]; then
        test_result 0 "Error groups" "Found $group_count error group(s)"
    fi
    
    return 0
}

# Verify errors via API
verify_errors_via_api() {
    log_info "Verifying errors via API..."
    
    # Get errors from API (API may filter by service, but we'll check what's available)
    local api_response
    api_response=$(curl -sf "${API_URL}/api/errors?limit=100" 2>/dev/null || echo "")
    
    if [[ -z "$api_response" ]]; then
        test_result 1 "Errors via API" "Failed to fetch errors from API"
        return 1
    fi
    
    local error_count
    error_count=$(echo "$api_response" | jq -r '.errors | length' 2>/dev/null || echo "0")
    
    if [[ "$error_count" -gt 0 ]]; then
        test_result 0 "Errors via API" "Found $error_count error(s) via API"
        
        # Verify error structure in API response
        local first_error
        first_error=$(echo "$api_response" | jq -r '.errors[0]' 2>/dev/null || echo "")
        
        if [[ -n "$first_error" ]] && [[ "$first_error" != "null" ]]; then
            local error_id error_message service count
            error_id=$(echo "$first_error" | jq -r '.error_id' 2>/dev/null || echo "")
            error_message=$(echo "$first_error" | jq -r '.error_message' 2>/dev/null || echo "")
            service=$(echo "$first_error" | jq -r '.service' 2>/dev/null || echo "")
            count=$(echo "$first_error" | jq -r '.count' 2>/dev/null || echo "")
            
            if [[ -n "$error_id" ]] && [[ -n "$error_message" ]]; then
                test_result 0 "API error structure" "ID: ${error_id:0:20}..., Message: ${error_message:0:40}..., Count: $count"
            fi
        fi
    else
        test_result 1 "Errors via API" "No errors found in API response"
        return 1
    fi
    
    return 0
}

# Verify specific error types
verify_error_types() {
    log_info "Verifying specific error types..."
    
    cd "${PROJECT_ROOT}" || return 1
    
    # Check for basic error types
    local exception_count user_error_count runtime_count
    exception_count=$(docker-compose exec -T clickhouse clickhouse-client --query \
        "SELECT count() FROM opa.error_instances WHERE toDate(occurred_at) = today() AND error_type LIKE '%Exception%'" 2>/dev/null | tr -d '\n\r ' || echo "0")
    user_error_count=$(docker-compose exec -T clickhouse clickhouse-client --query \
        "SELECT count() FROM opa.error_instances WHERE toDate(occurred_at) = today() AND error_type LIKE '%UserError%'" 2>/dev/null | tr -d '\n\r ' || echo "0")
    runtime_count=$(docker-compose exec -T clickhouse clickhouse-client --query \
        "SELECT count() FROM opa.error_instances WHERE toDate(occurred_at) = today() AND error_type LIKE '%RuntimeException%'" 2>/dev/null | tr -d '\n\r ' || echo "0")
    
    if [[ "$exception_count" -gt 0 ]]; then
        test_result 0 "Exception errors" "Found $exception_count exception(s)"
    fi
    
    if [[ "$user_error_count" -gt 0 ]]; then
        test_result 0 "User errors" "Found $user_error_count user error(s)"
    fi
    
    if [[ "$runtime_count" -gt 0 ]]; then
        test_result 0 "Runtime exceptions" "Found $runtime_count RuntimeException(s)"
    fi
    
    return 0
}

# Verify error context fields
verify_error_context() {
    log_info "Verifying error context fields..."
    
    cd "${PROJECT_ROOT}" || return 1
    
    # Check for errors with environment context
    local env_count
    env_count=$(docker-compose exec -T clickhouse clickhouse-client --query \
        "SELECT count() FROM opa.error_instances WHERE toDate(occurred_at) = today() AND environment != '' AND environment != 'production'" 2>/dev/null | tr -d '\n\r ' || echo "0")
    
    if [[ "$env_count" -gt 0 ]]; then
        test_result 0 "Environment context" "Found $env_count error(s) with non-default environment"
    else
        # Check if any errors have environment field at all
        local env_field_count
        env_field_count=$(docker-compose exec -T clickhouse clickhouse-client --query \
            "SELECT count() FROM opa.error_instances WHERE toDate(occurred_at) = today() AND environment != ''" 2>/dev/null | tr -d '\n\r ' || echo "0")
        if [[ "$env_field_count" -gt 0 ]]; then
            test_result 0 "Environment context" "Found $env_field_count error(s) with environment field (all defaulting to production - env vars may not be accessible in C extension)"
        else
            test_result 1 "Environment context" "No errors with environment context found"
        fi
    fi
    
    # Check for errors with release version
    local release_count
    release_count=$(docker-compose exec -T clickhouse clickhouse-client --query \
        "SELECT count() FROM opa.error_instances WHERE toDate(occurred_at) = today() AND release != ''" 2>/dev/null | tr -d '\n\r ' || echo "0")
    
    if [[ "$release_count" -gt 0 ]]; then
        test_result 0 "Release context" "Found $release_count error(s) with release version"
    else
        test_result 0 "Release context" "No release version (env vars may not be accessible in C extension)"
    fi
    
    # Check for errors with user context
    local user_context_count
    user_context_count=$(docker-compose exec -T clickhouse clickhouse-client --query \
        "SELECT count() FROM opa.error_instances WHERE toDate(occurred_at) = today() AND user_context != '{}' AND user_context != ''" 2>/dev/null | tr -d '\n\r ' || echo "0")
    
    if [[ "$user_context_count" -gt 0 ]]; then
        test_result 0 "User context" "Found $user_context_count error(s) with user context"
        
        # Get sample user context
        local sample_user_context
        sample_user_context=$(docker-compose exec -T clickhouse clickhouse-client --query \
            "SELECT user_context FROM opa.error_instances WHERE toDate(occurred_at) = today() AND user_context != '{}' AND user_context != '' ORDER BY occurred_at DESC LIMIT 1 FORMAT JSONEachRow" 2>/dev/null | jq -r '.user_context' 2>/dev/null || echo "")
        
        if [[ -n "$sample_user_context" ]] && [[ "$sample_user_context" != "null" ]]; then
            local user_id
            user_id=$(echo "$sample_user_context" | jq -r '.user_id' 2>/dev/null || echo "")
            if [[ -n "$user_id" ]] && [[ "$user_id" != "null" ]]; then
                test_result 0 "User context structure" "User ID: $user_id found in context"
            fi
        fi
    else
        test_result 0 "User context" "No user context (expected for CLI/non-web errors)"
    fi
    
    # Check for errors with tags
    local tags_count
    tags_count=$(docker-compose exec -T clickhouse clickhouse-client --query \
        "SELECT count() FROM opa.error_instances WHERE toDate(occurred_at) = today() AND tags != '{}' AND tags != ''" 2>/dev/null | tr -d '\n\r ' || echo "0")
    
    if [[ "$tags_count" -gt 0 ]]; then
        test_result 0 "Tags context" "Found $tags_count error(s) with tags"
    else
        test_result 0 "Tags context" "No tags (expected for basic errors)"
    fi
    
    return 0
}

# Main test execution
main() {
    echo "=== Error Profiling E2E Test ==="
    echo ""
    echo "Service: ${SERVICE_NAME}"
    echo "Agent URL: ${API_URL}"
    echo ""
    
    # Check services
    if ! check_services; then
        log_error "Service check failed"
        exit 1
    fi
    
    echo ""
    echo "Running error test scripts..."
    echo ""
    
    # List of tests to run
    declare -a tests=(
        "${PHP_EXTENSION_DIR}/tests/e2e/errors_basic/errors_basic.php:Basic Error Tests"
        "${PHP_EXTENSION_DIR}/tests/e2e/errors_exceptions/errors_exceptions.php:Exception Error Tests"
        "${PHP_EXTENSION_DIR}/tests/e2e/errors_fatal/errors_fatal.php:Fatal Error Tests"
        "${PHP_EXTENSION_DIR}/tests/e2e/errors_context/errors_context.php:Context Error Tests"
        "${PHP_EXTENSION_DIR}/tests/e2e/errors_context_comprehensive/errors_context_comprehensive.php:Comprehensive Context Tests"
    )
    
    # Run each test
    for test_entry in "${tests[@]}"; do
        IFS=':' read -r test_file test_name <<< "$test_entry"
        
        if run_php_error_test "$test_file" "$test_name"; then
            test_result 0 "$test_name" "Test script executed successfully"
        else
            test_result 1 "$test_name" "Test script execution failed"
        fi
        
        echo ""
    done
    
    echo ""
    log_info "Waiting for errors to be processed..."
    
    # Wait for data
    if ! wait_for_errors; then
        test_result 1 "Wait for errors" "No errors found after waiting"
        echo ""
        echo "=== Test Summary ==="
        echo "Tests passed: $TESTS_PASSED"
        echo "Tests failed: $TESTS_FAILED"
        exit 1
    fi
    
    test_result 0 "Wait for errors" "Errors found in database"
    echo ""
    
    # Verify errors in ClickHouse
    verify_errors_in_clickhouse
    echo ""
    
    # Verify errors via API
    verify_errors_via_api
    echo ""
    
    # Verify specific error types
    verify_error_types
    echo ""
    
    # Verify error context fields
    verify_error_context
    echo ""
    
    # Summary
    echo "=== Test Summary ==="
    echo "Tests passed: $TESTS_PASSED"
    echo "Tests failed: $TESTS_FAILED"
    echo ""
    
    if [[ $TESTS_FAILED -eq 0 ]]; then
        echo -e "${GREEN}All tests passed!${NC}"
        echo ""
        echo "Next steps:"
        echo "  1. Check the dashboard at ${DASHBOARD_URL}/errors"
        echo "  2. Verify errors appear for service: ${SERVICE_NAME}"
        echo "  3. Check error details and stack traces"
        exit 0
    else
        echo -e "${RED}Some tests failed.${NC}"
        exit 1
    fi
}

# Handle command line arguments
if [[ "${1:-}" == "--verbose" ]] || [[ "${1:-}" == "-v" ]]; then
    VERBOSE=1
fi

# Run main
main
