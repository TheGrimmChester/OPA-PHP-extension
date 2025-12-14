#!/bin/bash
set -euo pipefail

# End-to-End Test for Span Expansion Feature
# Tests both multiple spans mode (expand_spans=1) and full span mode (expand_spans=0)
#
# Usage:
#   ./test_span_expansion_e2e.sh [--verbose]
#
# Environment variables:
#   API_URL: Agent API URL (default: http://localhost:8081)
#   MYSQL_HOST: MySQL host (default: mysql-test)
#   MYSQL_PORT: MySQL port (default: 3306)
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
# Default to opa-agent:8080 (internal Docker network) or localhost:8081 (host)
API_URL="${API_URL:-http://opa-agent:8080}"
MYSQL_HOST="${MYSQL_HOST:-mysql-test}"
MYSQL_PORT="${MYSQL_PORT:-3306}"
MYSQL_DATABASE="${MYSQL_DATABASE:-test_db}"
MYSQL_USER="${MYSQL_USER:-test_user}"
MYSQL_PASSWORD="${MYSQL_PASSWORD:-test_password}"
MYSQL_ROOT_PASSWORD="${MYSQL_ROOT_PASSWORD:-root_password}"
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

# Detect docker compose command
if command -v docker-compose >/dev/null 2>&1 && docker-compose version >/dev/null 2>&1; then
    DOCKER_COMPOSE="docker-compose"
elif docker compose version >/dev/null 2>&1; then
    DOCKER_COMPOSE="docker compose"
else
    log_error "Neither 'docker compose' nor 'docker-compose' is available"
    exit 1
fi

# Set docker-compose file path
DOCKER_COMPOSE_FILE="${PROJECT_ROOT}/docker-compose.test.yml"

