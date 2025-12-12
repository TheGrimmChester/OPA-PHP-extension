#!/bin/bash
# Helper script to run tests in Docker containers
# This script detects the environment and adjusts URLs accordingly

# Detect if running in Docker container
if [[ -f /.dockerenv ]] || [[ -n "${DOCKER_CONTAINER:-}" ]]; then
    export IN_CONTAINER=1
    export API_URL="${API_URL:-http://agent:8080}"
    export BASE_URL="${BASE_URL:-http://nginx-test}"
    export MYSQL_HOST="${MYSQL_HOST:-mysql-test}"
    export MYSQL_PORT="${MYSQL_PORT:-3306}"
else
    export IN_CONTAINER=0
    export API_URL="${API_URL:-http://localhost:8081}"
    export BASE_URL="${BASE_URL:-http://localhost:8088}"
    export MYSQL_HOST="${MYSQL_HOST:-localhost}"
    export MYSQL_PORT="${MYSQL_PORT:-3307}"
fi

# Export other common variables
export MYSQL_DATABASE="${MYSQL_DATABASE:-test_db}"
export MYSQL_USER="${MYSQL_USER:-test_user}"
export MYSQL_PASSWORD="${MYSQL_PASSWORD:-test_password}"
export MYSQL_ROOT_PASSWORD="${MYSQL_ROOT_PASSWORD:-root_password}"

# Run the provided command
exec "$@"

