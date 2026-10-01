#!/usr/bin/env python3
"""Regression: splb20_run() must dispatch installed PC hooks in its fast path.

The GBA front end intercepts the logical Elfin link paths at BE0D, BDB7,
BE41 and BA99. A previous implementation checked hooks only in splb20_step(), while
the normal awake splb20_run() path executed instructions directly, making the
link rewrite unreachable during ordinary play.
"""
import ctypes
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

import verify_core

TRAPS = (0xBE0D, 0xBE41, 0xBDB7, 0xBA99)


def main():
    lib = verify_core.build_lib()
    rom = open(os.path.join(ROOT, "assets", "ElfinTwins.bin"), "rb").read()

    lib.h_create.restype = ctypes.c_void_p
    lib.h_test_hook_arm.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.h_test_hook_arm_wait.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.h_test_hook_hits_get.argtypes = [ctypes.c_void_p]
    lib.h_test_hook_hits_get.restype = ctypes.c_int
    lib.h_test_hook_clear.argtypes = [ctypes.c_void_p]
    lib.h_run.argtypes = [ctypes.c_void_p, ctypes.c_int32]
    lib.h_run.restype = ctypes.c_int32
    lib.h_destroy.argtypes = [ctypes.c_void_p]

    h = lib.h_create(rom, len(rom), 560000, 0, 255)
    if not h:
        raise RuntimeError("failed to create SPLB20 harness")

    try:
        for pc in TRAPS:
            lib.h_test_hook_arm(h, pc)
            # One emulated cycle is smaller than the hook's consumed cost,
            # guaranteeing exactly one outer-loop dispatch.
            lib.h_run(h, 1 << 8)
            hits = lib.h_test_hook_hits_get(h)
            if hits != 1:
                print("FAIL: PC %04X hook hits=%d (expected 1)" % (pc, hits))
                return 1
            lib.h_test_hook_clear(h)
            print("PASS: splb20_run dispatched hook at %04X" % pc)

        # A WAIT hook represents a native event that cannot arrive until the
        # GBA IRQ dispatcher runs again. Even with a large run budget, the
        # core must yield after one 8-cycle wait instead of spinning at BE41.
        lib.h_test_hook_arm_wait(h, 0xBE41)
        budget = 560000 << 8
        done = lib.h_run(h, budget)
        hits = lib.h_test_hook_hits_get(h)
        if hits != 1 or done >= budget:
            print("FAIL: WAIT hook did not yield: hits=%d done=%d budget=%d" %
                  (hits, done, budget))
            return 1
        lib.h_test_hook_clear(h)
        print("PASS: WAIT hook yielded partial run slice at BE41")

    finally:
        lib.h_destroy(h)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
