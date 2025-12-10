# Profiling Tests

This directory contains end-to-end tests for HTTP/cURL request profiling and SQL query profiling.

## Test Files

### cURL Profiling Tests

- `test_curl_ci_e2e.sh` - Comprehensive CI test with mock HTTP server
- `test_curl_trace_e2e.sh` - Trace validation test
- `test_http_mock_server.py` - Mock HTTP server for testing

### SQL Profiling Tests

- `test_sql_ci_e2e.sh` - Comprehensive CI test with MySQL database
- `test_sql_trace_e2e.sh` - Trace validation test

### Common

- `docker-compose.test.yml` - Docker Compose config for test services (mock HTTP server and MySQL)

## Running Tests

### cURL Profiling Tests

#### CI Test (with mock server)

```bash
./test_curl_ci_e2e.sh
./test_curl_ci_e2e.sh --verbose
CI_MODE=1 ./test_curl_ci_e2e.sh --ci
```

#### Trace Validation Test

```bash
./test_curl_trace_e2e.sh
./test_curl_trace_e2e.sh --verbose
```

### SQL Profiling Tests

#### CI Test (with MySQL)

```bash
./test_sql_ci_e2e.sh
./test_sql_ci_e2e.sh --verbose
CI_MODE=1 ./test_sql_ci_e2e.sh --ci
```

#### Trace Validation Test

```bash
./test_sql_trace_e2e.sh
./test_sql_trace_e2e.sh --verbose
```

## Test Scenarios

### cURL Profiling

The CI test validates:
- Various HTTP status codes (200, 404, 500, 201, 301, 403)
- Response timing (delayed responses)
- Response sizes (small and large)
- HTTP request structure (URL, method, status, duration)
- Data storage in ClickHouse
- API response validation

### SQL Profiling

The CI test validates:
- Various SQL query types (SELECT, INSERT, UPDATE, DELETE, CREATE TABLE)
- Row counts (single row, multiple rows, COUNT queries)
- Query execution timing
- SQL query structure (query text, duration, type, rows_affected)
- MySQLi and PDO support
- Prepared statements
- Data storage in ClickHouse
- API response validation

## Requirements

- Docker and Docker Compose
- Python 3 (for mock HTTP server)
- curl, jq (for validation)
- Access to opa_network Docker network
- Running agent and ClickHouse services
- MySQL 8.0+ (for SQL tests, provided via docker-compose.test.yml)
