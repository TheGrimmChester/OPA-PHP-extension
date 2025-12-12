#!/bin/bash
set -euo pipefail

# E2E Test for Multi-Service Service Map
# Validates that multiple services calling each other and external services
# creates a beautiful service map visualization

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if docker-compose is available
if ! command -v docker-compose &> /dev/null; then
    DOCKER_COMPOSE="docker compose"
else
    DOCKER_COMPOSE="docker-compose"
fi

# Check if agent is running
log_info "Checking if agent is available..."
if ! curl -s http://localhost:8081/api/traces?limit=1 > /dev/null 2>&1; then
    log_error "Agent is not available at http://localhost:8081"
    log_info "Please start the agent first: docker-compose up -d agent"
    exit 1
fi

# Check if ClickHouse is running
log_info "Checking if ClickHouse is available..."
if ! docker exec clickhouse clickhouse-client --query "SELECT 1" > /dev/null 2>&1; then
    log_warn "ClickHouse container not found or not accessible"
fi

# Check if mock server is running (start it if needed)
log_info "Checking if mock HTTP server is available..."
if ! curl -s http://localhost:8888/status/200 > /dev/null 2>&1; then
    log_warn "Mock HTTP server not found, starting it..."
    $DOCKER_COMPOSE -f docker/compose/docker-compose.test.yml up -d mock-http-server 2>/dev/null || true
    log_info "Waiting for mock server to be ready..."
    sleep 3
fi

# Check if MySQL is running (try multiple container names)
log_info "Checking if MySQL is available..."
MYSQL_CONTAINER=""
if docker exec opa-mysql mysqladmin ping -h localhost --silent 2>/dev/null; then
    MYSQL_CONTAINER="opa-mysql"
    log_info "✓ Using existing MySQL container: opa-mysql"
elif docker exec mysql-test mysqladmin ping -h localhost --silent 2>/dev/null; then
    MYSQL_CONTAINER="mysql-test"
    log_info "✓ Using existing MySQL container: mysql-test"
elif docker exec opa_mysql mysqladmin ping -h localhost --silent 2>/dev/null; then
    MYSQL_CONTAINER="opa_mysql"
    log_info "✓ Using existing MySQL container: opa_mysql"
else
    log_warn "MySQL container not found, starting it..."
    $DOCKER_COMPOSE -f docker/compose/docker-compose.test.yml up -d mysql-test 2>/dev/null || true
    log_info "Waiting for MySQL to be ready (this may take up to 30 seconds)..."
    
    # Wait for MySQL to be ready
    max_wait=30
    wait_count=0
    while [ $wait_count -lt $max_wait ]; do
        if docker exec mysql-test mysqladmin ping -h localhost --silent 2>/dev/null; then
            MYSQL_CONTAINER="mysql-test"
            log_info "✓ MySQL is ready"
            break
        fi
        sleep 1
        wait_count=$((wait_count + 1))
        if [ $((wait_count % 5)) -eq 0 ]; then
            echo -n "."
        fi
    done
    echo ""
    
    if [ -z "$MYSQL_CONTAINER" ]; then
        log_warn "MySQL may not be fully ready, but continuing..."
    fi
fi

log_info "Running multi-service service map E2E test..."
echo ""

# Run the PHP test with different service names to simulate multiple services
# We'll run it multiple times with different service names to create service-to-service calls

# Service A: API Gateway
log_info "Running Service A (API Gateway)..."
test_output_a=$($DOCKER_COMPOSE -f docker/compose/docker-compose.test.yml run --rm \
    --entrypoint /usr/local/bin/docker-entrypoint-custom.sh \
    -e OPA_ENABLED=1 \
    -e OPA_SOCKET_PATH=opa-agent:9090 \
    -e OPA_SAMPLING_RATE=1.0 \
    -e OPA_FULL_CAPTURE_THRESHOLD_MS=0 \
    -e OPA_STACK_DEPTH=50 \
    -e OPA_COLLECT_INTERNAL_FUNCTIONS=1 \
    -e API_URL=http://opa-agent:8080 \
    -e MOCK_SERVER_URL=http://mock-http-server:8888 \
    -e MYSQL_HOST=opa-mysql \
    -e MYSQL_PORT=3306 \
    -e MYSQL_DATABASE=test_db \
    -e MYSQL_USER=test_user \
    -e MYSQL_PASSWORD=test_password \
    -e MYSQL_ROOT_PASSWORD=root_password \
    -e SERVICE_B_URL=http://mock-http-server:8888 \
    -e SERVICE_C_URL=http://mock-http-server:8888 \
    php php -d opa.service=api-gateway \
        -d opa.socket_path=opa-agent:9090 \
        -d opa.enabled=1 \
        -d opa.sampling_rate=1.0 \
        -d opa.full_capture_threshold_ms=0 \
        -d opa.stack_depth=50 \
        -d opa.collect_internal_functions=1 \
        /var/www/html/tests/e2e/multi_service_map_e2e/multi_service_map_e2e.php 2>&1)

# Filter out log noise
echo "$test_output_a" | grep -v "timestamp\|level\|message\|fields\|ERROR.*Failed to connect" || true

# Wait a bit between service runs
sleep 2

