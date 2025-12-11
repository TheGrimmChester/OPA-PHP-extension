#!/bin/bash
set -euo pipefail

# Script to verify errors in ClickHouse
# Checks if errors are stored in error_instances and error_groups tables

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="${PROJECT_ROOT:-$(cd "${SCRIPT_DIR}/.." && pwd)}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "=== Error Verification in ClickHouse ==="
echo ""

cd "${PROJECT_ROOT}" || exit 1

# Check if docker-compose is available
if ! command -v docker-compose &> /dev/null; then
    echo -e "${RED}Error: docker-compose not found${NC}"
    exit 1
fi

# Query ClickHouse
clickhouse_query() {
    local query="$1"
    docker-compose exec -T clickhouse clickhouse-client --query "$query" 2>/dev/null || echo ""
}

echo -e "${GREEN}1. Checking error_instances table...${NC}"
error_count=$(clickhouse_query "SELECT count() FROM opa.error_instances" | tr -d '\n\r ' || echo "0")
echo "   Total error instances: $error_count"

if [[ "$error_count" -gt 0 ]]; then
    echo -e "\n${GREEN}2. Recent error instances:${NC}"
    clickhouse_query "SELECT error_type, error_message, service, environment, release, occurred_at FROM opa.error_instances ORDER BY occurred_at DESC LIMIT 10 FORMAT PrettyCompact" || true
    
    echo -e "\n${GREEN}3. Error types breakdown:${NC}"
    clickhouse_query "SELECT error_type, count() as cnt FROM opa.error_instances GROUP BY error_type ORDER BY cnt DESC FORMAT PrettyCompact" || true
    
    echo -e "\n${GREEN}4. Errors by service:${NC}"
    clickhouse_query "SELECT service, count() as cnt FROM opa.error_instances GROUP BY service ORDER BY cnt DESC FORMAT PrettyCompact" || true
    
    echo -e "\n${GREEN}5. Errors by environment:${NC}"
    clickhouse_query "SELECT environment, count() as cnt FROM opa.error_instances WHERE environment != '' GROUP BY environment ORDER BY cnt DESC FORMAT PrettyCompact" || true
    
    echo -e "\n${GREEN}6. Errors with user context:${NC}"
    clickhouse_query "SELECT count() as cnt FROM opa.error_instances WHERE user_context != '{}' AND user_context != ''" | tr -d '\n\r ' || echo "0"
    echo " error(s) with user context"
    
    echo -e "\n${GREEN}7. Sample error with full context:${NC}"
    clickhouse_query "SELECT error_type, error_message, environment, release, user_context, tags FROM opa.error_instances ORDER BY occurred_at DESC LIMIT 1 FORMAT JSONEachRow" | jq '.' 2>/dev/null || true
else
    echo -e "${YELLOW}   No errors found in error_instances table${NC}"
fi

echo -e "\n${GREEN}8. Checking error_groups table...${NC}"
group_count=$(clickhouse_query "SELECT count() FROM opa.error_groups" | tr -d '\n\r ' || echo "0")
echo "   Total error groups: $group_count"

if [[ "$group_count" -gt 0 ]]; then
    echo -e "\n${GREEN}9. Recent error groups:${NC}"
    clickhouse_query "SELECT error_type, error_message, service, count, first_seen, last_seen FROM opa.error_groups ORDER BY last_seen DESC LIMIT 10 FORMAT PrettyCompact" || true
else
    echo -e "${YELLOW}   No error groups found${NC}"
fi

echo -e "\n${GREEN}10. Checking spans_min for error status...${NC}"
span_error_count=$(clickhouse_query "SELECT count() FROM opa.spans_min WHERE status = 'error' OR status = '0'" | tr -d '\n\r ' || echo "0")
echo "   Spans with error status: $span_error_count"

if [[ "$span_error_count" -gt 0 ]]; then
    echo -e "\n${GREEN}11. Recent error spans:${NC}"
    clickhouse_query "SELECT service, name, status, start_ts FROM opa.spans_min WHERE status = 'error' OR status = '0' ORDER BY start_ts DESC LIMIT 10 FORMAT PrettyCompact" || true
fi

echo ""
echo "=== Summary ==="
echo "Error instances: $error_count"
echo "Error groups: $group_count"
echo "Error spans: $span_error_count"

if [[ "$error_count" -gt 0 ]] || [[ "$group_count" -gt 0 ]]; then
    echo -e "\n${GREEN}✓ Errors are stored in ClickHouse!${NC}"
    echo -e "${YELLOW}Note: Errors are stored correctly in ClickHouse${NC}"
else
    echo -e "\n${RED}✗ No errors found in ClickHouse${NC}"
    echo "   Make sure:"
    echo "   1. Errors are being generated (run test scripts)"
    echo "   2. Agent is running and processing errors"
    echo "   3. ClickHouse is accessible"
fi
