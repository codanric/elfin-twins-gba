#!/usr/bin/env python3
"""The sleep fast path in splb20_run() must give bit-identical results to
stepping through the sleep one 32 kHz tick at a time."""
import ctypes, pickle, sys, os
import elfin_sim

def main():
    snap_path = sys.argv[1] if len(sys.argv) > 1 else None
    a, b = elfin_sim.Pet(), elfin_sim.Pet()
    b.lib.h_run_noskip.argtypes = [ctypes.c_void_p, ctypes.c_int32]
    if snap_path:
        s = pickle.load(open(snap_path, "rb"))
    else:
        a.run(200); s = a.snapshot()
    a.restore(s); b.restore(s)
    chunk = 100000 * 256
    for i in range(3000):          # ~9 minutes of emulated time
        da = a.lib.h_run(a.h, chunk)
        db = b.lib.h_run_noskip(b.h, chunk)
        if da != db or a.snapshot()[0] != b.snapshot()[0]:
            print("MISMATCH at chunk", i, da, db)
            sys.exit(1)
        if i % 500 == 0 and i:
            a.press("enter"); b.press("enter")
    print("PASS: sleep skip identical over %.0f s" % (3000 * 100000 / 560000))

if __name__ == "__main__":
    main()
