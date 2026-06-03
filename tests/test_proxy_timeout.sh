#!/usr/bin/env bash
# Test proxy pool failover: dead backend → 502, mark down, skip on retry.
# Run from the tests/ directory:  bash test_proxy_timeout.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVER="$(cd "$SCRIPT_DIR/.." && pwd)/npserver"
CONFIG="$SCRIPT_DIR/test_proxy_timeout.config.json"
BACKEND="$SCRIPT_DIR/test_proxy_timeout.py"

TMPDIR="$(mktemp -d)"
mkdir -p "$TMPDIR/html"
SERVER_LOG="$TMPDIR/server.log"

cleanup() {
    kill "$PY_PID"  2>/dev/null || true
    kill "$SRV_PID" 2>/dev/null || true
    wait "$PY_PID"  2>/dev/null || true
    wait "$SRV_PID" 2>/dev/null || true
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

PASS=0
FAIL=0
pass() { PASS=$((PASS+1)); printf "  \033[32mPASS\033[0m %s\n" "$1"; }
fail() { FAIL=$((FAIL+1)); printf "  \033[31mFAIL\033[0m %s\n" "$1"; }

# Start only the live backend on :19991 (port 19990 is intentionally dead)
python3 "$BACKEND" 19991 &
PY_PID=$!

# Start nanoserver from tmpdir (needs html/ for fallback)
cd "$TMPDIR"
"$SERVER" -config "$CONFIG" 2>"$SERVER_LOG" &
SRV_PID=$!

sleep 0.5

echo "=== Proxy pool failover tests ==="

# 1. Dead backend returns 502
STATUS=$(curl -s -o /dev/null -w "%{http_code}" --max-time 5 http://127.0.0.1:19903/app/test)
if [ "$STATUS" = "502" ]; then pass "Dead backend (:19990) returns 502"
else fail "Dead backend: expected 502, got $STATUS"; fi

# 2. Live backend works
STATUS=$(curl -s -o /dev/null -w "%{http_code}" --max-time 5 http://127.0.0.1:19903/app/test)
if [ "$STATUS" = "200" ]; then pass "Live backend (:19991) returns 200"
else fail "Live backend: expected 200, got $STATUS"; fi

# 3. Dead target is skipped (marked down), request goes to live
STATUS=$(curl -s -o /dev/null -w "%{http_code}" --max-time 5 http://127.0.0.1:19903/app/test)
if [ "$STATUS" = "200" ]; then pass "Dead target skipped, got 200"
else fail "Dead target skip: expected 200, got $STATUS"; fi

# 4. Server logs show mark-down message
if grep -q "marked DOWN" "$SERVER_LOG"; then pass "Server logged 'marked DOWN'"
else fail "'marked DOWN' not found in server log"; fi

echo ""
echo "$PASS/4 tests passed"
[ "$PASS" -eq 4 ]
