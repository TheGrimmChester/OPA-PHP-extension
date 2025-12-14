#!/bin/bash
set -euo pipefail

# End-to-End Test: PDO SQL Profiling with ClickHouse Verification
# Tests PDO SQL profiling and verifies data appears in ClickHouse database
#
# Usage:
#   ./test_pdo_sql_profiling_e2e.sh [--verbose]
#
# Environment variables:
#   API_URL: Agent API URL (default: http://localhost:8081)
#   MYSQL_HOST: MySQL host (default: mysql-test)
#   MYSQL_PORT: MySQL port (default: 3306)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# PROJECT_ROOT should point to php-extension directory (where docker-compose files are)
PROJECT_ROOT="${PROJECT_ROOT:-$(cd "${SCRIPT_DIR}/../.." && pwd)}"
PHP_EXTENSION_DIR="${PROJECT_ROOT}"

# Configuration
# API_URL is set by common.sh if sourced, otherwise use environment-aware default
if [[ -z "${API_URL:-}" ]]; then
    if [[ -n "${DOCKER_CONTAINER:-}" ]] || [[ -f /.dockerenv ]]; then
        API_URL="http://agent:8080"
    else
        API_URL="http://localhost:8081"
    fi
fi
MYSQL_HOST="${MYSQL_HOST:-mysql-test}"
MYSQL_PORT="${MYSQL_PORT:-3306}"
MYSQL_DATABASE="${MYSQL_DATABASE:-test_db}"
MYSQL_USER="${MYSQL_USER:-test_user}"
MYSQL_PASSWORD="${MYSQL_PASSWORD:-test_password}"
MYSQL_ROOT_PASSWORD="${MYSQL_ROOT_PASSWORD:-root_password}"
VERBOSE="${VERBOSE:-0}"
CI_MODE="${CI_MODE:-0}"

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

# Source common helpers for path detection
if [[ -f "${SCRIPT_DIR}/helpers/common.sh" ]]; then
    source "${SCRIPT_DIR}/helpers/common.sh"
else
    # Fallback if common.sh not available
    if [[ -d "${PHP_EXTENSION_DIR}/../agent" ]] && [[ -d "${PHP_EXTENSION_DIR}/../clickhouse" ]]; then
        PROJECT_ROOT="${PHP_EXTENSION_DIR}/.."
    else
        PROJECT_ROOT="${PHP_EXTENSION_DIR}"
    fi
    export PROJECT_ROOT
fi

# Set docker-compose file path
DOCKER_COMPOSE_FILE="${PROJECT_ROOT}/docker-compose.test.yml"

