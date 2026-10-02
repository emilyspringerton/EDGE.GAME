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
# Secure transport (card #491): every socket is ML-KEM-768 + LZ4 + XChaCha20-Poly1305; the test
# drives the relay only through build/edge_ctl / build/edge_client (the real encrypted clients), and
# checks plaintext, bad pins and wrong tokens are all refused.
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

WORKDIR="$(mktemp -d)"
RELAY_LOG="$(mktemp)"
CLIENT_LOG="$(mktemp)"
RELAY_PID=""
CLIENT_PID=""

cleanup() {
  [ -n "$RELAY_PID" ] && kill "$RELAY_PID" 2>/dev/null || true
  [ -n "$CLIENT_PID" ] && kill "$CLIENT_PID" 2>/dev/null || true
  wait 2>/dev/null || true
  rm -rf "$RELAY_LOG" "$CLIENT_LOG" "$WORKDIR"
}
trap cleanup EXIT

export EDGE_KEY_FILE="$WORKDIR/relay.key"
cd "$WORKDIR" # edge_known_servers.txt (TOFU) lands here, not in the repo
BIN="$OLDPWD/build"

EDGE_HANDSHAKE_DEADLINE_S=2 EDGE_PI_TOKEN="$EDGE_PI_TOKEN" "$BIN/edge_relay" > "$RELAY_LOG" 2>&1 &
RELAY_PID=$!
sleep 0.5
PIN="$(sed -n 's/.*clients pin this): //p' "$RELAY_LOG")"
[ ${#PIN} -eq 64 ] || { echo "FAIL: relay did not print a 64-hex fingerprint"; cat "$RELAY_LOG"; exit 1; }
export EDGE_SERVER_PIN="$PIN"

"$BIN/edge_client" 127.0.0.1 "$EDGE_CLIENT_PORT" "$EDGE_CLIENT_TOKEN" > "$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!
sleep 0.5

BIN="$BIN" PIN="$PIN" EDGE_OPERATOR_PORT="$EDGE_OPERATOR_PORT" EDGE_OPERATOR_TOKEN="$EDGE_OPERATOR_TOKEN" \
EDGE_PI_TOKEN="$EDGE_PI_TOKEN" EDGE_CLIENT_PORT="$EDGE_CLIENT_PORT" python3 - <<'PYEOF'
import json, os, select, socket, subprocess, sys, time

fail = [0]
BIN = os.environ["BIN"]
PORT = os.environ["EDGE_OPERATOR_PORT"]

def check(desc, want, got):
    if got != want:
        print(f"FAIL: {desc} -- want {want!r}, got {got!r}")
        fail[0] = 1
    else:
        print(f"PASS: {desc}")

class Conn:
    """One live encrypted session: an edge_ctl --stdin child (it does the ML-KEM handshake)."""
    def __init__(self, token, extra=()):
        self.p = subprocess.Popen([BIN + "/edge_ctl", "127.0.0.1", PORT, token, "--follow", "60", "--stdin", *extra],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.buf = b""
    def send(self, obj):
        self.p.stdin.write((json.dumps(obj, separators=(",", ":")) + "\n").encode()); self.p.stdin.flush()
    def recv(self, timeout=3):
        end = time.time() + timeout
        while b"\n" not in self.buf:
            left = end - time.time()
            if left <= 0: return ""
            r, _, _ = select.select([self.p.stdout], [], [], left)
            if not r: return ""
            chunk = os.read(self.p.stdout.fileno(), 65536)
            if not chunk: return ""
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode()
    def close(self):
        self.p.kill(); self.p.wait()

op = Conn(os.environ["EDGE_OPERATOR_TOKEN"])
hello = json.loads(op.recv())
check("operator hello_ok (over the encrypted channel)", "operator", hello.get("role"))

def route(source, cid):
    op.send({"id": cid, "type": "route", "payload": {"source": source}})
    return json.loads(op.recv()).get("target")

check("source 1 (Windows) fans out to Pi+Nano", "ToPiAndNano", route(1, "c1"))
check("source 2 (Nano) relays to Windows", "ToWindows", route(2, "c2"))
check("source 3 (Pi) relays to Windows", "ToWindows", route(3, "c3"))

op.send({"id": "c4", "type": "ping", "payload": {}})
check("unrelated command type gets a generic ack", "ack", json.loads(op.recv()).get("type"))

# usb_probe round-trips relay -> cabinet client -> relay (no USB serial in this sandbox: devices []).
op.send({"id": "c5", "type": "usb_probe", "payload": {}})
pr = json.loads(op.recv())
check("usb_probe round-trips with a result type", "usb_probe_result", pr.get("type"))
check("usb_probe result carries a devices list + feather field", True, isinstance(pr.get("devices"), list) and "feather" in pr)

# ---- security checks ----
# Wrong operator token: edge_ctl exits 3 ("hello refused").
bad = subprocess.run([BIN + "/edge_ctl", "127.0.0.1", PORT, "wrong-token"], capture_output=True, timeout=10)
check("wrong operator token is refused (exit 3)", 3, bad.returncode)

# Wrong pin: refused at the handshake, before any secret is sent (exit 2).
badpin = subprocess.run([BIN + "/edge_ctl", "127.0.0.1", PORT, os.environ["EDGE_OPERATOR_TOKEN"], "--pin", "00" * 32],
                        capture_output=True, timeout=10)
check("wrong server pin refused at handshake (exit 2)", 2, badpin.returncode)

# Plaintext client: sends NDJSON hello in the clear; the relay must not answer hello_ok.
raw = socket.create_connection(("127.0.0.1", int(PORT)), timeout=3)
drained = b""
while len(drained) < 4 + 1187:  # drain the server's hello (length prefix + magic + ek)
    drained += raw.recv(4096)
raw.sendall(json.dumps({"type": "hello", "token": os.environ["EDGE_OPERATOR_TOKEN"]}).encode() + b"\n")
raw.settimeout(1.5)
try:
    got = raw.recv(4096)
except socket.timeout:
    got = b""
check("plaintext hello is never answered with hello_ok", False, b"hello_ok" in got)
raw.close()

# What the server sends first is magic + ML-KEM ek only: no plaintext protocol text anywhere.
raw = socket.create_connection(("127.0.0.1", int(PORT)), timeout=3)
first = b""
while len(first) < 4 + 1187:
    c = raw.recv(4096)
    if not c: break
    first += c
check("server speaks first with the 1187-byte ES1+ek hello", 4 + 1187, len(first))
check("server hello starts with the ES1 magic", b"ES1", first[4:7])
raw.close()

# Slow-loris guard: a socket that connects and then says nothing is dropped after the (test: 2s) deadline.
idle = socket.create_connection(("127.0.0.1", int(PORT)), timeout=3)
drained = b""
while len(drained) < 4 + 1187:
    drained += idle.recv(4096)
idle.settimeout(6)
try:
    closed = idle.recv(1) == b""
except socket.timeout:
    closed = False
check("idle half-open connection is reaped after the handshake deadline", True, closed)
idle.close()

# TOFU: with no pin in the env, first contact records the fingerprint, and a second contact matches it.
env = {k: v for k, v in os.environ.items() if k != "EDGE_SERVER_PIN"}
t1 = subprocess.run([BIN + "/edge_ctl", "127.0.0.1", PORT, os.environ["EDGE_OPERATOR_TOKEN"], "--wait", "0.2"],
                    capture_output=True, timeout=10, env=env)
check("TOFU first contact succeeds and announces itself", True, t1.returncode == 0 and b"FIRST CONTACT" in t1.stderr)
t2 = subprocess.run([BIN + "/edge_ctl", "127.0.0.1", PORT, os.environ["EDGE_OPERATOR_TOKEN"], "--wait", "0.2"],
                    capture_output=True, timeout=10, env=env)
check("TOFU second contact matches silently", True, t2.returncode == 0 and b"FIRST CONTACT" not in t2.stderr)
open("edge_known_servers.txt", "w").write("127.0.0.1:%s %s\n" % (PORT, "11" * 32))
t3 = subprocess.run([BIN + "/edge_ctl", "127.0.0.1", PORT, os.environ["EDGE_OPERATOR_TOKEN"], "--wait", "0.2"],
                    capture_output=True, timeout=10, env=env)
check("TOFU mismatch (swapped key) is refused", 2, t3.returncode)

# ---- REFLUX events channel (a Pi pushing "boot", buffered + live) ----
device = Conn(os.environ["EDGE_PI_TOKEN"])
check("device hello_ok", "device", json.loads(device.recv()).get("role"))
device.send({"type": "event", "event": "boot", "pi_id": 1})
time.sleep(0.3)

op.send({"type": "events_since", "since": 0})
items = json.loads(op.recv()).get("items", [])
check("events_since sees the buffered boot event", 1, len(items))
if items:
    check("boot event action_type is REFLUX_ACTION_PI_BOOTED", 1, items[0].get("action_type"))
    check("boot event payload a is the real pi_id", 1, items[0].get("a"))

sub = Conn(os.environ["EDGE_OPERATOR_TOKEN"])
sub.recv()
sub.send({"type": "events_subscribe", "since": 999999})
sub.recv()
device.send({"type": "event", "event": "boot", "pi_id": 2})
live = json.loads(sub.recv())
check("live subscriber receives the second boot event without polling", "event", live.get("type"))
check("live push carries the real pi_id", 2, live.get("a"))

for c in (op, device, sub): c.close()
sys.exit(1 if fail[0] else 0)
PYEOF
status=$?

if [ "$status" -ne 0 ]; then
  echo "--- relay log ---"; cat "$RELAY_LOG"
  echo "--- client log ---"; cat "$CLIENT_LOG"
  exit 1
fi
echo "ALL PASS"