# Cleanup function
cleanup() {
    local exit_code=$?
    cd "${PROJECT_ROOT}" || true
    
    # Stop test containers (only if not in CI container)
    if [[ -z "${DOCKER_CONTAINER:-}" ]]; then
        ${DOCKER_COMPOSE} -f "${DOCKER_COMPOSE_FILE}" down > /dev/null 2>&1 || true
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

# Start MySQL test database
start_mysql() {
    # Skip if already in container with services running
    if [[ -n "${DOCKER_CONTAINER:-}" ]]; then
        log_info "Running in container, MySQL should already be running"
        return 0
    fi
    
    cd "${PROJECT_ROOT}" || return 1
    
    log_info "Starting MySQL test database..."
    
    local compose_output
    compose_output=$(${DOCKER_COMPOSE} -f "${DOCKER_COMPOSE_FILE}" up -d mysql-test 2>&1) || true
    if [[ "$VERBOSE" -eq 1 ]]; then
        echo "$compose_output" | grep -v "Creating\|Created\|Starting\|Started" || true
    fi
    
    # Wait for MySQL to be ready
    local max_attempts=30
    local attempt=0
    while [[ $attempt -lt $max_attempts ]]; do
        if ${DOCKER_COMPOSE} -f "${DOCKER_COMPOSE_FILE}" exec -T mysql-test mysqladmin ping -h localhost -u root -p"${MYSQL_ROOT_PASSWORD}" --silent 2>/dev/null; then
            log_info "MySQL is ready"
            sleep 2
            
            # Create database and user
            ${DOCKER_COMPOSE} -f "${DOCKER_COMPOSE_FILE}" exec -T mysql-test mysql -u root -p"${MYSQL_ROOT_PASSWORD}" <<EOF 2>/dev/null || true
CREATE DATABASE IF NOT EXISTS ${MYSQL_DATABASE};
CREATE USER IF NOT EXISTS '${MYSQL_USER}'@'%' IDENTIFIED BY '${MYSQL_PASSWORD}';
GRANT ALL PRIVILEGES ON ${MYSQL_DATABASE}.* TO '${MYSQL_USER}'@'%';
FLUSH PRIVILEGES;
EOF
            sleep 2
            return 0
        fi
        sleep 1
        ((attempt++)) || true
    done
    
    log_error "MySQL failed to start after $max_attempts attempts"
    return 1
}

# Check if agent is running
check_agent() {
    log_info "Checking agent availability..."
    
    # Try to check from host first
    local test_url="${API_URL}"
    if [[ "$test_url" == *"opa-agent"* ]]; then
        # If using container name, try from within a test container
        log_info "Testing agent from Docker network..."
        if docker run --rm --network opa_network curlimages/curl:latest curl -s -f "${test_url}/api/stats" > /dev/null 2>&1; then
            log_info "Agent is available at ${test_url}"
            return 0
        fi
    else
        # Try from host
        local max_attempts=10
        local attempt=0
        
        while [[ $attempt -lt $max_attempts ]]; do
            if curl -s -f "${test_url}/api/stats" > /dev/null 2>&1; then
                log_info "Agent is available at ${test_url}"
                return 0
            fi
            sleep 1
            ((attempt++)) || true
        done
    fi
    
    log_warn "Agent check from host failed, will try from test container"
    return 0  # Continue anyway, test will fail if agent is really unavailable
}

# Run PHP test
run_php_test() {
    cd "${PHP_EXTENSION_DIR}" || return 1
    
    log_info "Running span expansion E2E test..."
    
    # Build PHP image if needed
    if ! docker images | grep -q "php-extension-test"; then
        log_info "Building PHP extension test image..."
        docker build -t php-extension-test -f Dockerfile . > /dev/null 2>&1 || {
            log_error "Failed to build PHP extension image"
            return 1
        }
    fi
    
    # Run test in PHP container
    # Configure OPA to use TCP connection to agent
    local agent_host_port="opa-agent:9090"
    
    log_info "Running PHP test with OPA_SOCKET_PATH=${agent_host_port}"
    
    local test_output
    test_output=$(${DOCKER_COMPOSE} -f "${DOCKER_COMPOSE_FILE}" run --rm \
        --entrypoint /usr/local/bin/docker-entrypoint-custom.sh \
        -e API_URL="${API_URL}" \
        -e MYSQL_HOST="${MYSQL_HOST}" \
        -e MYSQL_PORT="${MYSQL_PORT}" \
        -e MYSQL_DATABASE="${MYSQL_DATABASE}" \
        -e MYSQL_USER="${MYSQL_USER}" \
        -e MYSQL_PASSWORD="${MYSQL_PASSWORD}" \
        -e MYSQL_ROOT_PASSWORD="${MYSQL_ROOT_PASSWORD}" \
        -e OPA_ENABLED=1 \
        -e OPA_SOCKET_PATH="${agent_host_port}" \
        -e OPA_SAMPLING_RATE=1.0 \
        -e OPA_FULL_CAPTURE_THRESHOLD_MS=0 \
        -e OPA_STACK_DEPTH=50 \
        -e OPA_COLLECT_INTERNAL_FUNCTIONS=1 \
        -e OPA_DEBUG_LOG=1 \
        -e OPA_EXPAND_SPANS=1 \
        php php "${TESTS_DIR:-/app/tests}/e2e/span_expansion_simple/span_expansion_simple.php" 2>&1) || {
        log_error "PHP test execution failed"
        echo "$test_output"
        return 1
    }
    
    echo "$test_output"
    
    # Check exit code - look for success indicators
    if echo "$test_output" | grep -q "✓ expand_spans tag is present and correct"; then
        return 0
    elif echo "$test_output" | grep -q "expand_spans tag is present and correct"; then
        return 0
    else
        return 1
    fi
}

# Main execution
main() {
    echo "=== Span Expansion E2E Test ==="
    echo ""
    
    # Check agent
    if ! check_agent; then
        log_error "Agent check failed"
        exit 1
    fi
    
    # Start MySQL
    if ! start_mysql; then
        log_error "MySQL startup failed"
        exit 1
    fi
    
    # Run PHP test
    if run_php_test; then
        test_result 0 "Span Expansion E2E Test"
    else
        test_result 1 "Span Expansion E2E Test" "Test execution failed"
    fi
    
    # Summary
    echo ""
    echo "=== Test Summary ==="
    echo "Passed: $TESTS_PASSED"
    echo "Failed: $TESTS_FAILED"
    
    if [[ $TESTS_FAILED -gt 0 ]]; then
        exit 1
    fi
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --verbose)
            VERBOSE=1
            shift
            ;;
        --ci)
            CI_MODE=1
            shift
            ;;
        *)
            log_error "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Run main
main
