#!/bin/bash
# Script to update all E2E tests to use common.sh
# This makes tests portable across repository contexts

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
E2E_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Template for path detection
PATH_DETECTION_TEMPLATE='SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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
fi'

echo "Finding test files that need updating..."

# Find all test scripts that don't use common.sh
find "$E2E_DIR" -name "*_e2e.sh" -type f | while read -r file; do
    if ! grep -q "helpers/common.sh" "$file" 2>/dev/null; then
        echo "Would update: $file"
        # In a real scenario, you'd backup and update here
    fi
done

echo "Done. Review the files above before applying changes."