# Cleanup function
cleanup() {
    local exit_code=$?
    cd "${PROJECT_ROOT}" || true
    
    # Stop MySQL test container (only if not in CI container)
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
            
            # Create database and user if they don't exist
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
    
    log_error "MySQL failed to start"
    if [[ "$VERBOSE" -eq 1 ]]; then
        ${DOCKER_COMPOSE} -f "${DOCKER_COMPOSE_FILE}" logs mysql-test 2>&1 | tail -20
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

# Run PHP test with PDO SQL profiling
run_php_test() {
    log_info "Running PHP PDO SQL profiling test..."
    
    cd "${PHP_EXTENSION_DIR}" || return 1
    
    MYSQL_HOST="$MYSQL_HOST" \
    MYSQL_PORT="$MYSQL_PORT" \
    MYSQL_DATABASE="$MYSQL_DATABASE" \
    MYSQL_USER="$MYSQL_USER" \
    MYSQL_PASSWORD="$MYSQL_PASSWORD" \
    ${DOCKER_COMPOSE} -f "${DOCKER_COMPOSE_FILE}" run --rm \
        -e MYSQL_HOST="$MYSQL_HOST" \
        -e MYSQL_PORT="$MYSQL_PORT" \
        -e MYSQL_DATABASE="$MYSQL_DATABASE" \
        -e MYSQL_USER="$MYSQL_USER" \
        -e MYSQL_PASSWORD="$MYSQL_PASSWORD" \
        php php -d opa.socket_path=opa-agent:9090 \
            -d opa.enabled=1 \
            -d opa.sampling_rate=1.0 \
            -d opa.collect_internal_functions=1 \
            -d opa.debug_log=1 \
            -d opa.service=pdo-sql-profiling-test \
            /app/tests/e2e/sql/test_pdo_sql_profiling_e2e.php 2>&1 | grep -v "^Container" || true
    
    local php_exit_code=$?
    
    # Give agent time to process and batch traces
    sleep 8
    
    if [[ "$VERBOSE" -eq 1 ]] || [[ "$CI_MODE" -eq 1 ]]; then
        local agent_health
        agent_health=$(curl -sf "${API_URL}/api/health" 2>&1 || echo "Failed to check agent health")
        log_info "Agent health after test: $agent_health"
    fi
    
    return $php_exit_code
}

# Helper function to query ClickHouse
query_clickhouse() {
    local query="$1"
    local clickhouse_container
    local result
    local exit_code
    
    # Try to find ClickHouse container (GitHub Actions service)
    clickhouse_container=$(docker ps --filter "ancestor=clickhouse/clickhouse-server:23.3" --format "{{.ID}}" | head -1)
    
    if [[ -z "$clickhouse_container" ]]; then
        clickhouse_container=$(docker ps --filter "name=clickhouse" --format "{{.ID}}" | head -1)
    fi
    
    if [[ -n "$clickhouse_container" ]]; then
        result=$(docker exec "$clickhouse_container" clickhouse-client --query "$query" 2>&1)
        exit_code=$?
        
        if [[ $exit_code -ne 0 ]] && [[ "$VERBOSE" -eq 1 ]]; then
            log_warn "ClickHouse query failed (exit code: $exit_code): $result"
        fi
        
        echo "$result"
        return $exit_code
    else
        # Fallback: try docker-compose from main project
        if [[ -f "${PROJECT_ROOT}/docker-compose.yml" ]]; then
            result=$(cd "${PROJECT_ROOT}" && ${DOCKER_COMPOSE} exec -T clickhouse clickhouse-client --query "$query" 2>&1)
            exit_code=$?
        else
            local clickhouse_name
            clickhouse_name=$(docker ps --filter "name=clickhouse" --format "{{.Names}}" | head -1)
            if [[ -n "$clickhouse_name" ]]; then
                result=$(docker exec "$clickhouse_name" clickhouse-client --query "$query" 2>&1)
                exit_code=$?
            else
                result="ClickHouse container not found"
                exit_code=1
            fi
        fi
        
        if [[ $exit_code -ne 0 ]] && [[ "$VERBOSE" -eq 1 ]]; then
            log_warn "ClickHouse query failed via docker-compose (exit code: $exit_code): $result"
        fi
        
        echo "$result"
        return $exit_code
    fi
}

# Wait for trace to be stored
wait_for_trace() {
    cd "${PROJECT_ROOT}" || return 1
    
    log_info "Waiting for trace to be stored..."
    
    # First, verify ClickHouse is accessible
    local clickhouse_test
    clickhouse_test=$(query_clickhouse "SELECT 1" 2>&1)
    if [[ $? -ne 0 ]] || [[ "$clickhouse_test" =~ "not running" ]] || [[ "$clickhouse_test" =~ "error" ]] || [[ "$clickhouse_test" =~ "Exception" ]]; then
        log_error "ClickHouse is not accessible: $clickhouse_test"
        return 1
    fi
    
    # Check if table exists
    local table_check
    table_check=$(query_clickhouse "EXISTS opa.spans_min" 2>&1)
    if [[ $? -ne 0 ]] || [[ "$table_check" =~ "0" ]]; then
        log_warn "Table opa.spans_min may not exist. Checking for any spans table..."
        local any_table
        any_table=$(query_clickhouse "SHOW TABLES FROM opa" 2>&1)
        if [[ "$VERBOSE" -eq 1 ]] || [[ "$CI_MODE" -eq 1 ]]; then
            log_info "Available tables in opa database: $any_table"
        fi
    fi
    
    local max_attempts=45
    local attempt=0
    
    while [[ $attempt -lt $max_attempts ]]; do
        local trace_id=""
        local query_result
        query_result=$(query_clickhouse "SELECT trace_id FROM opa.spans_min WHERE service = 'pdo-sql-profiling-test' ORDER BY start_ts DESC LIMIT 1" 2>&1)
        local query_exit=$?
        
        if [[ "$VERBOSE" -eq 1 ]] && [[ $attempt -eq 0 ]]; then
            log_info "Query result (service filter): $query_result"
        fi
        
        if [[ $query_exit -eq 0 ]] && [[ -n "$query_result" ]] && [[ ! "$query_result" =~ "not running" ]] && [[ ! "$query_result" =~ "error" ]] && [[ ! "$query_result" =~ "Exception" ]]; then
            trace_id=$(echo "$query_result" | tr -d '\n\r ' | head -1)
        fi
        
        if [[ -z "${trace_id:-}" ]] || [[ "${trace_id:-}" == "" ]] || [[ "${trace_id:-}" == "null" ]]; then
            query_result=$(query_clickhouse "SELECT trace_id FROM opa.spans_min WHERE start_ts > now() - INTERVAL 2 MINUTE ORDER BY start_ts DESC LIMIT 1" 2>&1)
            query_exit=$?
            
            if [[ "$VERBOSE" -eq 1 ]] && [[ $attempt -eq 0 ]]; then
                log_info "Query result (time filter): $query_result"
            fi
            
            if [[ $query_exit -eq 0 ]] && [[ -n "$query_result" ]] && [[ ! "$query_result" =~ "not running" ]] && [[ ! "$query_result" =~ "error" ]] && [[ ! "$query_result" =~ "Exception" ]]; then
                trace_id=$(echo "$query_result" | tr -d '\n\r ' | head -1)
            fi
        fi
        
        # Diagnostic check at attempt 5
        if [[ $attempt -eq 5 ]] && [[ -z "${trace_id:-}" ]]; then
            local span_count
            span_count=$(query_clickhouse "SELECT count() FROM opa.spans_min WHERE start_ts > now() - INTERVAL 2 MINUTE" 2>&1)
            if [[ "$VERBOSE" -eq 1 ]] || [[ "$CI_MODE" -eq 1 ]]; then
                log_info "Total spans in last 2 minutes: $span_count"
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
    
    # Final diagnostic output if trace not found
    if [[ "$VERBOSE" -eq 1 ]] || [[ "$CI_MODE" -eq 1 ]]; then
        log_warn "Trace not found after $max_attempts attempts"
        local recent_traces
        recent_traces=$(query_clickhouse "SELECT trace_id, service, start_ts FROM opa.spans_min WHERE start_ts > now() - INTERVAL 5 MINUTE ORDER BY start_ts DESC LIMIT 5" 2>&1)
        log_info "Recent traces: $recent_traces"
    fi
    return 1
}

# Verify SQL profiling data in ClickHouse
verify_sql_in_clickhouse() {
    local trace_id="$1"
    
    cd "${PROJECT_ROOT}" || return 1
    
    log_info "Verifying SQL profiling data in ClickHouse for trace $trace_id..."
    
    # Check if SQL queries are stored in spans_full
    local sql_count
    sql_count=$(query_clickhouse "SELECT count() FROM opa.spans_full WHERE trace_id = '$trace_id' AND sql != '' AND sql != '[]'" 2>&1)
    
    if [[ "$sql_count" -gt 0 ]]; then
        test_result 0 "SQL queries in spans_full" "Found $sql_count span(s) with SQL queries"
        
        # Get sample SQL query data
        local sample_sql
        sample_sql=$(query_clickhouse "SELECT sql FROM opa.spans_full WHERE trace_id = '$trace_id' AND sql != '' AND sql != '[]' LIMIT 1 FORMAT JSONEachRow" 2>&1 | jq -r '.sql' 2>/dev/null || echo "")
        
        if [[ -n "$sample_sql" ]] && [[ "$sample_sql" != "[]" ]] && [[ "$sample_sql" != "null" ]]; then
            local query_count
            query_count=$(echo "$sample_sql" | jq 'length' 2>/dev/null || echo "0")
            
            if [[ $query_count -gt 0 ]]; then
                test_result 0 "SQL query array structure" "Sample span has $query_count SQL query(ies)"
                
                # Verify query structure for each query
                local valid_queries=0
                local queries_with_query_field=0
                local queries_with_duration=0
                local queries_with_type=0
                local queries_with_db_system=0
                local queries_with_db_host=0
                local queries_with_db_dsn=0
                
                for ((i=0; i<query_count; i++)); do
                    local query_obj
                    query_obj=$(echo "$sample_sql" | jq ".[$i]" 2>/dev/null)
                    
                    if [[ -n "$query_obj" ]] && [[ "$query_obj" != "null" ]]; then
                        local has_query has_duration has_type has_db_system has_db_host has_db_dsn
                        has_query=$(echo "$query_obj" | jq -r 'has("query")' 2>/dev/null || echo "false")
                        has_duration=$(echo "$query_obj" | jq -r 'has("duration_ms") or has("duration")' 2>/dev/null || echo "false")
                        has_type=$(echo "$query_obj" | jq -r 'has("query_type") or has("type")' 2>/dev/null || echo "false")
                        has_db_system=$(echo "$query_obj" | jq -r 'has("db_system")' 2>/dev/null || echo "false")
                        has_db_host=$(echo "$query_obj" | jq -r 'has("db_host")' 2>/dev/null || echo "false")
                        has_db_dsn=$(echo "$query_obj" | jq -r 'has("db_dsn")' 2>/dev/null || echo "false")
                        
                        if [[ "$has_query" == "true" ]]; then
                            ((queries_with_query_field++)) || true
                        fi
                        if [[ "$has_duration" == "true" ]]; then
                            ((queries_with_duration++)) || true
                        fi
                        if [[ "$has_type" == "true" ]]; then
                            ((queries_with_type++)) || true
                        fi
                        if [[ "$has_db_system" == "true" ]]; then
                            ((queries_with_db_system++)) || true
                        fi
                        if [[ "$has_db_host" == "true" ]]; then
                            ((queries_with_db_host++)) || true
                        fi
                        if [[ "$has_db_dsn" == "true" ]]; then
                            ((queries_with_dsn++)) || true
                        fi
                        
                        if [[ "$has_query" == "true" ]] && [[ "$has_duration" == "true" ]]; then
                            ((valid_queries++)) || true
                        fi
                    fi
                done
                
                # Verify required fields
                if [[ $queries_with_query_field -gt 0 ]]; then
                    test_result 0 "SQL query field" "Found $queries_with_query_field query(ies) with 'query' field"
                else
                    test_result 1 "SQL query field" "No queries found with 'query' field"
                fi
                
                if [[ $queries_with_duration -gt 0 ]]; then
                    test_result 0 "SQL duration field" "Found $queries_with_duration query(ies) with 'duration_ms' or 'duration' field"
                else
                    test_result 1 "SQL duration field" "No queries found with duration field"
                fi
                
                if [[ $queries_with_type -gt 0 ]]; then
                    test_result 0 "SQL query_type field" "Found $queries_with_type query(ies) with 'query_type' or 'type' field"
                else
                    test_result 1 "SQL query_type field" "No queries found with query_type field"
                fi
                
                # Verify database metadata fields
                if [[ $queries_with_db_system -gt 0 ]]; then
                    local db_system_value
                    db_system_value=$(echo "$sample_sql" | jq -r '[.[] | select(has("db_system"))][0].db_system' 2>/dev/null || echo "")
                    test_result 0 "SQL db_system field" "Found $queries_with_db_system query(ies) with 'db_system' field (value: $db_system_value)"
                else
                    test_result 1 "SQL db_system field" "No queries found with 'db_system' field"
                fi
                
                if [[ $queries_with_db_host -gt 0 ]]; then
                    local db_host_value
                    db_host_value=$(echo "$sample_sql" | jq -r '[.[] | select(has("db_host"))][0].db_host' 2>/dev/null || echo "")
                    test_result 0 "SQL db_host field" "Found $queries_with_db_host query(ies) with 'db_host' field (value: $db_host_value)"
                else
                    test_result 0 "SQL db_host field" "db_host may not be available for all queries (acceptable)"
                fi
                
                if [[ $queries_with_dsn -gt 0 ]]; then
                    local db_dsn_value
                    db_dsn_value=$(echo "$sample_sql" | jq -r '[.[] | select(has("db_dsn"))][0].db_dsn' 2>/dev/null || echo "")
                    # Check if DSN contains password (should be masked)
                    if [[ "$db_dsn_value" =~ password ]]; then
                        if [[ "$db_dsn_value" =~ password=\*\*\* ]] || [[ "$db_dsn_value" =~ password= ]]; then
                            test_result 0 "SQL db_dsn field" "Found $queries_with_dsn query(ies) with 'db_dsn' field (password should be masked)"
                        else
                            test_result 1 "SQL db_dsn password masking" "DSN may contain unmasked password"
                        fi
                    else
                        test_result 0 "SQL db_dsn field" "Found $queries_with_dsn query(ies) with 'db_dsn' field"
                    fi
                else
                    test_result 0 "SQL db_dsn field" "db_dsn may not be available for all queries (acceptable)"
                fi
                
                # Verify query types are present
                local select_count insert_count update_count delete_count
                select_count=$(echo "$sample_sql" | jq '[.[] | select(.query_type == "SELECT" or (.query | ascii_upcase | startswith("SELECT")))] | length' 2>/dev/null || echo "0")
                insert_count=$(echo "$sample_sql" | jq '[.[] | select(.query_type == "INSERT" or (.query | ascii_upcase | startswith("INSERT")))] | length' 2>/dev/null || echo "0")
                update_count=$(echo "$sample_sql" | jq '[.[] | select(.query_type == "UPDATE" or (.query | ascii_upcase | startswith("UPDATE")))] | length' 2>/dev/null || echo "0")
                delete_count=$(echo "$sample_sql" | jq '[.[] | select(.query_type == "DELETE" or (.query | ascii_upcase | startswith("DELETE")))] | length' 2>/dev/null || echo "0")
                
                if [[ $select_count -gt 0 ]]; then
                    test_result 0 "SELECT queries" "Found $select_count SELECT query(ies)"
                fi
                if [[ $insert_count -gt 0 ]]; then
                    test_result 0 "INSERT queries" "Found $insert_count INSERT query(ies)"
                fi
                if [[ $update_count -gt 0 ]]; then
                    test_result 0 "UPDATE queries" "Found $update_count UPDATE query(ies)"
                fi
                if [[ $delete_count -gt 0 ]]; then
                    test_result 0 "DELETE queries" "Found $delete_count DELETE query(ies)"
                fi
                
            else
                test_result 1 "SQL query array structure" "SQL array is empty or invalid"
            fi
        else
            test_result 1 "SQL query data" "No SQL query data found or invalid JSON"
        fi
    else
        test_result 1 "SQL queries in spans_full" "No SQL queries found in ClickHouse"
        return 1
    fi
    
    # Also check spans_min for db_system and query_fingerprint
    local spans_min_count
    spans_min_count=$(query_clickhouse "SELECT count() FROM opa.spans_min WHERE trace_id = '$trace_id' AND db_system IS NOT NULL" 2>&1)
    
    if [[ "$spans_min_count" -gt 0 ]]; then
        test_result 0 "db_system in spans_min" "Found $spans_min_count span(s) with db_system in spans_min"
        
        # Get sample db_system value
        local db_system_value
        db_system_value=$(query_clickhouse "SELECT db_system FROM opa.spans_min WHERE trace_id = '$trace_id' AND db_system IS NOT NULL LIMIT 1" 2>&1 | tr -d '\n\r ')
        
        if [[ -n "$db_system_value" ]] && [[ "$db_system_value" != "null" ]]; then
            test_result 0 "db_system value" "db_system: $db_system_value"
        fi
    else
        test_result 1 "db_system in spans_min" "No spans found with db_system in spans_min"
    fi
    
    return 0
}

# Main test execution
main() {
    echo "=== PDO SQL Profiling E2E Test ==="
    echo ""
    
    # Check services
    if ! check_services; then
        log_error "Service check failed"
        exit 1
    fi
    
    # Start MySQL
    if ! start_mysql; then
        log_error "Failed to start MySQL"
        exit 1
    fi
    
    echo ""
    echo "Running tests..."
    echo ""
    
    # Add a delay to ensure MySQL is fully ready for connections
    sleep 3
    
    # Run PHP test
    if ! run_php_test; then
        log_error "PHP test failed"
        if [[ "$VERBOSE" -eq 1 ]] || [[ "$CI_MODE" -eq 1 ]]; then
            log_info "Checking MySQL container status..."
            ${DOCKER_COMPOSE} -f "${DOCKER_COMPOSE_FILE}" ps mysql-test 2>&1 || true
            log_info "Checking MySQL logs (last 20 lines)..."
            ${DOCKER_COMPOSE} -f "${DOCKER_COMPOSE_FILE}" logs --tail 20 mysql-test 2>&1 || true
        fi
        exit 1
    fi
    
    echo ""
    
    # Check if agent has received any traces before waiting
    if [[ "$VERBOSE" -eq 1 ]] || [[ "$CI_MODE" -eq 1 ]]; then
        log_info "Checking agent for traces..."
        local agent_traces
        agent_traces=$(curl -sf "${API_URL}/api/traces?limit=5" 2>/dev/null | jq -r '.traces | length' 2>/dev/null || echo "0")
        if [[ "$agent_traces" =~ ^[0-9]+$ ]] && [[ $agent_traces -gt 0 ]]; then
            log_info "Agent has $agent_traces trace(s) available"
        else
            log_warn "Agent API check failed or no traces found yet"
        fi
    fi
    
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
    
    # Verify SQL profiling data in ClickHouse
    verify_sql_in_clickhouse "$trace_id"
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
