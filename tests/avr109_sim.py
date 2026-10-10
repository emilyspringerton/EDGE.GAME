"""tests/avr109_sim.py -- Caterina/AVR109 bootloader simulator on a pty (shared by test_avr109.py and
scripts/test_e2e.sh). Implements S V p b T P A B g L E; 128-byte pages, 28672-byte app flash."""
import os, threading, time

class Sim(threading.Thread):
    def __init__(self, master, corrupt=False, deaf=False):
        super().__init__(daemon=True)
        self.m = master; self.flash = bytearray(b"\xff" * 28672); self.addr = 0
        self.corrupt = corrupt; self.deaf = deaf; self.exited = False; self.stop = False
    def rd(self, n):
        buf = b""
        while len(buf) < n and not self.stop:
            try: buf += os.read(self.m, n - len(buf))
            except BlockingIOError: time.sleep(0.001)
            except OSError: return buf
        return buf
    def wr(self, b): os.write(self.m, b)
    def run(self):
        os.set_blocking(self.m, False)
        while not self.stop:
            c = self.rd(1)
            if not c or self.deaf: continue
            c = chr(c[0])
            if c == "S": self.wr(b"CATERIN")
            elif c == "V": self.wr(b"10")
            elif c == "p": self.wr(b"S")
            elif c == "b": self.wr(b"Y\x00\x80")
            elif c == "T": self.rd(1); self.wr(b"\r")
            elif c == "P": self.wr(b"\r")
            elif c == "A": h = self.rd(2); self.addr = ((h[0] << 8) | h[1]) * 2; self.wr(b"\r")
            elif c == "B":
                h = self.rd(3); n = (h[0] << 8) | h[1]; data = self.rd(n)
                self.flash[self.addr:self.addr + n] = data; self.wr(b"\r")
            elif c == "g":
                h = self.rd(3); n = (h[0] << 8) | h[1]
                out = bytearray(self.flash[self.addr:self.addr + n])
                if self.corrupt and self.addr == 128: out[5] ^= 0xFF
                self.wr(bytes(out))
            elif c == "L": self.wr(b"\r")
            elif c == "E": self.wr(b"\r"); self.exited = True

