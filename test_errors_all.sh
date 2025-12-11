#!/bin/bash
set -euo pipefail

# Test Runner for All Error Test Scripts
# Runs all error test scripts sequentially to validate error profiling
#
# Usage:
#   ./test_errors_all.sh [--verbose] [--service SERVICE_NAME]
#
# Environment variables:
#   API_URL: Agent API URL (default: http://localhost:8081)
#   CLICKHOUSE_HOST: ClickHouse host (default: clickhouse)
#   CLICKHOUSE_PORT: ClickHouse port (default: 9000)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PHP_EXTENSION_DIR="${SCRIPT_DIR}"
TESTS_DIR="${PHP_EXTENSION_DIR}/tests"

# Configuration
API_URL="${API_URL:-http://localhost:8081}"
CLICKHOUSE_HOST="${CLICKHOUSE_HOST:-clickhouse}"
CLICKHOUSE_PORT="${CLICKHOUSE_PORT:-9000}"
SERVICE_NAME="${SERVICE_NAME:-error-test-service}"
VERBOSE="${VERBOSE:-0}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --verbose|-v)
            VERBOSE=1
            shift
            ;;
        --service)
            SERVICE_NAME="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            echo "Usage: $0 [--verbose] [--service SERVICE_NAME]"
            exit 1
            ;;
    esac
done

# Logging functions
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

# Test result tracking
test_result() {
    local status=$1
    local test_name=$2
    local details="${3:-}"
    
    ((TESTS_RUN++)) || true
    
    if [[ $status -eq 0 ]]; then
        echo -e "${GREEN}✓${NC} $test_name"
        if [[ -n "$details" ]] && [[ "$VERBOSE" -eq 1 ]]; then
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
    
    return 0
}

# Check if Docker network exists
check_docker_network() {
    if docker network inspect opa_network > /dev/null 2>&1; then
        return 0
    else
        log_warn "Docker network 'opa_network' not found. Tests may fail."
        return 1
    fi
}

# Check if PHP test image exists
check_php_image() {
    if docker images | grep -q "php-extension-test"; then
        return 0
    else
        log_warn "Docker image 'php-extension-test' not found."
        log_info "You may need to build it first or use a different approach."
        return 1
    fi
}

# Run a PHP test script
run_php_test() {
    local test_file="$1"
    local test_name="$2"
    
    if [[ ! -f "$test_file" ]]; then
        test_result 1 "$test_name" "Test file not found: $test_file"
        return 1
    fi
    
    log_test "Running: $test_name"
    
    # Try to run via Docker if available
    if check_docker_network && check_php_image; then
        if docker run --rm --network opa_network \
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
                /var/www/html/test.php > /tmp/test_output.log 2>&1; then
            test_result 0 "$test_name" "Test completed successfully"
            if [[ "$VERBOSE" -eq 1 ]]; then
                cat /tmp/test_output.log
            fi
            return 0
        else
            test_result 1 "$test_name" "Test execution failed"
            if [[ "$VERBOSE" -eq 1 ]]; then
                cat /tmp/test_output.log
            fi
            return 1
        fi
    else
        # Fallback: try to run directly if PHP and extension are available
        log_warn "Docker not available, trying direct PHP execution..."
        if command -v php > /dev/null 2>&1; then
            if php -d opa.enabled=1 \
                -d opa.track_errors=1 \
                -d opa.track_logs=1 \
                -d opa.service="${SERVICE_NAME}" \
                "$test_file" > /tmp/test_output.log 2>&1; then
                test_result 0 "$test_name" "Test completed (direct execution)"
                if [[ "$VERBOSE" -eq 1 ]]; then
                    cat /tmp/test_output.log
                fi
                return 0
            else
                test_result 1 "$test_name" "Direct execution failed"
                if [[ "$VERBOSE" -eq 1 ]]; then
                    cat /tmp/test_output.log
                fi
                return 1
            fi
        else
            test_result 1 "$test_name" "PHP not available and Docker not configured"
            return 1
        fi
    fi
}

# Wait for data to be processed
wait_for_processing() {
    log_info "Waiting for errors to be processed..."
    sleep 2
}

# Main execution
main() {
    echo "=== Error Test Suite Runner ==="
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
        "test_errors_basic.php:Basic Error Tests"
        "test_errors_exceptions.php:Exception Error Tests"
        "test_errors_fatal.php:Fatal Error Tests"
        "test_errors_context.php:Context Error Tests"
    )
    
    # Run each test
    for test_entry in "${tests[@]}"; do
        IFS=':' read -r test_file test_name <<< "$test_entry"
        test_path="${TESTS_DIR}/${test_file}"
        
        run_php_test "$test_path" "$test_name"
        wait_for_processing
        
        echo ""
    done
    
    # Summary
    echo "=== Test Summary ==="
    echo "Tests run: $TESTS_RUN"
    echo "Tests passed: $TESTS_PASSED"
    echo "Tests failed: $TESTS_FAILED"
    echo ""
    
    if [[ $TESTS_FAILED -eq 0 ]]; then
        echo -e "${GREEN}All tests passed!${NC}"
        echo ""
        echo "Next steps:"
        echo "  1. Check the dashboard at http://localhost:3000/errors"
        echo "  2. Verify errors appear for service: ${SERVICE_NAME}"
        echo "  3. Check error details and stack traces"
        exit 0
    else
        echo -e "${RED}Some tests failed.${NC}"
        exit 1
    fi
}

# Run main
main
