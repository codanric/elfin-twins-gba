#!/usr/bin/env python3
"""
Lock-step verification of the C SPLB20 core against BrickEmuPy's Python core.

Both cores run the Elfin Twins ROM side by side. After every instruction the
CPU registers are compared; every N instructions the full RAM, LCD RAM,
SFRs and timer counters are compared as well. A scripted sequence of button
presses (the same one for both cores) exercises menus, feeding, games etc.

Usage: python3 gba/host/verify_core.py [instructions] [seed]
"""
import ctypes
import importlib
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, ROOT)

ROM_PATH = os.path.join(ROOT, "assets", "ElfinTwins.bin")
CLOCK = 560000

# Button -> (mask, level) from assets/ElfinTwins.brick
BUTTONS = {
    "esc": 1, "left": 2, "right": 4, "clock": 8, "enter": 16,
    "up": 6, "down": 24,
}


def build_lib():
    out = os.path.join(HERE, "build", "libsplb20.so")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    srcs = [os.path.join(HERE, "pyharness.c"), os.path.join(HERE, "..", "core", "splb20.c"),
            os.path.join(HERE, "..", "core", "elfin_catchup.c"),
            os.path.join(HERE, "..", "core", "elfin_link.c")]
    deps = srcs + [os.path.join(HERE, "..", "core", n) for n in ("splb20.h", "elfin_catchup.h", "elfin_link.h")]
    if not os.path.exists(out) or any(os.path.getmtime(s) > os.path.getmtime(out) for s in deps):
        subprocess.check_call(["gcc", "-O2", "-shared", "-fPIC", "-Wall", "-o", out] + srcs)
    lib = ctypes.CDLL(out)
    lib.h_create.restype = ctypes.c_void_p
    lib.h_create.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_int, ctypes.c_int]
    for name in ("h_step", "h_run", "h_port", "h_regs", "h_mem", "h_illegal", "h_read",
                 "h_poke", "h_set_warp", "h_instr", "h_reset", "h_sound_freq", "h_destroy"):
        getattr(lib, name).argtypes = [ctypes.c_void_p]
    lib.h_run.argtypes = [ctypes.c_void_p, ctypes.c_int32]
    lib.h_port.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    lib.h_regs.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int32)]
    lib.h_mem.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint8)]
    lib.h_read.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.h_poke.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    lib.h_set_warp.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.h_instr.restype = ctypes.c_uint32
    lib.h_sound_freq.restype = ctypes.c_uint32
    return lib


class CCore:
    def __init__(self, lib, rom):
        self.lib = lib
        self.h = lib.h_create(rom, len(rom), CLOCK, 0, 255)
        self._regs = (ctypes.c_int32 * 24)()
        self._mem = (ctypes.c_uint8 * (0x40 + 0x80 + 0x800))()

    def step(self):
        return self.lib.h_step(self.h)

    def port(self, mask, level):
        self.lib.h_port(self.h, mask, level)

    def regs(self):
        self.lib.h_regs(self.h, self._regs)
        return list(self._regs)

    def mem(self):
        self.lib.h_mem(self.h, self._mem)
        return bytes(self._mem)


class _IC:
    def register_port_device(self, dev):
        pass

    def emit_audio(self, channel, data):
        pass


class PyCore:
    def __init__(self):
        mod = importlib.import_module("cores.SPLB20")
        mask = {"rom_path": ROM_PATH, "non_crystal_mode": 0, "port_pullup": {"PA": 255}}
        self.cpu = mod.SPLB20(mask, CLOCK, _IC())

    def step(self):
        return self.cpu.clock()

    def port(self, mask, level):
        self.cpu.port_handler("PA", mask, level)

    def regs(self):
        c = self.cpu
        return [c._PC & 0xFFFF, c._A, c._X, c._Y, c._SP, c._ps(), c._PDIR["PA"], c._PCFG["PA"],
                c._PLATCH["PA"], c._INT_CFG, c._SYS_CTRL, c._IREQ, c._TC, c._TC_PRESET,
                c._PRESCALAR, int(bool(c._ROSC_ENBL)), int(bool(c._CPU_ENBL)), c._port_read("PA"),
                int(round(c._timer_counter * 256)), int(round(c._T2HZ_counter * 256)),
                int(round(c._T128HZ_counter * 256)), c._LCD_CFG, c._LCD_BIAS,
                int(bool(c._sound._enable))]

    def mem(self):
        c = self.cpu
        return bytes(c._LCD_RAM) + bytes(c._RAM) + bytes(c._DATA_RAM)


REG_NAMES = ["pc", "a", "x", "y", "sp", "ps", "pdir", "pcfg", "platch", "int_cfg", "sys_ctrl",
             "ireq", "tc", "tc_preset", "prescalar", "rosc", "cpu", "port", "timer_ctr",
             "t2hz_ctr", "t128hz_ctr", "lcd_cfg", "lcd_bias", "snd_en"]


def make_script(total_cycles, seed):
    """Random-but-plausible button presses: (cycle, mask, level)."""
    rng = random.Random(seed)
    events = []
    t = CLOCK * 3  # let it boot first
    names = list(BUTTONS)
    while t < total_cycles:
        name = rng.choice(names)
        hold = rng.randint(CLOCK // 20, CLOCK // 4)
        events.append((t, BUTTONS[name], 0))
        events.append((t + hold, BUTTONS[name], -1))
        t += hold + rng.randint(CLOCK // 8, CLOCK * 2)
    return events


def main():
    n_instr = int(sys.argv[1]) if len(sys.argv) > 1 else 2_000_000
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    lib = build_lib()
    rom = open(ROM_PATH, "rb").read()
    c, p = CCore(lib, rom), PyCore()

    # Rough cycle budget: ~3.3 cycles/instruction
    script = make_script(int(n_instr * 3.5), seed)
    si = 0
    cycles = 0.0
    for i in range(n_instr):
        while si < len(script) and cycles >= script[si][0]:
            _, mask, level = script[si]
            c.port(mask, level)
            p.port(mask, level)
            si += 1
        rc_c = c.step()
        rc_p = p.step()
        if rc_c != int(round(rc_p * 256)):
            print("cycle count mismatch at instr %d: C=%d Py=%s" % (i, rc_c, rc_p))
            sys.exit(1)
        cycles += rc_p
        rc, rp = c.regs(), p.regs()
        if rc != rp:
            diffs = ["%s C=%X Py=%X" % (REG_NAMES[k], rc[k], rp[k]) for k in range(len(rc)) if rc[k] != rp[k]]
            print("register mismatch after instr %d (PC Py=%04X): %s" % (i, rp[0], ", ".join(diffs)))
            sys.exit(1)
        if i % 5000 == 0:
            if c.mem() != p.mem():
                mc, mp = c.mem(), p.mem()
                k = next(k for k in range(len(mc)) if mc[k] != mp[k])
                print("memory mismatch after instr %d at offset %X: C=%02X Py=%02X" % (i, k, mc[k], mp[k]))
                sys.exit(1)
        if i % 200000 == 0 and i:
            print("  %d instructions OK (%.1f s emulated, %d button events)" % (i, cycles / CLOCK, si), flush=True)
    if c.mem() != p.mem():
        print("final memory mismatch")
        sys.exit(1)
    ill = lib.h_illegal(c.h)
    print("PASS: %d instructions, %.1f s of emulated time, %d button events, illegal=%s"
          % (n_instr, cycles / CLOCK, si, "none" if ill < 0 else "%04X" % ill))


if __name__ == "__main__":
    main()
