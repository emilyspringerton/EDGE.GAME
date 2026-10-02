#!/usr/bin/env python3
"""tests/test_avr109.py -- the client's AVR109/Caterina flasher against a protocol SIMULATOR on a pty.
The simulator implements the commands avrdude's avr109 programmer uses on a 32u4 (S p b T P A B g L E)
with a 128-byte page, 28672-byte app flash. This proves our byte-level protocol handling, the Intel HEX
parser, and verify-failure detection -- it does NOT prove a real Caterina bootloader accepts it
(never run on a real Feather)."""
import os, pty, subprocess, sys, threading, tty, time, tempfile

CLI, HEX = sys.argv[1], sys.argv[2]
fails = [0]
def check(desc, want, got):
    if want != got:
        print(f"FAIL: {desc} -- want {want!r}, got {got!r}"); fails[0] = 1
    else:
        print(f"PASS: {desc}")

def ihex(path):
    mem = bytearray(b"\xff" * 28672); top = 0
    for line in open(path):
        line = line.strip()
        if not line.startswith(":"): continue
        b = bytes.fromhex(line[1:]); n, addr, typ = b[0], (b[1] << 8) | b[2], b[3]
        if typ == 0: mem[addr:addr + n] = b[4:4 + n]; top = max(top, addr + n)
    return mem, top

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from avr109_sim import Sim

def run(corrupt=False, deaf=False, hexpath=HEX):
    master, slave = pty.openpty(); tty.setraw(master)
    sim = Sim(master, corrupt, deaf); sim.start()
    p = subprocess.run([CLI, hexpath, os.ttyname(slave)], capture_output=True, timeout=30)
    time.sleep(0.1); sim.stop = True
    return p, sim

want, top = ihex(HEX)
p, sim = run()
check("flash of the real pi_bridge.hex succeeds (exit 0)", 0, p.returncode)
check("flashed memory is byte-identical to the hex image", bytes(want[:top]), bytes(sim.flash[:top]))
check("bootloader was told to exit afterwards (E acknowledged)", True, sim.exited)
check("progress reports page writes and verify", True, b"writing page 1/" in p.stderr and b"verifying" in p.stderr)
check("pages beyond the image stay erased (0xFF)", b"\xff" * 64, bytes(sim.flash[top + 200:top + 264]))

p, sim = run(corrupt=True)
check("a corrupted read-back is caught (VERIFY FAILED, exit 2)", (2, True), (p.returncode, b"VERIFY FAILED" in p.stderr))
check("on verify failure the bootloader is NOT told to exit", False, sim.exited)

p, sim = run(deaf=True)
check("a port with no Caterina bootloader fails with a helpful message", (2, True), (p.returncode, b"double-tap RESET" in p.stderr))

bad = tempfile.NamedTemporaryFile("w", suffix=".hex", delete=False)
bad.write(":10000000" + "00" * 16 + "FF\n:00000001FF\n"); bad.close()
p, _ = run(hexpath=bad.name)
check("a bad Intel HEX checksum is rejected before touching the port (exit 1)", (1, True), (p.returncode, b"checksum" in p.stderr))
big = tempfile.NamedTemporaryFile("w", suffix=".hex", delete=False)
big.write(":02FFFE00AABB" + "%02X" % ((-(2 + 0xFF + 0xFE + 0 + 0xAA + 0xBB)) & 0xFF) + "\n:00000001FF\n"); big.close()
p, _ = run(hexpath=big.name)
check("an image past the app-flash limit is rejected (exit 1)", (1, True), (p.returncode, b"past" in p.stderr))
os.unlink(bad.name); os.unlink(big.name)
sys.exit(1 if fails[0] else 0)
