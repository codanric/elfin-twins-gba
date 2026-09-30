#!/usr/bin/env python3
"""
Validate the time-away catch-up: starting from the same state, compare
  A) leaving the pet running normally for N hours (full emulation), and
  B) applying N hours with elfin_catchup (what the GBA port does at power-on).
The clocks, age counters and care statistics must match; only cosmetic
animation state and the random number generator may differ.
"""
import ctypes, sys, time
import elfin_sim

# RAM addresses compared (see docs for the RAM map)
CHECK = {
    0x82: "clock half-seconds", 0x83: "clock hour", 0x84: "clock minute",
    0x8A: "life half-seconds", 0x89: "life minutes", 0x88: "life 10-min blocks",
    0x8D: "age (days)", 0x80: "lifespan", 0xBD: "game mode",
}

def stats(p):
    return {a: p.peek(a) for a in CHECK}

def main():
    hours = float(sys.argv[1]) if len(sys.argv) > 1 else 6
    base = elfin_sim.Pet()
    base.run(200)                       # past the intro, normal mode
    L = base.lib
    L.h_catchup_begin.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    L.h_catchup_run.argtypes = [ctypes.c_void_p, ctypes.c_int32]
    L.h_is_idle.argtypes = [ctypes.c_void_p]
    # make sure we start asleep at the sleep point
    while not L.h_is_idle(base.h):
        base.run_cycles(50)
    snap = base.snapshot()

    a = elfin_sim.Pet(); a.restore(snap)
    t = time.time(); a.run(hours * 3600); ta = time.time() - t

    b = elfin_sim.Pet(); b.restore(snap)
    i0 = b.lib.h_instr(b.h)
    t = time.time()
    L.h_catchup_begin(b.h, int(hours * 3600))
    n = 0
    while L.h_catchup_run(b.h, 200000):
        n += 1
    tb = time.time() - t
    ib = b.lib.h_instr(b.h)
    st = (ctypes.c_uint32 * 6)(); L.h_catchup_stats(st)

    sa, sb = stats(a), stats(b)
    ok = True
    for addr, name in CHECK.items():
        mark = "" if sa[addr] == sb[addr] else "   <-- differs"
        if sa[addr] != sb[addr]:
            ok = False
        print("  $%02X %-20s full=%3d catchup=%3d%s" % (addr, name, sa[addr], sb[addr], mark))
    ra, rb = a.ram(), b.ram()
    diff = [i for i in range(0x80, 0x100) if ra[i] != rb[i]]
    print("  other CPU RAM bytes that differ:", " ".join("%02X" % i for i in diff))
    print("  catch-up: fast=%d tick-calls=%d wake-ups=%d emulated=%d" % (st[1], st[2], st[4], st[3]))
    print("  host time: full %.1fs, catch-up %.2fs; catch-up executed %d instructions (%.0f per hour)"
          % (ta, tb, ib - i0, (ib - i0) / hours))
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
