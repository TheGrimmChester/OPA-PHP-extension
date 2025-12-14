#!/bin/bash
# Common functions and path detection for E2E tests
# Works when php-extension is standalone or a submodule in myapm

# Get the script directory (this file is in helpers/)
HELPERS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Find php-extension directory
# Structure: php-extension/tests/e2e/helpers/common.sh
# From helpers, go up: helpers -> e2e -> tests -> php-extension (3 levels up)
PHP_EXTENSION_DIR="$(cd "${HELPERS_DIR}/../../.." && pwd)"

# Detect repository structure and set PROJECT_ROOT
detect_project_root() {
    # PHP_EXTENSION_DIR is already the php-extension directory
    # Check if we're in a myapm structure (php-extension is a subdirectory)
    # Look for agent and clickhouse directories at the same level as php-extension
    # Use absolute path for parent directory
    local parent_dir="$(cd "${PHP_EXTENSION_DIR}/.." && pwd)"
    if [[ -d "${parent_dir}/agent" ]] && [[ -d "${parent_dir}/clickhouse" ]]; then
        # We're in myapm structure - PROJECT_ROOT is one level up from php-extension
        echo "${parent_dir}"
    else
        # We're in standalone php-extension repo - PROJECT_ROOT is the php-extension directory itself
        echo "${PHP_EXTENSION_DIR}"
    fi
}

PROJECT_ROOT="${PROJECT_ROOT:-$(detect_project_root)}"

# Export for use in other scripts
export PROJECT_ROOT
export PHP_EXTENSION_DIR

# Detect if running in Docker container
detect_environment() {
    if [[ -f /.dockerenv ]] || [[ -n "${DOCKER_CONTAINER:-}" ]]; then
        export IN_CONTAINER=1
        export API_URL="${API_URL:-http://agent:8080}"
        export BASE_URL="${BASE_URL:-http://nginx-test}"
        export MYSQL_HOST="${MYSQL_HOST:-mysql-test}"
        export MYSQL_PORT="${MYSQL_PORT:-3306}"
        # In container, tests are mounted at /app/tests
        # Always use /app/tests in container, regardless of PHP_EXTENSION_DIR resolution
        if [[ -d "/app/tests" ]]; then
            export TESTS_DIR="/app/tests"
        elif [[ -d "/var/www/html/tests" ]]; then
            export TESTS_DIR="/var/www/html/tests"
        else
            # Fallback: use PHP_EXTENSION_DIR but ensure no double /tests
            if [[ "${PHP_EXTENSION_DIR}" == "/app" ]] || [[ "${PHP_EXTENSION_DIR}" == "/usr/src/opa" ]]; then
                export TESTS_DIR="/app/tests"
            else
                export TESTS_DIR="${PHP_EXTENSION_DIR}/tests"
            fi
        fi
    else
        export IN_CONTAINER=0
        export API_URL="${API_URL:-http://localhost:8081}"
        export BASE_URL="${BASE_URL:-http://localhost:8090}"
        export MYSQL_HOST="${MYSQL_HOST:-localhost}"
        export MYSQL_PORT="${MYSQL_PORT:-3307}"
        export TESTS_DIR="${PHP_EXTENSION_DIR}/tests"
    fi
    
    # Export other common variables
    export MYSQL_DATABASE="${MYSQL_DATABASE:-test_db}"
    export MYSQL_USER="${MYSQL_USER:-test_user}"
    export MYSQL_PASSWORD="${MYSQL_PASSWORD:-test_password}"
    export MYSQL_ROOT_PASSWORD="${MYSQL_ROOT_PASSWORD:-root_password}"
}

# Auto-detect environment on source
detect_environment

