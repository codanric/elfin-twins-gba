#!/usr/bin/env python3
"""
Elfin Twins workbench: runs the ROM on the C SPLB20 core (fast) with a small
scripting API. Used for reverse engineering, for the link-cable simulation
and to produce the screenshots in the manual.

    from elfin_sim import Pet
    p = Pet()
    p.run(3)                 # seconds of emulated time
    p.press("enter")
    print(p.ascii())
    p.image().save("x.png")
"""
import ctypes
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, HERE)

import elfin_lcd  # noqa: E402
from verify_core import build_lib, ROM_PATH, CLOCK, BUTTONS  # noqa: E402

FP = 256

_lib = None


def lib():
    global _lib
    if _lib is None:
        _lib = build_lib()
        _lib.h_state_size.restype = ctypes.c_int
        _lib.h_state_size.argtypes = []
        _lib.h_state_get.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        _lib.h_state_set.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    return _lib


class Pet:
    def __init__(self, rom_path=ROM_PATH):
        self.lib = lib()
        rom = open(rom_path, "rb").read()
        self._rom = rom
        self.h = self.lib.h_create(rom, len(rom), CLOCK, 0, 255)
        self._mem = (ctypes.c_uint8 * (0x40 + 0x80 + 0x800))()
        self._regs = (ctypes.c_int32 * 24)()
        self.cycles = 0          # emulated cycles since creation
        self.held = set()

    # ------------------------------------------------------------ running
    def run_cycles(self, n):
        n = int(n)
        while n > 0:
            chunk = min(n, 4_000_000)
            done = self.lib.h_run(self.h, chunk * FP) // FP
            n -= done
            self.cycles += done

    def run(self, seconds):
        self.run_cycles(seconds * CLOCK)

    def run_fast(self, seconds, warp=64, slice_s=0.01):
        """Advance pet time quickly: timers run `warp` times faster."""
        self.lib.h_set_warp(self.h, warp)
        self.run(seconds / warp)
        self.lib.h_set_warp(self.h, 1)

    # ------------------------------------------------------------ input
    def down(self, name):
        self.lib.h_port(self.h, BUTTONS[name], 0)
        self.held.add(name)

    def up(self, name):
        self.lib.h_port(self.h, BUTTONS[name], -1)
        self.held.discard(name)

    def press(self, name, hold=0.15, after=0.35):
        self.down(name)
        self.run(hold)
        self.up(name)
        self.run(after)

    def seq(self, *names, hold=0.15, after=0.35):
        for n in names:
            self.press(n, hold, after)

    # ------------------------------------------------------------ state
    def mem(self):
        self.lib.h_mem(self.h, self._mem)
        return bytes(self._mem)

    def lcd(self):
        return self.mem()[:0x40]

    def ram(self):
        """CPU RAM as a dict-like 256-byte view indexed by zero page address."""
        m = self.mem()
        return bytes(0x80) + m[0x40:0xC0]

    def dram(self):
        return self.mem()[0xC0:]

    def peek(self, addr):
        return self.lib.h_read(self.h, addr)

    def poke(self, addr, value):
        self.lib.h_poke(self.h, addr, value)

    def regs(self):
        self.lib.h_regs(self.h, self._regs)
        return list(self._regs)

    def pc(self):
        return self.regs()[0]

    def snapshot(self):
        n = self.lib.h_state_size()
        buf = (ctypes.c_uint8 * n)()
        self.lib.h_state_get(self.h, buf)
        return (bytes(buf), self.cycles)

    def restore(self, snap):
        data, cycles = snap
        buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
        self.lib.h_state_set(self.h, buf)
        self.cycles = cycles

    # ------------------------------------------------------------ output
    def ascii(self):
        return elfin_lcd.ascii_frame(self.lcd())

    def image(self, width=456, full=False):
        return elfin_lcd.render(self.lcd(), width=width, full=full)

    def seconds(self):
        return self.cycles / CLOCK


if __name__ == "__main__":
    p = Pet()
    p.run(float(sys.argv[1]) if len(sys.argv) > 1 else 3)
    print(p.ascii())
