#!/usr/bin/env bash
# Real, local, no-hardware Phase 1 end-to-end proof for EDGE.GAME (see NORTHSTAR.md's own phased
# plan). Starts the relay + a real client, sends real "route" commands through the actual
# HTTP -> TCP -> compiled-PARENA-decision path (PARENA/stdlib/edge_game/traffic_router.prn), checks
# each reply against the real, expected routing decision, then shuts both processes down. Does NOT
# touch any real hardware -- that's Phase 5+, genuinely blocked on the founder's own machine.
set -euo pipefail

: "${EDGE_CLIENT_TOKEN:?}"
: "${EDGE_OPERATOR_TOKEN:?}"
: "${EDGE_CLIENT_PORT:?}"
: "${EDGE_OPERATOR_PORT:?}"

RELAY_LOG="$(mktemp)"
CLIENT_LOG="$(mktemp)"
RELAY_PID=""
CLIENT_PID=""

cleanup() {
  [ -n "$RELAY_PID" ] && kill "$RELAY_PID" 2>/dev/null || true
  [ -n "$CLIENT_PID" ] && kill "$CLIENT_PID" 2>/dev/null || true
  wait 2>/dev/null || true
  rm -f "$RELAY_LOG" "$CLIENT_LOG"
}
trap cleanup EXIT

node server/relay.js > "$RELAY_LOG" 2>&1 &
RELAY_PID=$!
sleep 0.5

./build/edge_client 127.0.0.1 "$EDGE_CLIENT_PORT" "$EDGE_CLIENT_TOKEN" > "$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!
sleep 0.5

fail=0
check() {
  local desc="$1" want="$2" got="$3"
  if [ "$got" != "$want" ]; then
    echo "FAIL: $desc -- want '$want', got '$got'"
    fail=1
  else
    echo "PASS: $desc"
  fi
}

COMMANDS_URL="http://127.0.0.1:$EDGE_OPERATOR_PORT/commands"
HEALTH_URL="http://127.0.0.1:$EDGE_OPERATOR_PORT/health"

health="$(curl -s "$HEALTH_URL")"
check "cabinet reports connected" '{"ok":true,"cabinet_connected":true}' "$health"

route() {
  local source="$1"
  local body
  body=$(printf '{"type":"route","payload":{"source":%s}}' "$source")
  curl -s -X POST "$COMMANDS_URL" \
    -H "Authorization: Bearer $EDGE_OPERATOR_TOKEN" -H "Content-Type: application/json" \
    -d "$body" \
    | python3 -c "import json,sys; print(json.load(sys.stdin)['target'])"
}

check "source 1 (Windows) fans out to Pi+Nano" "ToPiAndNano" "$(route 1)"
check "source 2 (Nano) relays to Windows" "ToWindows" "$(route 2)"
check "source 3 (Pi) relays to Windows" "ToWindows" "$(route 3)"

ack_response=$(curl -s -X POST "$COMMANDS_URL" \
  -H "Authorization: Bearer $EDGE_OPERATOR_TOKEN" -H "Content-Type: application/json" \
  -d '{"type":"ping","payload":{}}')
ack_type=$(echo "$ack_response" | python3 -c "import json,sys; print(json.load(sys.stdin)['type'])")
check "unrelated command type gets a generic ack" "ack" "$ack_type"

bad_code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$COMMANDS_URL" \
  -H "Authorization: Bearer wrong-token" -d '{}')
check "wrong operator token is rejected" "401" "$bad_code"

if [ "$fail" -ne 0 ]; then
  echo "--- relay log ---"; cat "$RELAY_LOG"
  echo "--- client log ---"; cat "$CLIENT_LOG"
  exit 1
fi
echo "ALL PASS"
