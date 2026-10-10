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
  [ -n "$CLIENT_PID" ] && kill -CONT "$CLIENT_PID" 2>/dev/null || true # a SIGSTOPped client ignores SIGTERM
  [ -n "$RELAY_PID" ] && kill "$RELAY_PID" 2>/dev/null || true
  [ -n "$CLIENT_PID" ] && kill "$CLIENT_PID" 2>/dev/null || true
  wait 2>/dev/null || true
  rm -rf "$RELAY_LOG" "$CLIENT_LOG" "$WORKDIR"
}
trap cleanup EXIT

export EDGE_KEY_FILE="$WORKDIR/relay.key"
cd "$WORKDIR" # edge_known_servers.txt (TOFU) lands here, not in the repo
BIN="$OLDPWD/build"

EDGE_HANDSHAKE_DEADLINE_S=2 EDGE_HEARTBEAT_INTERVAL_S=1 EDGE_HEARTBEAT_DEAD_S=20 EDGE_PI_TOKEN="$EDGE_PI_TOKEN" "$BIN/edge_relay" > "$RELAY_LOG" 2>&1 &
RELAY_PID=$!
sleep 0.5
PIN="$(sed -n 's/.*clients pin this): //p' "$RELAY_LOG")"
[ ${#PIN} -eq 64 ] || { echo "FAIL: relay did not print a 64-hex fingerprint"; cat "$RELAY_LOG"; exit 1; }
export EDGE_SERVER_PIN="$PIN"

"$BIN/edge_client" 127.0.0.1 "$EDGE_CLIENT_PORT" "$EDGE_CLIENT_TOKEN" > "$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!
sleep 0.5

CLIENT_PID="$CLIENT_PID" TESTS_DIR="$OLDPWD/tests" REPO_DIR="$OLDPWD" BIN="$BIN" PIN="$PIN" EDGE_OPERATOR_PORT="$EDGE_OPERATOR_PORT" EDGE_OPERATOR_TOKEN="$EDGE_OPERATOR_TOKEN" \
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

# ---- serial / terminal capture (cards #492/#474): a pty stands in for the Feather's COM port ----
import pty, tty
master, slave = pty.openpty()
tty.setraw(master)
slave_name = os.ttyname(slave)
op.send({"id": "s0", "type": "serial_open", "payload": {"port": "/dev/does-not-exist"}})
bad_open = json.loads(op.recv())
check("serial_open on a missing port reports ok:false with a reason", True, bad_open.get("ok") is False and "error" in bad_open)
op.send({"id": "s1", "type": "serial_open", "payload": {"port": slave_name, "baud": 115200}})
opened = json.loads(op.recv())
check("serial_open on the pty succeeds", True, opened.get("ok") is True)

os.write(master, b'hello feather\r\nsay "quoted" and tab\there\npartial prompt> ')
time.sleep(0.6)
op.send({"type": "log_since", "since": 0, "channel": "serial"})
logs = json.loads(op.recv())
texts = [i["data"] for i in logs.get("items", [])]
check("log_since returns the Feather's lines in order (CRLF stripped)", "hello feather", texts[0] if texts else None)
check("quotes and tabs survive the JSON round trip (escaped on the wire)", 'say "quoted" and tab\there', texts[1] if len(texts) > 1 else None)
check("a partial line (a prompt with no newline) is flushed after a short idle", "partial prompt> ", texts[2] if len(texts) > 2 else None)

sub2 = Conn(os.environ["EDGE_OPERATOR_TOKEN"]); sub2.recv()
sub2.send({"type": "log_subscribe", "since": 999999}); sub2.recv()
os.write(master, b"live line\n")
liveline = json.loads(sub2.recv(5))
check("log_subscribe streams a new serial line live", "live line", liveline.get("data"))
check("live log carries a seq and channel", ("serial", True), (liveline.get("channel"), isinstance(liveline.get("seq"), int)))

op.send({"id": "s2", "type": "serial_write", "payload": {"data": "ping", "newline": 1}})
wr = json.loads(op.recv())
check("serial_write reports the bytes written", 5, wr.get("bytes"))
time.sleep(0.2)
os.set_blocking(master, False)
try:
    got = os.read(master, 100)
except BlockingIOError:
    got = b""
check("the bytes really arrive on the Feather side of the port", b"ping\n", got)
# ---- two hosts, one Feather (card #478): the Windows PC and the Android tablet may both be online, but the
# Feather's single USB link is on one of them; commands follow the link, or an explicit "host" ----
android = subprocess.Popen([BIN + "/edge_client", "127.0.0.1", os.environ["EDGE_CLIENT_PORT"], os.environ["EDGE_CLIENT_TOKEN"]],
                           env=dict(os.environ, EDGE_HOST="android"), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1.0)
op.send({"type": "hosts"})
hs = json.loads(op.recv())
check("hosts lists both connected hosts, only Windows holding the Feather", ([("windows", True), ("android", False)], "windows"),
      ([(i["host"], i["feather"]) for i in hs.get("items", [])], hs.get("feather_host")))
op.send({"id": "h1", "type": "serial_write", "payload": {"data": "x", "newline": 0}})
h1 = json.loads(op.recv())
check("a command with no host goes to the host that has the Feather", (True, 1), (h1.get("ok"), h1.get("bytes")))
op.send({"id": "h2", "type": "serial_write", "host": "android", "payload": {"data": "x", "newline": 0}})
h2 = json.loads(op.recv())
check("an explicit host=android reaches the tablet, which has no serial port open", (False, True), (h2.get("ok"), "no serial port" in h2.get("error", "")))
op.send({"id": "h3", "type": "serial_write", "host": "mars", "payload": {"data": "x"}})
check("an unknown host is refused with a reason", True, "not connected" in json.loads(op.recv()).get("error", ""))
am, asl = pty.openpty(); tty.setraw(am)
op.send({"id": "h4", "type": "serial_open", "host": "android", "payload": {"port": os.ttyname(asl), "baud": 115200}})
check("the tablet can open a port too", True, json.loads(op.recv()).get("ok") is True)
time.sleep(0.3)
op.send({"type": "hosts"})
hs2 = json.loads(op.recv())
check("both hosts claiming the Feather is reported as a conflict", "conflict", hs2.get("feather_host"))
op.send({"id": "h5", "type": "serial_write", "payload": {"data": "x"}})
check("an ambiguous command is refused, not guessed", True, "claim the Feather" in json.loads(op.recv()).get("error", ""))
op.send({"id": "h6", "type": "serial_close", "host": "android", "payload": {}})
json.loads(op.recv())
time.sleep(0.3)
op.send({"type": "hosts"})
check("after the tablet lets go, Windows holds the link again", "windows", json.loads(op.recv()).get("feather_host"))
android.kill(); android.wait()
time.sleep(0.5)
op.send({"type": "hosts"})
check("a disconnected host disappears from the list", ["windows"], [i["host"] for i in json.loads(op.recv()).get("items", [])])

op.send({"id": "s3", "type": "serial_close", "payload": {}})
check("serial_close acknowledged", True, json.loads(op.recv()).get("ok") is True)
sub2.close()

# ---- flash_hex (cards #477/#474): server-compiled hex flashed by the client over AVR109 ----
sys.path.insert(0, os.environ["TESTS_DIR"])
from avr109_sim import Sim
hexpath = os.environ["REPO_DIR"] + "/feather/pi_bridge/pi_bridge.hex"
hex_text = open(hexpath).read()
fm, fs = pty.openpty(); tty.setraw(fm)
sim = Sim(fm); sim.start()
fsub = Conn(os.environ["EDGE_OPERATOR_TOKEN"]); fsub.recv()
fsub.send({"type": "log_subscribe", "since": 999999}); fsub.recv()
op.send({"id": "f1", "type": "flash_hex", "payload": {"hex": hex_text, "bootloader_port": os.ttyname(fs)}})
fr = json.loads(op.recv(30))
check("flash_hex through the relay reports ok with a plausible image size", (True, True), (fr.get("ok"), 6000 <= fr.get("bytes", 0) <= 28672))
want = bytearray(b"\xff" * 28672)
for ln in hex_text.splitlines():
    b = bytes.fromhex(ln[1:]) if ln.startswith(":") else b""
    if b and b[3] == 0: want[(b[1] << 8) | b[2]:((b[1] << 8) | b[2]) + b[0]] = b[4:4 + b[0]]
top = max(((bytes.fromhex(l[1:])[1] << 8) | bytes.fromhex(l[1:])[2]) + bytes.fromhex(l[1:])[0] for l in hex_text.splitlines() if l.startswith(":") and bytes.fromhex(l[1:])[3] == 0)
time.sleep(0.2)
check("the simulated Feather's flash now holds exactly the server-compiled image", bytes(want[:top]), bytes(sim.flash[:top]))
progress = []
t_end = time.time() + 3
while time.time() < t_end:
    ln = fsub.recv(0.3)
    if not ln: break
    try: progress.append(json.loads(ln).get("data", ""))
    except Exception: pass
check("flash progress streamed as log channel flash (pages + verify + done)", True,
      any("writing page" in x for x in progress) and any("verifying" in x for x in progress) and any(x.startswith("done") for x in progress))
sim.stop = True
op.send({"id": "f2", "type": "flash_hex", "payload": {"hex": ":10000000" + "00" * 16 + "FF\n", "bootloader_port": "/dev/null"}})
bad = json.loads(op.recv(10))
check("flash_hex with a corrupt hex fails cleanly (ok:false + reason)", (False, True), (bad.get("ok"), "checksum" in bad.get("error", "")))
op.send({"id": "f3", "type": "flash_hex", "payload": {"hex": hex_text}})
nof = json.loads(op.recv(15))
check("flash_hex with no Feather attached explains what to do", (False, True), (nof.get("ok"), "no Feather" in nof.get("error", "")))
fsub.close()

# ---- editor_set / editor_get (card #475) + the whole Claude loop scripts/edge_flash.sh (card #477) ----
op.send({"id": "e1", "type": "editor_set", "payload": {}, "name": "blink.prn", "text": ";; via relay\n(defn next-led-state [(current : Bool)] : Bool\n  (not current))\n"})
es = json.loads(op.recv(5))
check("editor_set writes the editor's file next to the client", (True, "blink.prn"), (es.get("ok"), es.get("name")))
op.send({"id": "e2", "type": "editor_get", "payload": {}, "name": "blink.prn"})
eg = json.loads(op.recv(5))
check("editor_get reads it back verbatim (newlines + parens)", ";; via relay\n(defn next-led-state [(current : Bool)] : Bool\n  (not current))\n", eg.get("text"))
op.send({"id": "e3", "type": "editor_set", "payload": {}, "name": "../evil.prn", "text": "x"})
check("editor_set refuses path traversal", False, json.loads(op.recv(5)).get("ok"))
op.send({"id": "e4", "type": "editor_set", "payload": {}, "name": "x.exe", "text": "x"})
check("editor_set refuses a non-document extension", False, json.loads(op.recv(5)).get("ok"))
op.send({"id": "e5", "type": "editor_get", "payload": {}, "name": "missing.prn"})
check("editor_get on a missing file reports ok:false", False, json.loads(op.recv(5)).get("ok"))

fm2, fs2 = pty.openpty(); tty.setraw(fm2)
sim2 = Sim(fm2); sim2.start()
PARENA = os.environ.get("PARENA_ROOT") or (os.environ["REPO_DIR"] + "/../PARENA")
prn = PARENA + "/examples/avr/blink.prn"
avr = os.environ.get("AVR_TOOLCHAIN_ROOT", os.path.expanduser("~/.local/opt/avr-toolchain")) + "/usr/bin/avr-gcc"
if os.path.exists(prn) and os.path.exists(PARENA + "/parena") and os.path.exists(avr):
    env2 = dict(os.environ, PARENA_ROOT=PARENA, EDGE_RELAY_OPERATOR_PORT=PORT, EDGE_BOOTLOADER_PORT=os.ttyname(fs2), EDGE_CTL=BIN + "/edge_ctl")
    r = subprocess.run([os.environ["REPO_DIR"] + "/scripts/edge_flash.sh", prn], capture_output=True, text=True, timeout=120, env=env2)
    check("edge_flash.sh: editor sync + server compile + flash through the relay reports ok", True, '"flash_result"' in r.stdout and '"ok":true' in r.stdout)
    check("edge_flash.sh synced the program into the editor file", True, "editor_set -> " in r.stdout and '"ok":true' in r.stdout.split("editor_set -> ")[1])
    time.sleep(0.2)
    check("the flashed image is the server-compiled blink (reset vector table present)", True, bytes(sim2.flash[:4]) == bytes.fromhex("0C945600"))
else:
    print("SKIP: edge_flash.sh end-to-end needs a sibling ../PARENA with parena built AND avr-gcc (AVR_TOOLCHAIN_ROOT)")
sim2.stop = True

# ---- cabinet heartbeat: a live client stays registered while idle; a frozen one is reaped ----
import signal
def hosts():
    # fresh operator session each time: the long-lived `op` above is an `edge_ctl --follow 60` child
    # and has expired by now (this test sleeps past the heartbeat deadline twice)
    c = Conn(os.environ["EDGE_OPERATOR_TOKEN"])
    try:
        c.recv()  # hello_ok
        c.send({"id": "hh", "type": "hosts"})
        for _ in range(5):
            m = json.loads(c.recv(3) or "{}")
            if m.get("type") == "hosts": return m.get("items", [])
        return None
    finally:
        c.close()
time.sleep(22)  # > EDGE_HEARTBEAT_DEAD_S with no operator traffic: only heartbeat acks keep it alive
check("an idle but live cabinet stays registered (heartbeat acks)", ["windows"], [h["host"] for h in (hosts() or [])])
cpid = int(os.environ["CLIENT_PID"])
os.kill(cpid, signal.SIGSTOP)  # a PC that slept / a half-open socket: connected, never answers
time.sleep(23)
check("a frozen cabinet is dropped from hosts (no stale 'connected')", [], hosts())
os.kill(cpid, signal.SIGCONT)

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
