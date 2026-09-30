#!/usr/bin/env python3
"""Link cable robustness: two units must reach the shared tug-of-war game.

Runs the call -> answer -> choose activity sequence with the GBA port's
time slice (273 cycles per 1/2048 s) for several phase offsets and clock
skews, with the latched-edge model used by the GBA link bridge."""
import sys
from link_sim import *

def trial(phase, skew, slice_cycles, latch=True, bridge=False):
    a, b = load(), load()
    b.run(phase)
    if bridge:
        L = LinkC(a, b, slice_cycles=slice_cycles, skew=skew)
    else:
        L = Link(a, b, slice_cycles=slice_cycles, skew=skew, latch=latch)
    L.select(a, 0x10); L.ready(a)
    L.press(a, 'enter')
    for _ in range(12):
        L.step(0.5)
        if a.peek(0xB5) == 2 and b.peek(0xB5) == 5:
            break
    else:
        return 'no session (A.B5=%02X B.B5=%02X)' % (a.peek(0xB5), b.peek(0xB5))
    L.press(a, 'enter')
    for _ in range(12):
        L.step(0.5)
        if (a.peek(0xBD) >> 4) == 6 and (b.peek(0xBD) >> 4) == 6:
            return 'ok'
    return 'no game (A.BD=%02X B.BD=%02X)' % (a.peek(0xBD), b.peek(0xBD))

if __name__ == "__main__":
    fails = 0
    runs = 0
    modes = [('bridge', True, True)] if '--bridge' in sys.argv else [('python', False, False), ('python', True, False)]
    for name, latch, bridge in modes:
        for slice_cycles in (100, 273, 546):
            for phase in (0.05, 0.37, 0.81):
                for skew in (0.97, 1.0, 1.03):
                    r = trial(phase, skew, slice_cycles, latch, bridge)
                    runs += 1
                    if latch and r != 'ok':
                        fails += 1
                    print(name + ' latch=%d slice=%3d phase=%.2f skew=%.2f: %s' % (latch, slice_cycles, phase, skew, r), flush=True)
    print('latched model failures: %d' % fails)
    sys.exit(1 if fails else 0)