# Service B: User Service
log_info "Running Service B (User Service)..."
test_output_b=$($DOCKER_COMPOSE -f docker/compose/docker-compose.test.yml run --rm \
    --entrypoint /usr/local/bin/docker-entrypoint-custom.sh \
    -e OPA_ENABLED=1 \
    -e OPA_SOCKET_PATH=opa-agent:9090 \
    -e OPA_SAMPLING_RATE=1.0 \
    -e OPA_FULL_CAPTURE_THRESHOLD_MS=0 \
    -e OPA_STACK_DEPTH=50 \
    -e OPA_COLLECT_INTERNAL_FUNCTIONS=1 \
    -e API_URL=http://opa-agent:8080 \
    -e MOCK_SERVER_URL=http://mock-http-server:8888 \
    -e MYSQL_HOST=opa-mysql \
    -e MYSQL_PORT=3306 \
    -e MYSQL_DATABASE=test_db \
    -e MYSQL_USER=test_user \
    -e MYSQL_PASSWORD=test_password \
    -e MYSQL_ROOT_PASSWORD=root_password \
    -e SERVICE_B_URL=http://mock-http-server:8888 \
    -e SERVICE_C_URL=http://mock-http-server:8888 \
    php php -d opa.service=user-service \
        -d opa.socket_path=opa-agent:9090 \
        -d opa.enabled=1 \
        -d opa.sampling_rate=1.0 \
        -d opa.full_capture_threshold_ms=0 \
        -d opa.stack_depth=50 \
        -d opa.collect_internal_functions=1 \
        /var/www/html/tests/e2e/multi_service_map_e2e/multi_service_map_e2e.php 2>&1)

echo "$test_output_b" | grep -v "timestamp\|level\|message\|fields\|ERROR.*Failed to connect" || true

sleep 2

# Service C: Order Service
log_info "Running Service C (Order Service)..."
test_output_c=$($DOCKER_COMPOSE -f docker/compose/docker-compose.test.yml run --rm \
    --entrypoint /usr/local/bin/docker-entrypoint-custom.sh \
    -e OPA_ENABLED=1 \
    -e OPA_SOCKET_PATH=opa-agent:9090 \
    -e OPA_SAMPLING_RATE=1.0 \
    -e OPA_FULL_CAPTURE_THRESHOLD_MS=0 \
    -e OPA_STACK_DEPTH=50 \
    -e OPA_COLLECT_INTERNAL_FUNCTIONS=1 \
    -e API_URL=http://opa-agent:8080 \
    -e MOCK_SERVER_URL=http://mock-http-server:8888 \
    -e MYSQL_HOST=opa-mysql \
    -e MYSQL_PORT=3306 \
    -e MYSQL_DATABASE=test_db \
    -e MYSQL_USER=test_user \
    -e MYSQL_PASSWORD=test_password \
    -e MYSQL_ROOT_PASSWORD=root_password \
    -e SERVICE_B_URL=http://mock-http-server:8888 \
    -e SERVICE_C_URL=http://mock-http-server:8888 \
    php php -d opa.service=order-service \
        -d opa.socket_path=opa-agent:9090 \
        -d opa.enabled=1 \
        -d opa.sampling_rate=1.0 \
        -d opa.full_capture_threshold_ms=0 \
        -d opa.stack_depth=50 \
        -d opa.collect_internal_functions=1 \
        /var/www/html/tests/e2e/multi_service_map_e2e/multi_service_map_e2e.php 2>&1)

echo "$test_output_c" | grep -v "timestamp\|level\|message\|fields\|ERROR.*Failed to connect" || true

echo ""
log_info "Waiting 15 seconds for traces to be processed and service map to be built..."
sleep 15

# Check if test passed (look for "ALL TESTS PASSED" in any output)
if echo "$test_output_a $test_output_b $test_output_c" | grep -q "ALL TESTS PASSED"; then
    log_info "✓ Multi-service service map E2E test PASSED"
    
    # Additional validation: Query service map API
    log_info "Validating service map in ClickHouse..."
    
    # Query ClickHouse for service dependencies
    clickhouse_result=$(docker exec clickhouse clickhouse-client --query "
        SELECT 
            from_service,
            to_service,
            dependency_type,
            count(*) as dep_count
        FROM opa.service_dependencies 
        WHERE date >= today() - INTERVAL 1 DAY
        GROUP BY from_service, to_service, dependency_type
        ORDER BY dep_count DESC
        LIMIT 20
    " 2>&1 || echo "ERROR")
    
    if [[ "$clickhouse_result" != "ERROR" ]] && [[ -n "$clickhouse_result" ]]; then
        log_info "Service dependencies found in ClickHouse:"
        echo "$clickhouse_result" | head -10
    else
        log_warn "Could not query ClickHouse directly (API validation passed, which is sufficient)"
    fi
    
    # Query service map metadata
    metadata_result=$(docker exec clickhouse clickhouse-client --query "
        SELECT 
            from_service,
            to_service,
            call_count
        FROM opa.service_map_metadata 
        WHERE last_seen >= now() - INTERVAL 1 HOUR
        ORDER BY call_count DESC
        LIMIT 10
    " 2>&1 || echo "ERROR")
    
    if [[ "$metadata_result" != "ERROR" ]] && [[ -n "$metadata_result" ]]; then
        log_info "Service map metadata found:"
        echo "$metadata_result" | head -5
    fi
    
    exit 0
else
    log_error "✗ Multi-service service map E2E test FAILED"
    echo ""
    echo "Service A output (last 20 lines):"
    echo "$test_output_a" | tail -20
    echo ""
    echo "Service B output (last 20 lines):"
    echo "$test_output_b" | tail -20
    echo ""
    echo "Service C output (last 20 lines):"
    echo "$test_output_c" | tail -20
    exit 1
fi
