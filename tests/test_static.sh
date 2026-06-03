#!/usr/bin/env bash
# Test static file serving: HEAD, GET, Range (lowercase), gzip compression.
# Run from the tests/ directory:  bash test_static.sh
set -uo pipefail

SERVER="$(cd "$(dirname "$0")/.." && pwd)/npserver"
PORT=19901
PASS=0
FAIL=0

pass() { PASS=$((PASS+1)); printf "  \033[32mPASS\033[0m %s\n" "$1"; }
fail() { FAIL=$((FAIL+1)); printf "  \033[31mFAIL\033[0m %s\n" "$1"; }

# ── setup temp site ─────────────────────────────────────────────────────
TMPDIR="$(mktemp -d)"
mkdir -p "$TMPDIR/html"

cat > "$TMPDIR/html/index.html" <<'HTML'
<!DOCTYPE html>
<html lang="en"><head><title>Test</title></head>
<body>
  <h1>Lorem Ipsum</h1>
  <p>Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed do eiusmod
  tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam,
  quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo
  consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse
  cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat
  non proident, sunt in culpa qui officia deserunt mollit anim id est laborum.</p>
  <p>Curabitur pretium tincidunt lacus. Nulla gravida orci a odio. Nullam varius,
  turpis et commodo pharetra, est eros bibendum elit.</p>
</body>
</html>
HTML

# ── start server (from tmpdir so "html/" default root works) ────────────
cd "$TMPDIR"
"$SERVER" "http://:$PORT" 2>/dev/null &
SERVER_PID=$!

cleanup() {
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    rm -rf "$TMPDIR"
}
trap cleanup EXIT
sleep 0.3

URL="http://127.0.0.1:$PORT/"

echo "=== Static file serving tests (port $PORT) ==="

# 1. HEAD returns no body
DL=$(curl --max-time 2 -s -o /dev/null -w '%{size_download}' -X HEAD -H 'Connection: close' "$URL")
if [ "$DL" = "0" ]; then pass "HEAD returns no body (size_download=0)"
else fail "HEAD returns no body (got $DL)"; fi

# 2. GET returns full body
DL=$(curl --max-time 2 -s -o /dev/null -w '%{size_download}' "$URL")
if [ "$DL" -gt 0 ] 2>/dev/null; then pass "GET returns full body ($DL bytes)"
else fail "GET returns full body (got $DL)"; fi

# 3. Lowercase range header → 206
RESP=$(curl --max-time 2 -s -D- -H "range: bytes=0-10" -o /dev/null "$URL")
if echo "$RESP" | grep -q "206"; then pass "Lowercase range header returns 206"
else fail "Lowercase range header (got: $(echo "$RESP" | head -1))"; fi

# 4. Gzip Content-Encoding header present
RESP=$(curl --max-time 2 -s -D- -H "Accept-Encoding: gzip" -o /dev/null "$URL")
if echo "$RESP" | grep -qi "Content-Encoding: gzip"; then pass "Gzip Content-Encoding header present"
else fail "Gzip Content-Encoding missing"; fi

# 5. Gzip actually reduces size
PLAIN=$(curl --max-time 2 -s -o /dev/null -w '%{size_download}' "$URL")
GZIP=$(curl --max-time 2 -s -o /dev/null -w '%{size_download}' -H "Accept-Encoding: gzip" "$URL")
if [ "$GZIP" -lt "$PLAIN" ] 2>/dev/null; then pass "Gzip reduces size ($GZIP < $PLAIN)"
else fail "Gzip not smaller (gzip=$GZIP plain=$PLAIN)"; fi

echo ""
echo "$PASS/5 tests passed"
[ "$PASS" -eq 5 ]
