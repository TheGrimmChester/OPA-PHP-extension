# E2E Tests

End-to-end tests for the PHP extension. These tests work in multiple repository contexts:

## Repository Contexts

### 1. Standalone php-extension Repository
When the `php-extension` is a standalone repository, tests can be run but require the full myapm infrastructure (agent, ClickHouse, etc.) to be set up separately.

### 2. myapm Repository (php-extension as subdirectory)
When `php-extension` is part of the `myapm` repository, tests use the full infrastructure via `docker-compose.test.yml` and `run-tests.sh`.

## Running Tests

### From myapm Repository (Recommended)

```bash
# From myapm root
./run-tests.sh                    # Run all tests
./run-tests.sh --no-cleanup       # Run tests and preserve data
./run-tests.sh http_errors/test_http_errors_e2e.sh  # Run specific test
```

### From php-extension Directory

```bash
# From php-extension/tests
./run-e2e-tests.sh                # Delegates to myapm runner if available
```

## Test Structure

All E2E tests:
- Auto-detect repository structure (standalone vs myapm)
- Auto-detect environment (container vs host)
- Use common helpers for path resolution
- Support both contexts transparently

## Common Helpers

- `helpers/common.sh` - Path detection and environment setup
- `helpers/find_project_root.sh` - Finds project root in any context
- `helpers/run_test_in_container.sh` - Container environment detection
- `helpers/create_test_endpoint.sh` - Creates test endpoints

## Environment Variables

Tests automatically adjust based on context:

**In Container:**
- `API_URL=http://agent:8080`
- `BASE_URL=http://nginx-test`
- `MYSQL_HOST=mysql-test`

**On Host:**
- `API_URL=http://localhost:8081`
- `BASE_URL=http://localhost:8090`
- `MYSQL_HOST=localhost`

## Adding New Tests

When creating new E2E tests, use the common helpers:

```bash
#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Source common helpers
if [[ -f "${SCRIPT_DIR}/helpers/common.sh" ]]; then
    source "${SCRIPT_DIR}/helpers/common.sh"
else
    # Fallback path detection
    PHP_EXTENSION_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
    PROJECT_ROOT="${PHP_EXTENSION_DIR}"
    export PROJECT_ROOT PHP_EXTENSION_DIR
fi

# Use PROJECT_ROOT and other variables from common.sh
# Tests will work in both standalone and myapm contexts
```

