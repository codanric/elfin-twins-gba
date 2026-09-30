#!/usr/bin/env python3
"""Log calls of the stat routines while playing, to build an effects table."""
import ctypes, collections
import elfin_sim
from explore import *

STAT_ROUTINES = {
    0x9E46: "$94-", 0x9E54: "stamina-", 0x9E5B: "IQ-", 0x9E62: "mood-", 0x9E69: "social-",
    0x9E70: "money-", 0x9E7E: "weight-", 0x9E87: "$94+", 0x9E90: "$93+", 0x9E99: "stamina+",
    0x9EA2: "IQ+", 0x9EAB: "mood+", 0x9EB4: "social+", 0x9EBD: "money+", 0x9EC6: "weight+",
    0x9E4D: "$93-",
}

class Tracer:
    def __init__(self, p):
        self.p = p
        L = p.lib
        L.h_trap_set.argtypes = [ctypes.c_int, ctypes.c_int]
        L.h_run_traced.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_void_p, ctypes.c_int]
        for a in STAT_ROUTINES:
            L.h_trap_set(a, 1)
        self.buf = (ctypes.c_uint16 * (4 * 4096))()
        self.log = []

    def run(self, secs):
        n = self.p.lib.h_run_traced(self.p.h, int(secs * 560000), self.buf, 4096)
        self.p.cycles += int(secs * 560000)
        for i in range(n):
            pc, mode, ca, ba = self.buf[4 * i:4 * i + 4]
            self.log.append((STAT_ROUTINES[pc], mode, ca, ba))

    def press(self, k, hold=0.15, after=0.35):
        self.p.down(k); self.run(hold); self.p.up(k); self.run(after)
