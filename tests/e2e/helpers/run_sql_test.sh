#!/bin/bash
set -e

# Get database connection details from environment or use defaults
MYSQL_HOST=${MYSQL_HOST:-mysql}
MYSQL_PORT=${MYSQL_PORT:-3306}
MYSQL_USER=${MYSQL_USER:-test_user}
MYSQL_PASSWORD=${MYSQL_PASSWORD:-test_password}
MYSQL_ROOT_PASSWORD=${MYSQL_ROOT_PASSWORD:-root_password}

echo "=== SQL Profiling Test Runner ==="
echo "Waiting for MySQL to be ready..."

# Wait for MySQL to be ready
max_attempts=60
attempt=0
connected=false

while [ $attempt -lt $max_attempts ] && [ "$connected" = false ]; do
    # Use PHP to check MySQL connectivity
    if php -r "
        \$conn = @new mysqli('$MYSQL_HOST', 'root', '$MYSQL_ROOT_PASSWORD', '', $MYSQL_PORT);
        if (!\$conn->connect_error) {
            \$conn->close();
            exit(0);
        }
        exit(1);
    " 2>/dev/null; then
        connected=true
        echo "MySQL is ready!"
    else
        attempt=$((attempt + 1))
        if [ $((attempt % 5)) -eq 0 ]; then
            echo "Still waiting for MySQL... (attempt $attempt/$max_attempts)"
        else
            echo -n "."
        fi
        sleep 1
    fi
done

if [ "$connected" = false ]; then
    echo ""
    echo "ERROR: Could not connect to MySQL after $max_attempts attempts"
    exit 1
fi

echo ""
echo "Running SQL profiling tests..."
echo ""

# Run the test script
php /app/tests/e2e/sql_profiling_test/sql_profiling_test.php

exit_code=$?

if [ $exit_code -eq 0 ]; then
    echo ""
    echo "=== Tests completed successfully ==="
else
    echo ""
    echo "=== Tests failed with exit code $exit_code ==="
fi

exit $exit_code

