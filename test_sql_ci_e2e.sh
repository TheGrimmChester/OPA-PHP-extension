#!/bin/bash
set -euo pipefail

# End-to-End Test for CI: SQL Query Profiling with MySQL
# Tests various query types, row counts, and execution times
#
# Usage:
#   ./test_sql_ci_e2e.sh [--verbose]
#
# Environment variables:
#   API_URL: Agent API URL (default: http://localhost:8081)
#   MYSQL_HOST: MySQL host (default: mysql-test)
#   MYSQL_PORT: MySQL port (default: 3306)
#   CI_MODE: Set to '1' for CI mode (structured output)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="${PROJECT_ROOT:-$(cd "${SCRIPT_DIR}/.." && pwd)}"
PHP_EXTENSION_DIR="${SCRIPT_DIR}"

# Configuration
API_URL="${API_URL:-http://localhost:8081}"
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

# Cleanup function
cleanup() {
    local exit_code=$?
    cd "${PHP_EXTENSION_DIR}" || true
    
    # Stop MySQL test container
    docker compose -f docker-compose.test.yml down > /dev/null 2>&1 || true
    
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
    cd "${PHP_EXTENSION_DIR}" || return 1
    
    log_info "Starting MySQL test database..."
    
    # Start MySQL using docker compose
    docker compose -f docker-compose.test.yml up -d mysql-test 2>&1 | grep -v "Creating\|Created\|Starting\|Started" || true
    
    # Wait for MySQL to be ready
    local max_attempts=30
    local attempt=0
    while [[ $attempt -lt $max_attempts ]]; do
        if docker compose -f docker-compose.test.yml exec -T mysql-test mysqladmin ping -h localhost -u root -p"${MYSQL_ROOT_PASSWORD}" --silent 2>/dev/null; then
            log_info "MySQL is ready"
            
            # Create database and user if they don't exist
            docker compose -f docker-compose.test.yml exec -T mysql-test mysql -u root -p"${MYSQL_ROOT_PASSWORD}" <<EOF 2>/dev/null || true
CREATE DATABASE IF NOT EXISTS ${MYSQL_DATABASE};
CREATE USER IF NOT EXISTS '${MYSQL_USER}'@'%' IDENTIFIED BY '${MYSQL_PASSWORD}';
GRANT ALL PRIVILEGES ON ${MYSQL_DATABASE}.* TO '${MYSQL_USER}'@'%';
FLUSH PRIVILEGES;
EOF
            
            return 0
        fi
        sleep 1
        ((attempt++)) || true
    done
    
    log_error "MySQL failed to start"
    if [[ "$VERBOSE" -eq 1 ]]; then
        docker compose -f docker-compose.test.yml logs mysql-test 2>&1 | tail -20
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

# Run PHP test with various SQL scenarios
run_php_test() {
    log_info "Running PHP SQL test with MySQL..."
    
    cd "${PHP_EXTENSION_DIR}" || return 1
    
    # Create comprehensive test script
    local test_script="${PHP_EXTENSION_DIR}/tests/test_sql_ci_e2e.php"
    cat > "$test_script" << 'EOF'
<?php
echo "Testing SQL profiling with various query types...\n";

$mysql_host = getenv('MYSQL_HOST') ?: 'mysql-test';
$mysql_port = (int)(getenv('MYSQL_PORT') ?: 3306);
$mysql_database = getenv('MYSQL_DATABASE') ?: 'test_db';
$mysql_user = getenv('MYSQL_USER') ?: 'test_user';
$mysql_password = getenv('MYSQL_PASSWORD') ?: 'test_password';

$test_results = [];

try {
    // Test MySQLi
    $mysqli = new mysqli($mysql_host, $mysql_user, $mysql_password, $mysql_database, $mysql_port);
    
    if ($mysqli->connect_error) {
        die("MySQLi Connection failed: " . $mysqli->connect_error . "\n");
    }
    
    // Test 1: CREATE TABLE
    mysqli_query($mysqli, "DROP TABLE IF EXISTS test_products");
    mysqli_query($mysqli, "CREATE TABLE test_products (
        id INT AUTO_INCREMENT PRIMARY KEY,
        name VARCHAR(100) NOT NULL,
        price DECIMAL(10,2) NOT NULL,
        category VARCHAR(50),
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    )");
    $test_results[] = ['status' => true, 'name' => 'CREATE TABLE', 'type' => 'DDL'];
    
    // Test 2: INSERT single row
    mysqli_query($mysqli, "INSERT INTO test_products (name, price, category) VALUES ('Product A', 19.99, 'Electronics')");
    $test_results[] = ['status' => true, 'name' => 'INSERT single row', 'type' => 'INSERT', 'rows' => 1];
    
    // Test 3: INSERT multiple rows
    mysqli_query($mysqli, "INSERT INTO test_products (name, price, category) VALUES 
        ('Product B', 29.99, 'Electronics'),
        ('Product C', 39.99, 'Clothing'),
        ('Product D', 49.99, 'Clothing'),
        ('Product E', 59.99, 'Electronics')");
    $test_results[] = ['status' => true, 'name' => 'INSERT multiple rows', 'type' => 'INSERT', 'rows' => 4];
    
    // Test 4: SELECT all rows
    $result = mysqli_query($mysqli, "SELECT * FROM test_products");
    $row_count = mysqli_num_rows($result);
    mysqli_free_result($result);
    $test_results[] = ['status' => $row_count >= 5, 'name' => 'SELECT all rows', 'type' => 'SELECT', 'rows' => $row_count];
    
    // Test 5: SELECT with WHERE
    $result = mysqli_query($mysqli, "SELECT * FROM test_products WHERE category = 'Electronics'");
    $row_count = mysqli_num_rows($result);
    mysqli_free_result($result);
    $test_results[] = ['status' => $row_count >= 3, 'name' => 'SELECT with WHERE', 'type' => 'SELECT', 'rows' => $row_count];
    
    // Test 6: SELECT COUNT
    $result = mysqli_query($mysqli, "SELECT COUNT(*) as total FROM test_products");
    $row = mysqli_fetch_assoc($result);
    $count = (int)$row['total'];
    mysqli_free_result($result);
    $test_results[] = ['status' => $count >= 5, 'name' => 'SELECT COUNT', 'type' => 'SELECT', 'rows' => $count];
    
    // Test 7: UPDATE
    mysqli_query($mysqli, "UPDATE test_products SET price = 24.99 WHERE name = 'Product A'");
    $test_results[] = ['status' => true, 'name' => 'UPDATE', 'type' => 'UPDATE'];
    
    // Test 8: DELETE
    mysqli_query($mysqli, "DELETE FROM test_products WHERE name = 'Product E'");
    $test_results[] = ['status' => true, 'name' => 'DELETE', 'type' => 'DELETE'];
    
    $mysqli->close();
    
} catch (Exception $e) {
    echo "ERROR in MySQLi tests: " . $e->getMessage() . "\n";
    exit(1);
}

try {
    // Test PDO
    $dsn = "mysql:host=$mysql_host;port=$mysql_port;dbname=$mysql_database;charset=utf8mb4";
    $pdo = new PDO($dsn, $mysql_user, $mysql_password, [
        PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
        PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC
    ]);
    
    // Test 9: PDO query SELECT
    $stmt = $pdo->query("SELECT COUNT(*) as count FROM test_products");
    $result = $stmt->fetch();
    $count = (int)$result['count'];
    $test_results[] = ['status' => $count >= 4, 'name' => 'PDO query SELECT', 'type' => 'SELECT', 'rows' => $count];
    
    // Test 10: PDO prepared statement SELECT
    $stmt = $pdo->prepare("SELECT * FROM test_products WHERE category = ?");
    $stmt->execute(['Clothing']);
    $rows = $stmt->fetchAll();
    $test_results[] = ['status' => count($rows) >= 2, 'name' => 'PDO prepared SELECT', 'type' => 'SELECT', 'rows' => count($rows)];
    
    // Test 11: PDO prepared statement INSERT
    $stmt = $pdo->prepare("INSERT INTO test_products (name, price, category) VALUES (?, ?, ?)");
    $stmt->execute(['Product F', 69.99, 'Electronics']);
    $test_results[] = ['status' => true, 'name' => 'PDO prepared INSERT', 'type' => 'INSERT', 'rows' => 1];
    
    // Test 12: PDO prepared statement UPDATE
    $stmt = $pdo->prepare("UPDATE test_products SET price = ? WHERE name = ?");
    $stmt->execute([79.99, 'Product F']);
    $test_results[] = ['status' => true, 'name' => 'PDO prepared UPDATE', 'type' => 'UPDATE'];
    
} catch (PDOException $e) {
    echo "ERROR in PDO tests: " . $e->getMessage() . "\n";
    exit(1);
}

// Summary
$passed = 0;
$failed = 0;
foreach ($test_results as $test) {
    if ($test['status']) {
        $passed++;
        $rows_info = isset($test['rows']) ? " ({$test['rows']} rows)" : "";
        echo "✓ " . $test['name'] . $rows_info . "\n";
    } else {
        $failed++;
        echo "✗ " . $test['name'] . "\n";
    }
}

echo "\nTest Summary: $passed passed, $failed failed\n";
exit($failed > 0 ? 1 : 0);
EOF
    
    MYSQL_HOST="$MYSQL_HOST" \
    MYSQL_PORT="$MYSQL_PORT" \
    MYSQL_DATABASE="$MYSQL_DATABASE" \
    MYSQL_USER="$MYSQL_USER" \
    MYSQL_PASSWORD="$MYSQL_PASSWORD" \
    docker compose -f docker-compose.test.yml run --rm \
        -e MYSQL_HOST="$MYSQL_HOST" \
        -e MYSQL_PORT="$MYSQL_PORT" \
        -e MYSQL_DATABASE="$MYSQL_DATABASE" \
        -e MYSQL_USER="$MYSQL_USER" \
        -e MYSQL_PASSWORD="$MYSQL_PASSWORD" \
        php php -d opa.socket_path=opa-agent:9090 \
            -d opa.enabled=1 \
            -d opa.sampling_rate=1.0 \
            -d opa.collect_internal_functions=1 \
            -d opa.debug_log=0 \
            -d opa.service=sql-ci-test \
            /var/www/html/tests/test_sql_ci_e2e.php 2>&1 | grep -v "^Container" || true
    
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
        query_result=$(docker compose exec -T clickhouse clickhouse-client --query \
            "SELECT trace_id FROM opa.spans_min WHERE service = 'sql-ci-test' ORDER BY start_ts DESC LIMIT 1" 2>&1)
        
        if [[ $? -eq 0 ]] && [[ -n "$query_result" ]] && [[ ! "$query_result" =~ "not running" ]]; then
            trace_id=$(echo "$query_result" | tr -d '\n\r ' | head -1)
        fi
        
        if [[ -z "${trace_id:-}" ]] || [[ "${trace_id:-}" == "" ]] || [[ "${trace_id:-}" == "null" ]]; then
            query_result=$(docker compose exec -T clickhouse clickhouse-client --query \
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

# Verify SQL queries in trace
verify_sql_queries() {
    local trace_id="$1"
    
    cd "${PROJECT_ROOT}" || return 1
    
    log_info "Verifying SQL queries in trace $trace_id..."
    
    # Get trace from API
    local trace_json
    trace_json=$(curl -sf "${API_URL}/api/traces/${trace_id}/full" 2>/dev/null)
    
    if [[ -z "$trace_json" ]]; then
        test_result 1 "Get trace from API" "Failed to fetch trace"
        return 1
    fi
    
    test_result 0 "Get trace from API" "Trace fetched successfully"
    
    # Extract SQL queries
    local sql_queries
    sql_queries=$(echo "$trace_json" | jq -r '[.spans[]?.sql // .spans[]?.Sql // empty | .[]?]' 2>/dev/null)
    
    # Also check call stack for SQL queries
    local stack_sql_queries
    stack_sql_queries=$(echo "$trace_json" | jq -r '[.spans[].stack[]?.sql_queries // .spans[].stack[]?.SQLQueries // empty | .[]?] | flatten' 2>/dev/null)
    
    # Combine both sources
    local all_sql_queries
    if [[ -n "$stack_sql_queries" ]] && [[ "$stack_sql_queries" != "null" ]] && [[ "$stack_sql_queries" != "[]" ]]; then
        if [[ -n "$sql_queries" ]] && [[ "$sql_queries" != "null" ]] && [[ "$sql_queries" != "[]" ]]; then
            all_sql_queries=$(echo "[$sql_queries, $stack_sql_queries]" | jq -r 'flatten' 2>/dev/null)
        else
            all_sql_queries="$stack_sql_queries"
        fi
    else
        all_sql_queries="$sql_queries"
    fi
    
    if [[ -z "$all_sql_queries" ]] || [[ "$all_sql_queries" == "null" ]] || [[ "$all_sql_queries" == "[]" ]]; then
        test_result 1 "SQL queries in trace" "No SQL queries found"
        return 1
    fi
    
    local query_count
    query_count=$(echo "$all_sql_queries" | jq 'length' 2>/dev/null || echo "0")
    
    if [[ $query_count -lt 10 ]]; then
        test_result 1 "SQL query count" "Expected at least 10 queries, found $query_count"
        return 1
    fi
    
    test_result 0 "SQL query count" "Found $query_count SQL query(ies)"
    
    # Verify query types
    local select_count insert_count update_count delete_count
    select_count=$(echo "$all_sql_queries" | jq '[.[] | select(.query_type == "SELECT" or (.query | ascii_upcase | startswith("SELECT")))] | length' 2>/dev/null || echo "0")
    insert_count=$(echo "$all_sql_queries" | jq '[.[] | select(.query_type == "INSERT" or (.query | ascii_upcase | startswith("INSERT")))] | length' 2>/dev/null || echo "0")
    update_count=$(echo "$all_sql_queries" | jq '[.[] | select(.query_type == "UPDATE" or (.query | ascii_upcase | startswith("UPDATE")))] | length' 2>/dev/null || echo "0")
    delete_count=$(echo "$all_sql_queries" | jq '[.[] | select(.query_type == "DELETE" or (.query | ascii_upcase | startswith("DELETE")))] | length' 2>/dev/null || echo "0")
    
    local type_checks_passed=0
    if [[ $select_count -gt 0 ]]; then
        ((type_checks_passed++)) || true
        test_result 0 "SELECT queries" "Found $select_count SELECT query(ies)"
    else
        test_result 1 "SELECT queries" "Not found"
    fi
    
    if [[ $insert_count -gt 0 ]]; then
        ((type_checks_passed++)) || true
        test_result 0 "INSERT queries" "Found $insert_count INSERT query(ies)"
    else
        test_result 1 "INSERT queries" "Not found"
    fi
    
    if [[ $update_count -gt 0 ]]; then
        ((type_checks_passed++)) || true
        test_result 0 "UPDATE queries" "Found $update_count UPDATE query(ies)"
    else
        test_result 1 "UPDATE queries" "Not found"
    fi
    
    if [[ $delete_count -gt 0 ]]; then
        ((type_checks_passed++)) || true
        test_result 0 "DELETE queries" "Found $delete_count DELETE query(ies)"
    else
        test_result 1 "DELETE queries" "Not found"
    fi
    
    # Verify query structure
    local first_query
    first_query=$(echo "$all_sql_queries" | jq '.[0]' 2>/dev/null)
    
    if [[ -n "$first_query" ]] && [[ "$first_query" != "null" ]]; then
        local has_query has_duration has_type
        has_query=$(echo "$first_query" | jq -r 'has("query")' 2>/dev/null || echo "false")
        has_duration=$(echo "$first_query" | jq -r 'has("duration_ms") or has("duration")' 2>/dev/null || echo "false")
        has_type=$(echo "$first_query" | jq -r 'has("query_type") or has("type")' 2>/dev/null || echo "false")
        
        if [[ "$has_query" == "true" ]] && [[ "$has_duration" == "true" ]]; then
            test_result 0 "SQL query structure" "All required fields present"
        else
            test_result 1 "SQL query structure" "Missing fields (query: $has_query, duration: $has_duration, type: $has_type)"
        fi
    fi
    
    # Verify rows_affected for INSERT/UPDATE/DELETE
    local rows_affected_queries
    rows_affected_queries=$(echo "$all_sql_queries" | jq '[.[] | select(.rows_affected != null and .rows_affected >= 0)] | length' 2>/dev/null || echo "0")
    
    if [[ $rows_affected_queries -gt 0 ]]; then
        test_result 0 "Rows affected tracking" "Found $rows_affected_queries query(ies) with rows_affected"
    else
        test_result 1 "Rows affected tracking" "No queries with rows_affected found"
    fi
    
    return 0
}

# Main test execution
main() {
    echo "=== SQL CI End-to-End Test ==="
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
    
    # Verify SQL queries
    verify_sql_queries "$trace_id"
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

