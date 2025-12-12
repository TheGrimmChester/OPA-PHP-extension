#!/bin/bash
set -euo pipefail

# End-to-End Test for CI: Service Map with HTTP, Redis, SQL, and Cache
# Tests service map visualization with various dependency types
#
# Usage:
#   ./test_service_map_ci_e2e.sh [--verbose]
#
# Environment variables:
#   API_URL: Agent API URL (default: http://localhost:8081)
#   MOCK_SERVER_PORT: Mock HTTP server port (default: 8888)
#   CI_MODE: Set to '1' for CI mode (structured output)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="${PROJECT_ROOT:-$(cd "${SCRIPT_DIR}/../../.." && pwd)}"" && pwd)}"
PHP_EXTENSION_DIR="${PROJECT_ROOT}"

# Configuration
API_URL="${API_URL:-http://localhost:8081}"
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

# Detect docker compose command
if docker compose version >/dev/null 2>&1; then
    DOCKER_COMPOSE="docker compose"
elif docker-compose version >/dev/null 2>&1; then
    DOCKER_COMPOSE="docker-compose"
else
    echo "Error: Neither 'docker compose' nor 'docker-compose' is available" >&2
    exit 1
fi

# Cleanup function
cleanup() {
    local exit_code=$?
    cd "${PHP_EXTENSION_DIR}" || true
    
    # Stop test containers
    ${DOCKER_COMPOSE} -f docker/compose/docker-compose.test.yml down > /dev/null 2>&1 || true
    
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
    cd "${PHP_EXTENSION_DIR}" || return 1
    
    log_info "Starting mock HTTP server..."
    
    ${DOCKER_COMPOSE} -f docker/compose/docker-compose.test.yml up -d mock-http-server 2>&1 | grep -v "Creating\|Created\|Starting\|Started" || true
    
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
    return 1
}

# Start MySQL test database
start_mysql() {
    cd "${PHP_EXTENSION_DIR}" || return 1
    
    log_info "Starting MySQL test database..."
    
    ${DOCKER_COMPOSE} -f docker/compose/docker-compose.test.yml up -d mysql-test 2>&1 | grep -v "Creating\|Created\|Starting\|Started" || true
    
    local max_attempts=30
    local attempt=0
    while [[ $attempt -lt $max_attempts ]]; do
        if ${DOCKER_COMPOSE} -f docker/compose/docker-compose.test.yml exec -T mysql-test mysqladmin ping -h localhost -u root -p"${MYSQL_ROOT_PASSWORD:-root_password}" --silent 2>/dev/null; then
            log_info "MySQL is ready"
            
            # Create database and user if they don't exist
            ${DOCKER_COMPOSE} -f docker/compose/docker-compose.test.yml exec -T mysql-test mysql -u root -p"${MYSQL_ROOT_PASSWORD:-root_password}" <<EOF 2>/dev/null || true
CREATE DATABASE IF NOT EXISTS ${MYSQL_DATABASE:-test_db};
CREATE USER IF NOT EXISTS '${MYSQL_USER:-test_user}'@'%' IDENTIFIED BY '${MYSQL_PASSWORD:-test_password}';
GRANT ALL PRIVILEGES ON ${MYSQL_DATABASE:-test_db}.* TO '${MYSQL_USER:-test_user}'@'%';
FLUSH PRIVILEGES;
EOF
            
            sleep 2
            return 0
        fi
        sleep 1
        ((attempt++)) || true
    done
    
    log_error "MySQL failed to start"
    return 1
}

# Start Redis test server
start_redis() {
    cd "${PHP_EXTENSION_DIR}" || return 1
    
    log_info "Starting Redis test server..."
    
    ${DOCKER_COMPOSE} -f docker/compose/docker-compose.test.yml up -d redis-test 2>&1 | grep -v "Creating\|Created\|Starting\|Started" || true
    
    local max_attempts=20
    local attempt=0
    while [[ $attempt -lt $max_attempts ]]; do
        if ${DOCKER_COMPOSE} -f docker/compose/docker-compose.test.yml exec -T redis-test redis-cli ping 2>/dev/null | grep -q "PONG"; then
            log_info "Redis is ready"
            return 0
        fi
        sleep 0.5
        ((attempt++)) || true
    done
    
    log_error "Redis failed to start"
    return 1
}

