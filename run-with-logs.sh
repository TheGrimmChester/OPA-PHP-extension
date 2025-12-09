#!/bin/bash
# Script to run php-extension tests and show logs from all services

cd "$(dirname "$0")"

echo "Starting php-extension services..."
docker-compose up -d mysql

echo "Waiting for mysql to be healthy..."
docker-compose up --abort-on-container-exit php &
PHP_PID=$!

# Follow logs from all relevant containers
docker logs -f opa-agent opa-dashboard-dev opa_mysql opa_php 2>&1 &
LOGS_PID=$!

# Wait for php container to exit
wait $PHP_PID
EXIT_CODE=$?

# Stop log following
kill $LOGS_PID 2>/dev/null

exit $EXIT_CODE
