#!/usr/bin/env python3
"""Run each care action / option and log the stat routine calls."""
import collections, sys
from trace_effects import *

def traced_pet(snap='normal'):
    p = load(snap)
    t = Tracer(p)
    p.run = t.run
    p.press = t.press
    return p, t

def until_home(p, t, secs=90, poke=None):
    waited = 0
    while waited < secs:
        p.run(1); waited += 1
        if (p.peek(0xBD) & 0x0F) == 0 and waited > 2:
            return waited
        if poke and waited % 3 == 0:
            p.press(poke)
    return waited

def summarize(t):
    c = collections.Counter((e[0], e[1]) for e in t.log)
    return ", ".join("%s(mode %02X)x%d" % (k[0], k[1], v) for k, v in sorted(c.items()))

def run(item, option_moves, poke='enter', snap='normal'):
    p, t = traced_pet(snap)
    enter_item(p, item)
    p.run(1)
    for m in option_moves:
        p.press(m, after=0.5)
    t.log.clear()
    p.press('enter', after=0.5)
    w = until_home(p, t, poke=poke)
    return summarize(t), w, p

if __name__ == "__main__":
    for item, opts in [('food', [[], ['right'], ['right', 'right']]),
                       ('picture', [[], ['right'], ['right', 'right']]),
                       ('game', [[], ['right']]),
                       ('clean', [[]]), ('bag', [[]])]:
        for o in opts:
            s, w, p = run(item, o)
            print("%-8s option %d: %s  (%ds)" % (item, len(o), s or "-", w))
