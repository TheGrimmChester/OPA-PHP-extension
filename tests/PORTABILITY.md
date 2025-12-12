# Test Portability

The E2E tests are designed to work in multiple repository contexts:

## Repository Contexts

### 1. myapm Repository (php-extension as subdirectory)
```
myapm/
├── agent/
├── clickhouse/
├── php-extension/
│   └── tests/
│       └── e2e/
│           └── helpers/
│               └── common.sh
└── run-tests.sh
```

When in this structure:
- `PROJECT_ROOT` = `/path/to/myapm`
- `PHP_EXTENSION_DIR` = `/path/to/myapm/php-extension`
- Tests use `docker-compose.test.yml` from myapm root
- Tests use `run-tests.sh` from myapm root

### 2. Standalone php-extension Repository
```
php-extension/
└── tests/
    └── e2e/
        └── helpers/
            └── common.sh
```

When in this structure:
- `PROJECT_ROOT` = `/path/to/php-extension`
- `PHP_EXTENSION_DIR` = `/path/to/php-extension`
- Tests require external infrastructure setup

## How It Works

All E2E tests source `helpers/common.sh` which:

1. **Detects repository structure** by checking for `agent/` and `clickhouse/` directories
2. **Sets PROJECT_ROOT** appropriately based on structure
3. **Detects environment** (container vs host) and sets API URLs accordingly
4. **Exports variables** for use in test scripts

## Usage in Tests

All test scripts should start with:

```bash
#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Source common helpers for path detection
if [[ -f "${SCRIPT_DIR}/helpers/common.sh" ]]; then
    source "${SCRIPT_DIR}/helpers/common.sh"
else
    # Fallback if common.sh not available
    PHP_EXTENSION_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
    if [[ -d "${PHP_EXTENSION_DIR}/../agent" ]] && [[ -d "${PHP_EXTENSION_DIR}/../clickhouse" ]]; then
        PROJECT_ROOT="${PHP_EXTENSION_DIR}/.."
    else
        PROJECT_ROOT="${PHP_EXTENSION_DIR}"
    fi
    export PROJECT_ROOT
    export PHP_EXTENSION_DIR
fi

# Now use $PROJECT_ROOT and other variables
```

## Environment Variables

The `common.sh` script sets these variables based on context:

**In Container:**
- `API_URL=http://agent:8080`
- `BASE_URL=http://nginx-test`
- `MYSQL_HOST=mysql-test`

**On Host:**
- `API_URL=http://localhost:8081`
- `BASE_URL=http://localhost:8090`
- `MYSQL_HOST=localhost`

## Running Tests

### From myapm Repository
```bash
./run-tests.sh                    # Run all tests
./run-tests.sh http_errors/test_http_errors_e2e.sh  # Run specific test
./run-tests.sh --no-cleanup       # Preserve test data
```

### From php-extension Directory
```bash
cd tests
./run-e2e-tests.sh                # Delegates to myapm runner if available
```

## Benefits

- ✅ Tests work in both repository contexts
- ✅ No hardcoded paths
- ✅ Automatic environment detection
- ✅ Easy to add new tests
- ✅ Works as submodule or standalone