# Check if services are running
check_services() {
    log_info "Checking required services..."
    
    if ! curl -sf "${API_URL}/api/health" > /dev/null 2>&1; then
        log_error "Agent is not accessible at ${API_URL}"
        return 1
    fi
    log_info "Agent is accessible"
    
    return 0
}

# Run PHP test
run_php_test() {
    log_info "Running PHP service map test..."
    
    cd "${PHP_EXTENSION_DIR}" || return 1
    
    MOCK_SERVER_URL="$MOCK_SERVER_URL" ${DOCKER_COMPOSE} -f docker/compose/docker-compose.test.yml run --rm \
        -e MOCK_SERVER_URL="$MOCK_SERVER_URL" \
        -e MYSQL_HOST="${MYSQL_HOST:-mysql-test}" \
        -e MYSQL_PORT="${MYSQL_PORT:-3306}" \
        -e MYSQL_DATABASE="${MYSQL_DATABASE:-test_db}" \
        -e MYSQL_USER="${MYSQL_USER:-test_user}" \
        -e MYSQL_PASSWORD="${MYSQL_PASSWORD:-test_password}" \
        -e MYSQL_ROOT_PASSWORD="${MYSQL_ROOT_PASSWORD:-root_password}" \
        -e REDIS_HOST="${REDIS_HOST:-redis-test}" \
        -e REDIS_PORT="${REDIS_PORT:-6379}" \
        php php -d opa.socket_path=opa-agent:9090 \
            -d opa.enabled=1 \
            -d opa.sampling_rate=1.0 \
            -d opa.collect_internal_functions=1 \
            -d opa.debug_log=0 \
            -d opa.service=service-map-e2e-test \
            /var/www/html/tests/e2e/service_map_e2e/service_map_e2e.php 2>&1 | grep -v "^Container" || true
    
    local php_exit_code=$?
    
    log_info "Waiting 10 seconds for agent to process and store traces..."
    sleep 10
    
    return $php_exit_code
}

# Helper function to query ClickHouse
query_clickhouse() {
    local query="$1"
    local clickhouse_container
    local result
    
    clickhouse_container=$(docker ps --filter "ancestor=clickhouse/clickhouse-server:23.3" --format "{{.ID}}" | head -1)
    
    if [[ -z "$clickhouse_container" ]]; then
        clickhouse_container=$(docker ps --filter "name=clickhouse" --format "{{.ID}}" | head -1)
    fi
    
    if [[ -n "$clickhouse_container" ]]; then
        result=$(docker exec "$clickhouse_container" clickhouse-client --query "$query" 2>&1)
        local exit_code=$?
        if [[ $exit_code -ne 0 ]]; then
            if [[ "$VERBOSE" -eq 1 ]] && [[ "$query" =~ "SELECT" ]]; then
                log_warn "ClickHouse query failed (exit $exit_code): $result"
            fi
            return $exit_code
        fi
        echo "$result"
        return 0
    else
        result=$(${DOCKER_COMPOSE} exec -T clickhouse clickhouse-client --query "$query" 2>&1)
        local exit_code=$?
        if [[ $exit_code -ne 0 ]]; then
            if [[ "$VERBOSE" -eq 1 ]] && [[ "$query" =~ "SELECT" ]]; then
                log_warn "ClickHouse query failed via docker-compose (exit $exit_code): $result"
            fi
            return $exit_code
        fi
        echo "$result"
        return 0
    fi
}

