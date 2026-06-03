#!/usr/bin/env bash
# Test built-in DNS resolver: A, AAAA, CNAME, MX, TXT, NS, SRV, NXDOMAIN,
# TTL override, case insensitivity, and bind-address safety check.
# Run from the tests/ directory:  bash test_dns.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVER="$(cd "$SCRIPT_DIR/.." && pwd)/npserver"
CONFIG="$SCRIPT_DIR/test_dns.config.json"
HELPER="$SCRIPT_DIR/test_dns.py"
PORT=19953

SERVER_LOG="$(mktemp)"
SRV_PID=""

cleanup() {
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null && wait "$SRV_PID" 2>/dev/null
    rm -f "$SERVER_LOG"
}
trap cleanup EXIT

PASS=0
FAIL=0
TOTAL=0
pass() { PASS=$((PASS+1)); TOTAL=$((TOTAL+1)); printf "  \033[32mPASS\033[0m %s\n" "$1"; }
fail() { FAIL=$((FAIL+1)); TOTAL=$((TOTAL+1)); printf "  \033[31mFAIL\033[0m %s\n" "$1"; }

# ── Safety check tests (no server needed) ──────────────────────────────
echo "=== DNS bind-address safety checks ==="

# Helper: run server with a config, check if output contains a pattern
check_server_output() {
    local cfg="$1" pattern="$2" out
    out=$(timeout 2 "$SERVER" -config "$cfg" 2>&1 || true)
    echo "$out" | grep -q "$pattern"
}

# Wildcard bind must be rejected
TMPF=$(mktemp)
echo '{"listen":[{"name":"d","uri":"udp://:5353"}],"dns":{"a.test":{"A":"1.2.3.4"}},"dispatch":[{"listener":"d","handler":"dns"}]}' > "$TMPF"
if check_server_output "$TMPF" "Refusing"; then
    pass "udp://:5353 rejected (wildcard)"
else
    fail "udp://:5353 should be rejected"
fi
rm -f "$TMPF"

# 0.0.0.0 must be rejected
TMPF=$(mktemp)
echo '{"listen":[{"name":"d","uri":"udp://0.0.0.0:5353"}],"dns":{"a.test":{"A":"1.2.3.4"}},"dispatch":[{"listener":"d","handler":"dns"}]}' > "$TMPF"
if check_server_output "$TMPF" "Refusing"; then
    pass "udp://0.0.0.0:5353 rejected"
else
    fail "udp://0.0.0.0:5353 should be rejected"
fi
rm -f "$TMPF"

# [::] must be rejected
TMPF=$(mktemp)
echo '{"listen":[{"name":"d","uri":"udp://[::]:5353"}],"dns":{"a.test":{"A":"1.2.3.4"}},"dispatch":[{"listener":"d","handler":"dns"}]}' > "$TMPF"
if check_server_output "$TMPF" "Refusing"; then
    pass "udp://[::]:5353 rejected"
else
    fail "udp://[::]:5353 should be rejected"
fi
rm -f "$TMPF"

# 127.0.0.1 must be accepted
if check_server_output "$CONFIG" "dns handler"; then
    pass "udp://127.0.0.1:$PORT accepted"
else
    fail "udp://127.0.0.1:$PORT should be accepted"
fi

# ── Start server for record tests ──────────────────────────────────────
echo ""
echo "=== DNS record resolution tests ==="

"$SERVER" -config "$CONFIG" 2>"$SERVER_LOG" &
SRV_PID=$!
sleep 0.5

# Check server started
if ! kill -0 "$SRV_PID" 2>/dev/null; then
    echo "Server failed to start. Log:"
    cat "$SERVER_LOG"
    exit 1
fi

# ── Use Python helper for thorough record validation ───────────────────
PY_OUT=$(python3 "$HELPER" 127.0.0.1 "$PORT" 2>&1)
PY_RC=$?
echo "$PY_OUT"

# Count Python results (last line is "N/M tests passed")
PY_LINE=$(echo "$PY_OUT" | tail -1)
PY_P=$(echo "$PY_LINE" | grep -o '^[0-9]*')
PY_T=$(echo "$PY_LINE" | grep -o '/[0-9]*' | tr -d '/')
if [ -n "$PY_P" ] && [ -n "$PY_T" ]; then
    PASS=$((PASS + PY_P))
    TOTAL=$((TOTAL + PY_T))
    if [ "$PY_P" -ne "$PY_T" ]; then
        FAIL=$((FAIL + PY_T - PY_P))
    fi
fi

# ── dig smoke tests (if dig is available) ──────────────────────────────
if command -v dig &>/dev/null; then
    echo ""
    echo "=== dig smoke tests ==="

    A_RESULT=$(dig @127.0.0.1 -p "$PORT" single.test A +short +tries=1 +time=2 2>/dev/null)
    if [ "$A_RESULT" = "10.0.0.1" ]; then pass "dig A single.test"
    else fail "dig A single.test (got: '$A_RESULT')"; fi

    AAAA_RESULT=$(dig @127.0.0.1 -p "$PORT" dual.test AAAA +short +tries=1 +time=2 2>/dev/null)
    if [ "$AAAA_RESULT" = "fd00::2" ]; then pass "dig AAAA dual.test"
    else fail "dig AAAA dual.test (got: '$AAAA_RESULT')"; fi

    NX_STATUS=$(dig @127.0.0.1 -p "$PORT" nope.test A +tries=1 +time=2 +noall +comments 2>/dev/null | grep -o 'status: [A-Z]*')
    if [ "$NX_STATUS" = "status: NXDOMAIN" ]; then pass "dig NXDOMAIN"
    else fail "dig NXDOMAIN (got: '$NX_STATUS')"; fi

    MX_RESULT=$(dig @127.0.0.1 -p "$PORT" mail.test MX +short +tries=1 +time=2 2>/dev/null | head -1)
    if echo "$MX_RESULT" | grep -q "mx1.mail.test"; then pass "dig MX mail.test"
    else fail "dig MX mail.test (got: '$MX_RESULT')"; fi
else
    echo "(dig not found, skipping dig smoke tests)"
fi

# ── Summary ────────────────────────────────────────────────────────────
echo ""
echo "$PASS/$TOTAL tests passed"
[ "$FAIL" -eq 0 ]
