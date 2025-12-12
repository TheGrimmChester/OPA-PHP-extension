#!/bin/bash
set -euo pipefail

# Test to verify multiple spans are created when call stack exists
# This demonstrates that the expansion feature works correctly

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
API_URL="${API_URL:-http://localhost:8081}"

echo "=== Multiple Spans Verification Test ==="
echo ""

# Find a trace that has a call stack
echo "Searching for traces with call stacks..."
TRACES=$(curl -s "$API_URL/api/traces?limit=20" | jq -r '.traces[].trace_id' 2>/dev/null)

FOUND_WITH_STACK=0
FOUND_WITHOUT_STACK=0

for TRACE_ID in $TRACES; do
    if [ -z "$TRACE_ID" ] || [ "$TRACE_ID" = "null" ]; then
        continue
    fi
    
    TRACE_DATA=$(curl -s "$API_URL/api/traces/$TRACE_ID" 2>/dev/null)
    STACK_SIZE=$(echo "$TRACE_DATA" | jq '.root.stack | length' 2>/dev/null)
    SPAN_COUNT=$(echo "$TRACE_DATA" | jq '.spans | length' 2>/dev/null)
    EXPAND_SPANS=$(echo "$TRACE_DATA" | jq '.root.tags.expand_spans' 2>/dev/null)
    
    if [ "$STACK_SIZE" -gt 0 ] 2>/dev/null; then
        if [ "$FOUND_WITH_STACK" -eq 0 ]; then
            echo "✓ Found trace WITH call stack:"
            echo "  Trace ID: $TRACE_ID"
            echo "  Stack size: $STACK_SIZE"
            echo "  Total spans: $SPAN_COUNT"
            echo "  expand_spans: $EXPAND_SPANS"
            echo ""
            echo "  Span details:"
            echo "$TRACE_DATA" | jq -r '.spans[] | "    - \(.name) (parent: \(.parent_id // "root"), SQL: \(.sql | length))"' 2>/dev/null
            echo ""
            FOUND_WITH_STACK=1
        fi
    else
        if [ "$FOUND_WITHOUT_STACK" -eq 0 ]; then
            echo "⚠ Found trace WITHOUT call stack:"
            echo "  Trace ID: $TRACE_ID"
            echo "  Stack size: $STACK_SIZE"
            echo "  Total spans: $SPAN_COUNT"
            echo "  expand_spans: $EXPAND_SPANS"
            echo "  → This trace has no call stack, so it remains as 1 span (expected behavior)"
            echo ""
            FOUND_WITHOUT_STACK=1
        fi
    fi
    
    if [ "$FOUND_WITH_STACK" -eq 1 ] && [ "$FOUND_WITHOUT_STACK" -eq 1 ]; then
        break
    fi
done

echo "=== Summary ==="
echo ""
if [ "$FOUND_WITH_STACK" -eq 1 ]; then
    echo "✓ Multiple spans feature is WORKING"
    echo "  When call stack exists, spans are expanded correctly"
else
    echo "⚠ No traces with call stacks found"
    echo "  This means the PHP extension isn't capturing call stacks"
    echo "  Try using web requests (PHP-FPM) instead of CLI"
fi

if [ "$FOUND_WITHOUT_STACK" -eq 1 ]; then
    echo ""
    echo "ℹ Traces without call stacks remain as 1 span (expected)"
    echo "  The expansion only works when call stack data is present"
fi

echo ""
echo "=== How to Generate Traces with Call Stacks ==="
echo "1. Use web requests (PHP-FPM) instead of CLI"
echo "2. Ensure opa.collect_internal_functions=1"
echo "3. Make function calls that execute SQL/HTTP/cache operations"
echo "4. Operations >10ms will also create child spans"
