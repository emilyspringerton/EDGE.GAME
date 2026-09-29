#!/usr/bin/env bash
# Real, local, no-hardware end-to-end proof for EDGE.GAME (see NORTHSTAR.md's own phased plan).
# Rewritten 2026-09-29 (founder real-time: "write it in PARENA in what world are we using node for
# any part of this stack?" -> "all of the node stuff gets ported to PARENA" -> "make sure we are
# using REFLUX for the pub sub"): server/relay.js (Node/HTTP) is gone. The relay is now
# build/edge_relay, a real native binary linking PARENA-compiled LLVM object code
# (stdlib/reflux/reflux.prn, stdlib/net/tcp_llvm.prn -- see server/relay_main.c's own header
# comment) with a thin hand-written C host for the select()/NDJSON plumbing PARENA's v0 LLVM
# target can't express. Wire protocol is plain TCP + NDJSON everywhere now (both the cabinet port
# AND the operator port -- the old HTTP operator API is gone, since PARENA has no HTTP-server
# stdlib and unifying transports is simpler than inventing one).
#
# Proves: cabinet connect+route (unchanged Phase-1 claim, PARENA/stdlib/edge_game/
# traffic_router.prn's own real routing decision), operator command forwarding, wrong-token
# rejection, AND the new REFLUX events channel (a device/Pi pushing a "boot" event, buffered
# events_since, and live events_subscribe streaming) end to end. Does NOT touch any real
# hardware -- that's Phase 5+, genuinely blocked on the founder's own machine.
set -euo pipefail

: "${EDGE_CLIENT_TOKEN:?}"
: "${EDGE_OPERATOR_TOKEN:?}"
: "${EDGE_CLIENT_PORT:?}"
: "${EDGE_OPERATOR_PORT:?}"
: "${EDGE_PI_TOKEN:=e2e-pi-secret}"

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

EDGE_PI_TOKEN="$EDGE_PI_TOKEN" ./build/edge_relay > "$RELAY_LOG" 2>&1 &
RELAY_PID=$!
sleep 0.3

./build/edge_client 127.0.0.1 "$EDGE_CLIENT_PORT" "$EDGE_CLIENT_TOKEN" > "$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!
sleep 0.3

EDGE_OPERATOR_PORT="$EDGE_OPERATOR_PORT" EDGE_OPERATOR_TOKEN="$EDGE_OPERATOR_TOKEN" \
EDGE_PI_TOKEN="$EDGE_PI_TOKEN" python3 - <<'PYEOF'
import json, os, socket, sys, time

fail = [0]

def check(desc, want, got):
    if got != want:
        print(f"FAIL: {desc} -- want {want!r}, got {got!r}")
        fail[0] = 1
    else:
        print(f"PASS: {desc}")

def connect():
    return socket.create_connection(("127.0.0.1", int(os.environ["EDGE_OPERATOR_PORT"])), timeout=3)

def send(s, obj):
    s.sendall((json.dumps(obj, separators=(",", ":")) + "\n").encode())

def recv_line(s, timeout=3):
    s.settimeout(timeout)
    buf = b""
    try:
        while b"\n" not in buf:
            chunk = s.recv(4096)
            if not chunk:
                break
            buf += chunk
    except socket.timeout:
        pass
    return buf.decode().strip()

op = connect()
send(op, {"type": "hello", "token": os.environ["EDGE_OPERATOR_TOKEN"]})
hello = json.loads(recv_line(op))
check("operator hello_ok", "operator", hello.get("role"))

def route(source, cid):
    send(op, {"id": cid, "type": "route", "payload": {"source": source}})
    resp = json.loads(recv_line(op))
    return resp.get("target")

check("source 1 (Windows) fans out to Pi+Nano", "ToPiAndNano", route(1, "c1"))
check("source 2 (Nano) relays to Windows", "ToWindows", route(2, "c2"))
check("source 3 (Pi) relays to Windows", "ToWindows", route(3, "c3"))

send(op, {"id": "c4", "type": "ping", "payload": {}})
ack = json.loads(recv_line(op))
check("unrelated command type gets a generic ack", "ack", ack.get("type"))

# Wrong operator token: the connection should be closed (no hello_ok), not silently accepted.
bad = connect()
send(bad, {"type": "hello", "token": "wrong-token"})
bad_resp = recv_line(bad, timeout=2)
check("wrong operator token gets no hello_ok (connection closed)", "", bad_resp)

# REFLUX events channel: a device (Pi) pushes a boot event; buffered events_since sees it even
# though nobody was subscribed when it fired; a live subscriber sees a SECOND boot event pushed
# without polling.
device = connect()
send(device, {"type": "hello", "token": os.environ["EDGE_PI_TOKEN"]})
dev_hello = json.loads(recv_line(device))
check("device hello_ok", "device", dev_hello.get("role"))

send(device, {"type": "event", "event": "boot", "pi_id": 1})
time.sleep(0.2)

send(op, {"type": "events_since", "since": 0})
ev = json.loads(recv_line(op))
items = ev.get("items", [])
check("events_since sees the buffered boot event", 1, len(items))
if items:
    check("boot event action_type is REFLUX_ACTION_PI_BOOTED", 1, items[0].get("action_type"))
    check("boot event payload a is the real pi_id", 1, items[0].get("a"))

sub = connect()
send(sub, {"type": "hello", "token": os.environ["EDGE_OPERATOR_TOKEN"]})
recv_line(sub)
send(sub, {"type": "events_subscribe", "since": 999999})
recv_line(sub)  # the immediate catch-up flush (empty, since=999999 skips everything retained)
send(device, {"type": "event", "event": "boot", "pi_id": 2})
live = json.loads(recv_line(sub))
check("live subscriber receives the second boot event without polling", "event", live.get("type"))
check("live push carries the real pi_id", 2, live.get("a"))

sys.exit(1 if fail[0] else 0)
PYEOF
status=$?

if [ "$status" -ne 0 ]; then
  echo "--- relay log ---"; cat "$RELAY_LOG"
  echo "--- client log ---"; cat "$CLIENT_LOG"
  exit 1
fi
echo "ALL PASS"
