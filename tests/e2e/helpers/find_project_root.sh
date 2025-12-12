#!/bin/bash
# Helper script to find project root regardless of repository structure
# Works when php-extension is standalone or a submodule in myapm

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Start from helpers directory, go up to e2e, then tests, then php-extension
PHP_EXTENSION_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# Check if we're in a myapm structure (php-extension is a subdirectory)
if [[ -d "${PHP_EXTENSION_DIR}/../agent" ]] && [[ -d "${PHP_EXTENSION_DIR}/../clickhouse" ]]; then
    # We're in myapm structure
    PROJECT_ROOT="${PHP_EXTENSION_DIR}/.."
    echo "${PROJECT_ROOT}"
else
    # We're in standalone php-extension repo
    PROJECT_ROOT="${PHP_EXTENSION_DIR}"
    echo "${PROJECT_ROOT}"
fi

