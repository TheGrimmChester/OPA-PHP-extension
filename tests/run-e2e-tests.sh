#!/bin/bash
set -euo pipefail

# Portable E2E test runner for php-extension
# Works when php-extension is standalone or a submodule in myapm
# Usage: ./run-e2e-tests.sh [test-script-path] [--verbose] [--cleanup|--no-cleanup]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PHP_EXTENSION_DIR="${SCRIPT_DIR}"
PROJECT_ROOT=""

# Detect repository structure
if [[ -d "${PHP_EXTENSION_DIR}/../agent" ]] && [[ -d "${PHP_EXTENSION_DIR}/../clickhouse" ]]; then
    # We're in myapm structure (php-extension is a subdirectory)
    PROJECT_ROOT="${PHP_EXTENSION_DIR}/.."
    DOCKER_COMPOSE_FILE="${PROJECT_ROOT}/docker-compose.test.yml"
    RUN_TESTS_SCRIPT="${PROJECT_ROOT}/run-tests.sh"
    
    # Use the main test runner if it exists
    if [[ -f "${RUN_TESTS_SCRIPT}" ]]; then
        exec "${RUN_TESTS_SCRIPT}" "$@"
    fi
else
    # We're in standalone php-extension repo
    PROJECT_ROOT="${PHP_EXTENSION_DIR}"
    DOCKER_COMPOSE_FILE="${PHP_EXTENSION_DIR}/docker/compose/docker-compose.test.yml"
fi

# Configuration
VERBOSE="${VERBOSE:-0}"
CLEANUP="${CLEANUP:-1}"
TEST_SCRIPT=""
NETWORK_NAME="opa_network"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --verbose|-v)
            VERBOSE=1
            shift
            ;;
        --no-cleanup)
            CLEANUP=0
            shift
            ;;
        --cleanup)
            CLEANUP=1
            shift
            ;;
        --help|-h)
            echo "Usage: $0 [test-script] [--verbose] [--cleanup|--no-cleanup]"
            echo ""
            echo "Options:"
            echo "  test-script    Path to specific test script (relative to tests/e2e/)"
            echo "  --verbose, -v  Enable verbose output"
            echo "  --cleanup      Clean up services after tests (default)"
            echo "  --no-cleanup   Keep services running after tests (preserves test data)"
            exit 0
            ;;
        -*)
            echo "Unknown option: $1" >&2
            echo "Use --help for usage information" >&2
            exit 1
            ;;
        *)
            if [[ -z "$TEST_SCRIPT" ]]; then
                TEST_SCRIPT="$1"
            else
                echo "Multiple test scripts specified: $TEST_SCRIPT and $1" >&2
                exit 1
            fi
            shift
            ;;
    esac
done

log_info() {
    if [[ "$VERBOSE" -eq 1 ]] || [[ "${1:-}" == "force" ]]; then
        echo -e "${GREEN}[INFO]${NC} $*"
    fi
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $*" >&2
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $*" >&2
}

# Ensure Docker network exists
ensure_network() {
    if ! docker network inspect "$NETWORK_NAME" > /dev/null 2>&1; then
        log_info "Creating Docker network: $NETWORK_NAME"
        docker network create "$NETWORK_NAME" || true
    fi
}

# Check if we have docker-compose file
if [[ ! -f "${DOCKER_COMPOSE_FILE}" ]]; then
    log_error "Docker compose file not found: ${DOCKER_COMPOSE_FILE}"
    log_error "Please ensure you're in the correct directory or have the required files"
    exit 1
fi

log_info "Using project root: ${PROJECT_ROOT}"
log_info "Using docker-compose: ${DOCKER_COMPOSE_FILE}"

# For standalone repo, we need to set up a minimal docker-compose
# For now, delegate to the main runner if in myapm, otherwise show instructions
if [[ "${PROJECT_ROOT}" == "${PHP_EXTENSION_DIR}" ]]; then
    log_warn "Standalone php-extension repo detected"
    log_warn "For full E2E tests, please use this from the myapm repository"
    log_warn "Or set up the required services manually"
    exit 1
fi

# If we reach here, we should have delegated to the main runner
# This should not execute, but just in case:
log_error "Unexpected execution path"
exit 1

