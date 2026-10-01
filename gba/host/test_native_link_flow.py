#!/usr/bin/env python3
"""End-to-end ROM test for the logical link mapping used by the GBA port.

Two real Elfin ROM instances run on the C SPLB20 core. The bridge carries only
DATA edge-count messages plus the BA91 WAKE event, exactly the abstraction used
above native GBA multiplayer SIO. This exercises:
  hello -> responder wake -> ready -> game selection -> acknowledgement.
"""
import ctypes
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from explore import load

CLOCK = 560000
FP = 256
SLICE = 273  # same ~1/2048 s emulation slice as the GBA front end


class NativeLink:
    def __init__(self, a, b):
        self.a, self.b = a, b
        L = a.lib
        L.h_native_enable.argtypes = [ctypes.c_void_p]
        L.h_native_take_tx.argtypes = [ctypes.c_void_p]
        L.h_native_take_tx.restype = ctypes.c_int
        L.h_native_feed.argtypes = [ctypes.c_void_p, ctypes.c_int]
        L.h_native_feed.restype = ctypes.c_int
        L.h_native_run.argtypes = [ctypes.c_void_p, ctypes.c_int32]
        L.h_native_run.restype = ctypes.c_int32
        L.h_native_enable(a.h)
        L.h_native_enable(b.h)

    def pump(self, src, dst):
        while True:
            event = src.lib.h_native_take_tx(src.h)
            if event < 0:
                return
            if not dst.lib.h_native_feed(dst.h, event):
                raise RuntimeError("native host receive queue overflow")

    def run_one(self, p, cycles):
        done_fp = p.lib.h_native_run(p.h, int(cycles * FP))
        p.cycles += done_fp // FP

    def step(self, secs):
        n = int(secs * CLOCK / SLICE)
        for _ in range(n):
            self.pump(self.b, self.a)
            self.run_one(self.a, SLICE)
            self.pump(self.a, self.b)
            self.run_one(self.b, SLICE)
            self.pump(self.b, self.a)

    def press(self, p, key, hold=0.2, after=0.4):
        p.down(key)
        self.step(hold)
        p.up(key)
        self.step(after)

    def ready(self, p, timeout=6):
        t = 0.0
        while t < timeout and ((p.peek(0xB9) & 0x04) or not (p.peek(0xB7) & 0x04)):
            self.step(0.1)
            t += 0.1

    def select_link(self, p):
        for _ in range(12):
            if p.peek(0xBC) == 0x10:
                return
            self.press(p, "right", hold=0.15, after=0.35)
        raise RuntimeError("could not select link icon")


def main():
    a, b = load(), load()
    # A small phase offset makes the two emulated units less artificially
    # lock-stepped before the logical cable is attached.
    b.run(0.37)
    link = NativeLink(a, b)

    link.select_link(a)
    link.ready(a)
    link.press(a, "enter")

    for _ in range(24):
        link.step(0.25)
        if a.peek(0xB5) == 2 and b.peek(0xB5) == 5:
            break
    else:
        print("FAIL handshake: A.B5=%02X B.B5=%02X A.BD=%02X B.BD=%02X" %
              (a.peek(0xB5), b.peek(0xB5), a.peek(0xBD), b.peek(0xBD)))
        return 1

    print("PASS handshake: caller state 2, responder state 5")

    # Let the caller's link UI finish its post-handshake redraw before pressing
    # ENTER, exactly as a human must wait for input to become enabled.
    link.ready(a)

    # ENTER on the default first activity goes through the inline BDB4
    # send-then-receive routine. Both sides should enter mode 0x6x.
    link.press(a, "enter")
    for _ in range(32):
        link.step(0.25)
        if (a.peek(0xBD) & 0xF0) == 0x60 and (b.peek(0xBD) & 0xF0) == 0x60:
            print("PASS game selection: both units entered mode 0x6x")
            return 0

    print("FAIL game selection: A.B5=%02X B.B5=%02X A.BD=%02X B.BD=%02X" %
          (a.peek(0xB5), b.peek(0xB5), a.peek(0xBD), b.peek(0xBD)))
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
