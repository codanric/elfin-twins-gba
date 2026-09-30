#!/usr/bin/env python3
"""splb20_run() (sleep skip + batched timers) must produce exactly the same
machine state as stepping instruction by instruction with splb20_step()."""
import ctypes, random, sys
import elfin_sim

def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 600
    a, b = elfin_sim.Pet(), elfin_sim.Pet()
    b.lib.h_run_noskip.argtypes = [ctypes.c_void_p, ctypes.c_int32]
    rng = random.Random(7)
    chunk = 7919 * 256          # odd size so boundaries fall everywhere
    n = int(secs * 560000 / 7919)
    names = list(elfin_sim.BUTTONS)
    held = None
    for i in range(n):
        if i % 40 == 0:
            if held:
                a.up(held); b.up(held); held = None
            elif rng.random() < 0.5:
                held = rng.choice(names); a.down(held); b.down(held)
        da = a.lib.h_run(a.h, chunk)
        db = b.lib.h_run_noskip(b.h, chunk)
        if da != db or a.snapshot()[0] != b.snapshot()[0]:
            sa, sb = a.snapshot()[0], b.snapshot()[0]
            k = [j for j in range(len(sa)) if sa[j] != sb[j]][:8]
            print("MISMATCH at chunk %d (%.1f s): done %d vs %d, bytes %s" % (i, i * 7919 / 560000, da, db, k))
            sys.exit(1)
    print("PASS: run() == step() for %.0f s of emulated time" % secs)

if __name__ == "__main__":
    main()