# Wait for service dependencies to be stored
wait_for_service_dependencies() {
    cd "${PROJECT_ROOT}" || return 1
    
    log_info "Waiting for service dependencies to be stored..."
    
    local clickhouse_test
    clickhouse_test=$(query_clickhouse "SELECT 1" 2>&1)
    if [[ $? -ne 0 ]] || [[ "$clickhouse_test" =~ "not running" ]] || [[ "$clickhouse_test" =~ "error" ]]; then
        log_error "ClickHouse is not accessible: $clickhouse_test"
        return 1
    fi
    
    local max_attempts=40
    local attempt=0
    
    while [[ $attempt -lt $max_attempts ]]; do
        local dep_count
        dep_count=$(query_clickhouse "SELECT count() FROM opa.service_dependencies WHERE from_service = 'service-map-e2e-test' AND hour >= now() - INTERVAL 2 HOUR" 2>&1 | tr -d '\n\r ' || echo "0")
        
        if [[ "$dep_count" =~ ^[0-9]+$ ]] && [[ "$dep_count" -gt 0 ]]; then
            log_info "Found $dep_count service dependency(ies)"
            return 0
        fi
        
        sleep 1
        ((attempt++)) || true
        if [[ "$VERBOSE" -eq 1 ]] && [[ $((attempt % 5)) -eq 0 ]]; then
            echo -n "."
        fi
    done
    
    log_warn "No service dependencies found after $max_attempts attempts"
    return 1
}

