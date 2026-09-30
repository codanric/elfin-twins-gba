#!/usr/bin/env python3
"""
Two Elfin Twins connected by a simulated link cable.

The wire is open-drain with pull-ups (like the toy and like the GBA port on
the SD line): it is low when either unit drives PA5 low. Each unit sees the
wire on PA5 as an input. Both run in lock-step slices of `slice` cycles
(default 100 = 0.18 ms, finer than the GBA port's 0.49 ms).
"""
import ctypes, sys
import elfin_sim
from explore import load, state, icons

PA_LINK = 0x20

class Link:
    def __init__(self, a, b, slice_cycles=100, skew=1.0, latch=False):
        self.a, self.b = a, b
        self.slice = slice_cycles
        self.skew = skew           # speed ratio of b relative to a (clock tolerance test)
        self.low_seen = [False, False]
        self.edges = 0
        a.lib.h_drive_low.argtypes = [ctypes.c_void_p]
        self.line = 1
        # latch=True models a pending port-change interrupt: a falling edge of
        # the wire is remembered and delivered once the key interrupt is
        # enabled, even if the unit could not see it on its pin at that time.
        self.latch = latch
        self.pending = [False, False]
        a.lib.h_key_irq.argtypes = [ctypes.c_void_p]
        a.lib.h_int_cfg.argtypes = [ctypes.c_void_p]

    def step(self, secs):
        n = int(secs * 560000 / self.slice)
        for _ in range(n):
            da = self.a.lib.h_drive_low(self.a.h) & PA_LINK
            db = self.b.lib.h_drive_low(self.b.h) & PA_LINK
            line = 0 if (da or db) else 1
            if line != self.line:
                self.edges += 1
                self.line = line
                if line == 0 and self.latch:
                    self.pending = [True, True]
            if self.latch:
                for i, p in enumerate((self.a, self.b)):
                    if self.pending[i] and (p.lib.h_int_cfg(p.h) & 0x88) == 0x88:
                        self.pending[i] = False
                        if self.line == 0:
                            p.lib.h_key_irq(p.h)
            # each unit sees the other's drive (its own drive is already on its pin)
            for p, other, st in ((self.a, db, 0), (self.b, da, 1)):
                low = bool(other)
                if low != self.low_seen[st]:
                    self.low_seen[st] = low
                    p.lib.h_port(p.h, PA_LINK, 0 if low else -1)
            self.a.run_cycles(self.slice)
            self.b.run_cycles(self.slice * self.skew)

    def press(self, pet, key, hold=0.2, after=0.4):
        pet.down(key); self.step(hold); pet.up(key); self.step(after)

    def ready(self, pet, timeout=6):
        t = 0
        while t < timeout and ((pet.peek(0xB9) & 0x04) or not (pet.peek(0xB7) & 0x04)):
            self.step(0.1); t += 0.1

    def select(self, pet, bit):
        for _ in range(12):
            if pet.peek(0xBC) == bit:
                return
            self.press(pet, 'right', hold=0.15, after=0.35)

class LinkC(Link):
    """Same wiring, but through the C bridge used by the GBA port."""
    def __init__(self, a, b, slice_cycles=273, skew=1.0):
        Link.__init__(self, a, b, slice_cycles, skew, latch=False)
        for f in ('h_link_reset', 'h_link_after'):
            getattr(a.lib, f).argtypes = [ctypes.c_void_p]
        a.lib.h_link_before.argtypes = [ctypes.c_void_p, ctypes.c_int]
        a.lib.h_link_reset(a.h); a.lib.h_link_reset(b.h)
        self.wire_low = 0

    def step(self, secs):
        n = int(secs * 560000 / self.slice)
        L = self.a.lib
        for _ in range(n):
            L.h_link_before(self.a.h, self.wire_low)
            L.h_link_before(self.b.h, self.wire_low)
            self.a.run_cycles(self.slice)
            self.b.run_cycles(self.slice * self.skew)
            da = L.h_link_after(self.a.h)
            db = L.h_link_after(self.b.h)
            w = 1 if (da or db) else 0
            if w != self.wire_low:
                self.edges += 1
            self.wire_low = w

def show(tag, a, b):
    print("%-12s A: %s | %s" % (tag, state(a), icons(a)))
    print("%-12s B: %s | %s" % ("", state(b), icons(b)))

def connect(latch=True, phase=0.37, skew=1.0):
    a, b = load(), load()
    b.run(phase)
    L = Link(a, b, latch=latch, skew=skew)
    L.select(a, 0x10); L.ready(a)
    L.press(a, 'enter')
    return a, b, L

if __name__ == "__main__":
    a, b, L = connect(latch='--latch' in sys.argv)
    for i in range(12):
        L.step(1)
        show('t+%ds' % (i + 1), a, b)
    print('wire edges so far', L.edges)
