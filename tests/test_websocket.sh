#!/usr/bin/env bash
# Test WebSocket proxy passthrough through nanoserver.
# Run from the tests/ directory:  bash test_websocket.sh
set -uo pipefail

SERVER="$(cd "$(dirname "$0")/.." && pwd)/npserver"
PROXY_PORT=19902
BACKEND_PORT=19999
VENV=/tmp/ws-test-venv
PASS=0
FAIL=0

pass() { PASS=$((PASS+1)); printf "  \033[32mPASS\033[0m %s\n" "$1"; }
fail() { FAIL=$((FAIL+1)); printf "  \033[31mFAIL\033[0m %s\n" "$1"; }

# ── Python venv with websockets ─────────────────────────────────────────
if [ ! -f "$VENV/bin/python" ]; then
    echo "Creating Python venv at $VENV ..."
    python3 -m venv "$VENV"
fi
"$VENV/bin/pip" install -q websockets 2>/dev/null
PY="$VENV/bin/python"

# ── temp dir ────────────────────────────────────────────────────────────
TMPDIR="$(mktemp -d)"
mkdir -p "$TMPDIR/html"

# ── nanoserver config ───────────────────────────────────────────────────
cat > "$TMPDIR/config.json" <<EOF
{
  "listen": [{ "name": "web", "uri": "http://:$PROXY_PORT" }],
  "dispatch": [
    { "listener": "web", "handler": "proxy", "path": "/ws/**",
      "proxy_pass": "http://127.0.0.1:$BACKEND_PORT" }
  ]
}
EOF

# ── echo server (backend) ──────────────────────────────────────────────
cat > "$TMPDIR/ws_echo.py" <<'PYEOF'
import asyncio, websockets
async def echo(ws):
    async for msg in ws:
        await ws.send(msg)
async def main():
    async with websockets.serve(echo, "127.0.0.1", 19999):
        await asyncio.Future()
asyncio.run(main())
PYEOF

# ── client helper ───────────────────────────────────────────────────────
cat > "$TMPDIR/ws_client.py" <<'PYEOF'
import asyncio, sys, websockets
async def run(uri):
    async with websockets.connect(uri) as ws:
        for msg in ["hello", "world"]:
            await ws.send(msg)
            reply = await asyncio.wait_for(ws.recv(), timeout=3)
            if reply != msg:
                print(f"MISMATCH: sent {msg!r}, got {reply!r}")
                sys.exit(1)
    print("PASS")
asyncio.run(run(sys.argv[1]))
PYEOF

# ── cleanup trap ────────────────────────────────────────────────────────
ECHO_PID=""
SRV_PID=""
cleanup() {
    [ -n "$ECHO_PID" ]  && kill "$ECHO_PID"  2>/dev/null || true
    [ -n "$SRV_PID" ]   && kill "$SRV_PID"   2>/dev/null || true
    wait "$ECHO_PID"  2>/dev/null || true
    wait "$SRV_PID"   2>/dev/null || true
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

# ── start processes ─────────────────────────────────────────────────────
"$PY" "$TMPDIR/ws_echo.py" &
ECHO_PID=$!
sleep 0.5

cd "$TMPDIR"
"$SERVER" -config "$TMPDIR/config.json" 2>/dev/null &
SRV_PID=$!
sleep 0.3

echo "=== WebSocket proxy tests ==="

# 1. Direct WebSocket to backend (sanity check)
OUT=$("$PY" "$TMPDIR/ws_client.py" "ws://127.0.0.1:$BACKEND_PORT" 2>&1) || true
if [ "$OUT" = "PASS" ]; then pass "Direct WebSocket echo"
else fail "Direct WebSocket echo ($OUT)"; fi

# 2. Proxied WebSocket through nanoserver
OUT=$("$PY" "$TMPDIR/ws_client.py" "ws://127.0.0.1:$PROXY_PORT/ws/" 2>&1) || true
if [ "$OUT" = "PASS" ]; then pass "Proxied WebSocket echo"
else fail "Proxied WebSocket echo ($OUT)"; fi

echo ""
echo "$PASS/2 tests passed"
[ "$PASS" -eq 2 ]