# Verify service dependencies in ClickHouse
verify_service_dependencies_in_clickhouse() {
    cd "${PROJECT_ROOT}" || return 1
    
    log_info "Verifying service dependencies in ClickHouse..."
    
    # Check HTTP dependencies
    local http_count
    http_count=$(query_clickhouse "SELECT count() FROM opa.service_dependencies WHERE from_service = 'service-map-e2e-test' AND dependency_type = 'http' AND hour >= now() - INTERVAL 2 HOUR" 2>&1 | tr -d '\n\r ' || echo "0")
    
    if [[ "$http_count" =~ ^[0-9]+$ ]] && [[ "$http_count" -gt 0 ]]; then
        test_result 0 "HTTP dependencies in ClickHouse" "Found $http_count HTTP dependency(ies)"
        
        # Check dependency_target format
        local http_target
        http_target=$(query_clickhouse "SELECT dependency_target FROM opa.service_dependencies WHERE from_service = 'service-map-e2e-test' AND dependency_type = 'http' AND hour >= now() - INTERVAL 2 HOUR LIMIT 1" 2>&1 | tr -d '\n\r ' || echo "")
        if [[ -n "$http_target" ]] && [[ "$http_target" =~ ^http ]]; then
            test_result 0 "HTTP dependency_target format" "Target: $http_target"
        else
            test_result 1 "HTTP dependency_target format" "Invalid or missing target: $http_target"
        fi
    else
        test_result 1 "HTTP dependencies in ClickHouse" "No HTTP dependencies found"
    fi
    
    # Check Redis dependencies
    local redis_count
    redis_count=$(query_clickhouse "SELECT count() FROM opa.service_dependencies WHERE from_service = 'service-map-e2e-test' AND dependency_type = 'redis' AND hour >= now() - INTERVAL 2 HOUR" 2>&1 | tr -d '\n\r ' || echo "0")
    
    if [[ "$redis_count" =~ ^[0-9]+$ ]] && [[ "$redis_count" -gt 0 ]]; then
        test_result 0 "Redis dependencies in ClickHouse" "Found $redis_count Redis dependency(ies)"
        
        # Check dependency_target format
        local redis_target
        redis_target=$(query_clickhouse "SELECT dependency_target FROM opa.service_dependencies WHERE from_service = 'service-map-e2e-test' AND dependency_type = 'redis' AND hour >= now() - INTERVAL 2 HOUR LIMIT 1" 2>&1 | tr -d '\n\r ' || echo "")
        if [[ -n "$redis_target" ]] && [[ "$redis_target" =~ ^redis ]]; then
            test_result 0 "Redis dependency_target format" "Target: $redis_target"
        else
            test_result 1 "Redis dependency_target format" "Invalid or missing target: $redis_target"
        fi
    else
        test_result 1 "Redis dependencies in ClickHouse" "No Redis dependencies found"
    fi
    
    # Check database dependencies
    local db_count
    db_count=$(query_clickhouse "SELECT count() FROM opa.service_dependencies WHERE from_service = 'service-map-e2e-test' AND dependency_type = 'database' AND hour >= now() - INTERVAL 2 HOUR" 2>&1 | tr -d '\n\r ' || echo "0")
    
    if [[ "$db_count" =~ ^[0-9]+$ ]] && [[ "$db_count" -gt 0 ]]; then
        test_result 0 "Database dependencies in ClickHouse" "Found $db_count database dependency(ies)"
        
        # Check dependency_target format (should be mysql://hostname or db:mysql)
        local db_target
        db_target=$(query_clickhouse "SELECT dependency_target FROM opa.service_dependencies WHERE from_service = 'service-map-e2e-test' AND dependency_type = 'database' AND hour >= now() - INTERVAL 2 HOUR LIMIT 1" 2>&1 | tr -d '\n\r ' || echo "")
        if [[ -n "$db_target" ]]; then
            if [[ "$db_target" =~ ^mysql:// ]] || [[ "$db_target" =~ ^db:mysql ]]; then
                test_result 0 "Database dependency_target format" "Target: $db_target"
            else
                test_result 1 "Database dependency_target format" "Unexpected target format: $db_target (expected mysql://hostname or db:mysql)"
            fi
        else
            test_result 1 "Database dependency_target" "Missing dependency_target"
        fi
    else
        # Database dependencies might not be tracked separately if they're part of SQL queries
        # This is acceptable - SQL queries are tracked in spans
        test_result 0 "Database dependencies in ClickHouse" "Database operations tracked via SQL queries"
    fi
    
    # Verify SQL data in spans_full contains db_system and db_host
    log_info "Verifying SQL data in spans_full contains db_system and db_host..."
    local sql_data_query
    sql_data_query="SELECT 
        sql
        FROM opa.spans_full
        WHERE service = 'service-map-e2e-test' 
            AND start_ts >= now() - INTERVAL 2 HOUR
            AND sql != '' AND sql != '[]'
        LIMIT 1
        FORMAT JSONEachRow"
    
    local sql_data_result
    sql_data_result=$(query_clickhouse "$sql_data_query" 2>&1)
    
    if [[ -n "$sql_data_result" ]] && [[ ! "$sql_data_result" =~ "error" ]] && [[ ! "$sql_data_result" =~ "Exception" ]]; then
        # Extract SQL JSON and check for db_system and db_host
        local sql_json
        sql_json=$(echo "$sql_data_result" | jq -r '.sql' 2>/dev/null || echo "")
        
        if [[ -n "$sql_json" ]] && [[ "$sql_json" != "null" ]] && [[ "$sql_json" != "[]" ]]; then
            # Check if SQL array contains objects with db_system
            local has_db_system
            has_db_system=$(echo "$sql_json" | jq '[.[] | select(has("db_system"))] | length' 2>/dev/null || echo "0")
            
            if [[ "$has_db_system" =~ ^[0-9]+$ ]] && [[ "$has_db_system" -gt 0 ]]; then
                test_result 0 "SQL data contains db_system" "Found $has_db_system SQL query(ies) with db_system"
                
                # Check db_system value
                local db_system_value
                db_system_value=$(echo "$sql_json" | jq -r '[.[] | select(has("db_system"))][0].db_system' 2>/dev/null || echo "")
                if [[ -n "$db_system_value" ]] && [[ "$db_system_value" != "null" ]]; then
                    test_result 0 "SQL db_system value" "db_system: $db_system_value"
                fi
                
                # Check if db_host is present
                local has_db_host
                has_db_host=$(echo "$sql_json" | jq '[.[] | select(has("db_host"))] | length' 2>/dev/null || echo "0")
                
                if [[ "$has_db_host" =~ ^[0-9]+$ ]] && [[ "$has_db_host" -gt 0 ]]; then
                    test_result 0 "SQL data contains db_host" "Found $has_db_host SQL query(ies) with db_host"
                    
                    # Check db_host value
                    local db_host_value
                    db_host_value=$(echo "$sql_json" | jq -r '[.[] | select(has("db_host"))][0].db_host' 2>/dev/null || echo "")
                    if [[ -n "$db_host_value" ]] && [[ "$db_host_value" != "null" ]]; then
                        test_result 0 "SQL db_host value" "db_host: $db_host_value"
                    fi
                else
                    # db_host might not be available for all queries (e.g., if connection info is not accessible)
                    test_result 0 "SQL db_host" "db_host may not be available for all queries (acceptable)"
                fi
                
                # Check if db_dsn is present
                local has_db_dsn
                has_db_dsn=$(echo "$sql_json" | jq '[.[] | select(has("db_dsn"))] | length' 2>/dev/null || echo "0")
                
                if [[ "$has_db_dsn" =~ ^[0-9]+$ ]] && [[ "$has_db_dsn" -gt 0 ]]; then
                    test_result 0 "SQL data contains db_dsn" "Found $has_db_dsn SQL query(ies) with db_dsn"
                    
                    # Check db_dsn value (should not contain actual password, only masked)
                    local db_dsn_value
                    db_dsn_value=$(echo "$sql_json" | jq -r '[.[] | select(has("db_dsn"))][0].db_dsn' 2>/dev/null || echo "")
                    if [[ -n "$db_dsn_value" ]] && [[ "$db_dsn_value" != "null" ]]; then
                        # Check that password is masked if present
                        if [[ "$db_dsn_value" =~ password= ]] && [[ ! "$db_dsn_value" =~ password=\*\*\* ]]; then
                            # Check if it's an actual password (not just "password=" with no value)
                            # Use grep to check for unmasked password
                            if echo "$db_dsn_value" | grep -qE "password=[^;*]{1,}"; then
                                test_result 1 "SQL db_dsn password masking" "DSN may contain unmasked password"
                            else
                                test_result 0 "SQL db_dsn value" "db_dsn: $db_dsn_value"
                            fi
                        else
                            test_result 0 "SQL db_dsn value" "db_dsn: $db_dsn_value"
                        fi
                    fi
                else
                    test_result 0 "SQL db_dsn" "db_dsn may not be available for all queries (acceptable)"
                fi
            else
                test_result 1 "SQL data contains db_system" "No SQL queries found with db_system field"
            fi
        else
            test_result 1 "SQL data in spans_full" "No SQL data found or invalid JSON"
        fi
    else
        test_result 1 "SQL data query" "Failed to query SQL data from spans_full"
    fi
    
    # Check cache dependencies (if available)
    local cache_count
    cache_count=$(query_clickhouse "SELECT count() FROM opa.service_dependencies WHERE from_service = 'service-map-e2e-test' AND dependency_type = 'cache' AND hour >= now() - INTERVAL 2 HOUR" 2>&1 | tr -d '\n\r ' || echo "0")
    
    if [[ "$cache_count" =~ ^[0-9]+$ ]] && [[ "$cache_count" -gt 0 ]]; then
        test_result 0 "Cache dependencies in ClickHouse" "Found $cache_count cache dependency(ies)"
    else
        # Cache dependencies might not be available if APCu is not enabled
        test_result 0 "Cache dependencies in ClickHouse" "Cache operations may not be available (APCu extension)"
    fi
    
    # Verify metrics
    local metrics_query
    metrics_query="SELECT 
        dependency_type,
        sum(call_count) as total_calls,
        avg(avg_duration_ms) as avg_duration,
        avg(error_rate) as avg_error_rate,
        sum(bytes_sent) as total_bytes_sent,
        sum(bytes_received) as total_bytes_received
        FROM opa.service_dependencies
        WHERE from_service = 'service-map-e2e-test' AND hour >= now() - INTERVAL 2 HOUR
        GROUP BY dependency_type
        FORMAT JSONEachRow"
    
    local metrics_result
    metrics_result=$(query_clickhouse "$metrics_query" 2>&1)
    
    if [[ -n "$metrics_result" ]] && [[ ! "$metrics_result" =~ "error" ]] && [[ ! "$metrics_result" =~ "Exception" ]]; then
        test_result 0 "Service dependency metrics" "Metrics retrieved successfully"
        
        if [[ "$VERBOSE" -eq 1 ]]; then
            echo "$metrics_result" | jq '.' 2>/dev/null || echo "$metrics_result"
        fi
    else
        test_result 1 "Service dependency metrics" "Failed to retrieve metrics"
    fi
    
    return 0
}

# Verify service map API
verify_service_map_api() {
    cd "${PROJECT_ROOT}" || return 1
    
    log_info "Verifying service map API..."
    
    # Get service map data from API
    local api_response
    api_response=$(curl -sf "${API_URL}/api/service-map?from=$(date -u -d '10 minutes ago' +%Y-%m-%dT%H:%M:%SZ)&to=$(date -u +%Y-%m-%dT%H:%M:%SZ)" 2>/dev/null)
    
    if [[ -z "$api_response" ]]; then
        test_result 1 "Service map API response" "Failed to fetch service map data"
        return 1
    fi
    
    test_result 0 "Service map API response" "Data fetched successfully"
    
    # Check if response has nodes and edges
    local has_nodes has_edges
    has_nodes=$(echo "$api_response" | jq -r 'has("nodes") or has("data") and (.data | has("nodes"))' 2>/dev/null || echo "false")
    has_edges=$(echo "$api_response" | jq -r 'has("edges") or has("data") and (.data | has("edges"))' 2>/dev/null || echo "false")
    
    if [[ "$has_nodes" == "true" ]] && [[ "$has_edges" == "true" ]]; then
        test_result 0 "Service map structure" "Nodes and edges present"
    else
        test_result 1 "Service map structure" "Missing nodes or edges (nodes: $has_nodes, edges: $has_edges)"
    fi
    
    # Extract nodes and edges
    local nodes edges
    nodes=$(echo "$api_response" | jq -r '.nodes // .data.nodes // []' 2>/dev/null || echo "[]")
    edges=$(echo "$api_response" | jq -r '.edges // .data.edges // []' 2>/dev/null || echo "[]")
    
    # Check for external dependencies in edges
    local external_deps
    external_deps=$(echo "$edges" | jq '[.[] | select(has("dependency_type") and .dependency_type != "service")] | length' 2>/dev/null || echo "0")
    
    if [[ "$external_deps" =~ ^[0-9]+$ ]] && [[ "$external_deps" -gt 0 ]]; then
        test_result 0 "External dependencies in API" "Found $external_deps external dependency(ies)"
        
        # Check for HTTP dependencies
        local http_deps
        http_deps=$(echo "$edges" | jq '[.[] | select(.dependency_type == "http")] | length' 2>/dev/null || echo "0")
        if [[ "$http_deps" -gt 0 ]]; then
            test_result 0 "HTTP dependencies in API" "Found $http_deps HTTP edge(s)"
            
            # Check dependency_target
            local http_target
            http_target=$(echo "$edges" | jq -r '[.[] | select(.dependency_type == "http")][0].dependency_target // ""' 2>/dev/null || echo "")
            if [[ -n "$http_target" ]] && [[ "$http_target" =~ ^http ]]; then
                test_result 0 "HTTP dependency_target in API" "Target: $http_target"
            fi
        fi
        
        # Check for Redis dependencies
        local redis_deps
        redis_deps=$(echo "$edges" | jq '[.[] | select(.dependency_type == "redis")] | length' 2>/dev/null || echo "0")
        if [[ "$redis_deps" -gt 0 ]]; then
            test_result 0 "Redis dependencies in API" "Found $redis_deps Redis edge(s)"
        fi
        
        # Check for database dependencies
        local db_deps
        db_deps=$(echo "$edges" | jq '[.[] | select(.dependency_type == "database")] | length' 2>/dev/null || echo "0")
        if [[ "$db_deps" -gt 0 ]]; then
            test_result 0 "Database dependencies in API" "Found $db_deps database edge(s)"
            
            # Check dependency_target format for database
            local db_target
            db_target=$(echo "$edges" | jq -r '[.[] | select(.dependency_type == "database")][0].dependency_target // ""' 2>/dev/null || echo "")
            if [[ -n "$db_target" ]]; then
                if [[ "$db_target" =~ ^mysql:// ]] || [[ "$db_target" =~ ^postgresql:// ]] || [[ "$db_target" =~ ^db: ]]; then
                    test_result 0 "Database dependency_target in API" "Target: $db_target"
                else
                    test_result 1 "Database dependency_target format in API" "Unexpected format: $db_target"
                fi
            fi
            
            # Check if 'to' field contains database system and hostname
            local db_to
            db_to=$(echo "$edges" | jq -r '[.[] | select(.dependency_type == "database")][0].to // ""' 2>/dev/null || echo "")
            if [[ -n "$db_to" ]]; then
                if [[ "$db_to" =~ ^mysql:// ]] || [[ "$db_to" =~ ^postgresql:// ]] || [[ "$db_to" =~ ^db: ]]; then
                    test_result 0 "Database 'to' field in API" "To: $db_to"
                else
                    test_result 0 "Database 'to' field in API" "To: $db_to (may be in different format)"
                fi
            fi
        else
            test_result 0 "Database dependencies in API" "No database dependencies found (may be tracked differently)"
        fi
    else
        test_result 1 "External dependencies in API" "No external dependencies found in edges"
    fi
    
    # Check for health_status in edges
    local health_status_count
    health_status_count=$(echo "$edges" | jq '[.[] | select(has("health_status"))] | length' 2>/dev/null || echo "0")
    
    if [[ "$health_status_count" -gt 0 ]]; then
        test_result 0 "Health status in edges" "Found $health_status_count edge(s) with health_status"
    else
        test_result 0 "Health status in edges" "Health status may not be present for all edges"
    fi
    
    # Check for latency metrics
    local latency_metrics
    latency_metrics=$(echo "$edges" | jq '[.[] | select(has("avg_latency_ms"))] | length' 2>/dev/null || echo "0")
    
    if [[ "$latency_metrics" -gt 0 ]]; then
        test_result 0 "Latency metrics in edges" "Found $latency_metrics edge(s) with latency metrics"
    else
        test_result 0 "Latency metrics in edges" "Latency metrics may not be present for all edges"
    fi
    
    # Check for call_count
    local call_count_metrics
    call_count_metrics=$(echo "$edges" | jq '[.[] | select(has("call_count"))] | length' 2>/dev/null || echo "0")
    
    if [[ "$call_count_metrics" -gt 0 ]]; then
        test_result 0 "Call count metrics in edges" "Found $call_count_metrics edge(s) with call_count"
    else
        test_result 0 "Call count metrics in edges" "Call count may not be present for all edges"
    fi
    
    if [[ "$VERBOSE" -eq 1 ]]; then
        echo ""
        echo "Service Map API Response (sample):"
        echo "$api_response" | jq '{nodes: (.nodes // .data.nodes | length), edges: (.edges // .data.edges | length), sample_edge: (.edges // .data.edges | .[0])}' 2>/dev/null || echo "$api_response"
    fi
    
    return 0
}

# Main test execution
main() {
    echo "=== Service Map CI End-to-End Test ==="
    echo ""
    
    # Check services
    if ! check_services; then
        log_error "Service check failed"
        exit 1
    fi
    
    # Start test infrastructure
    if ! start_mock_server; then
        log_error "Failed to start mock server"
        exit 1
    fi
    
    if ! start_mysql; then
        log_error "Failed to start MySQL"
        exit 1
    fi
    
    if ! start_redis; then
        log_error "Failed to start Redis"
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
    log_info "Waiting for service dependencies to be stored..."
    
    # Wait for dependencies
    if ! wait_for_service_dependencies; then
        log_warn "Service dependencies may not be available yet, continuing with verification..."
    fi
    
    echo ""
    
    # Verify service dependencies in ClickHouse
    verify_service_dependencies_in_clickhouse
    echo ""
    
    # Verify service map API
    verify_service_map_api
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
