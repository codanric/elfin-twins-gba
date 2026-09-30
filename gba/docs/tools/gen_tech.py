#!/usr/bin/env python3
"""
Build gba/docs/tech/technical.html from technical.src.html by filling in the
tables that are generated from the sources of truth:

  <!--@SFR-->       SFR list            (gba/tools/elfin_symbols.py)
  <!--@RAM-->       RAM map             (gba/tools/elfin_symbols.py)
  <!--@ROUTINES-->  routine index       (gba/tools/elfin_symbols.py)
  <!--@OPCODES-->   instruction set     (cores/SPLB20.py, cycle counts parsed)
  <!--@MATRIX-->    LCD RAM -> dot map  (assets/ElfinTwins.svg)
  <!--@ICONS-->     icon segments       (assets/ElfinTwins.svg)
  <!--@MODES-->     mode jump tables    (assets/ElfinTwins.bin)
  <!--@STATS-->     ROM statistics      (gba/tools/elfin_disasm.py)
"""
import html
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "gba", "tools"))

import elfin_disasm  # noqa: E402
import elfin_lcd  # noqa: E402
import elfin_symbols as sym  # noqa: E402

SRC = os.path.join(ROOT, "gba", "docs", "tech", "technical.src.html")
OUT = os.path.join(ROOT, "gba", "docs", "tech", "technical.html")

MODE_NAMES = {
    0x0: "Home (normal life)", 0x1: "Food & drink", 0x2: "Light", 0x3: "Games",
    0x4: "Doctor / hospital", 0x5: "Clean", 0x6: "Status meter", 0x7: "Outings",
    0x8: "Asleep (lights off)", 0x9: "Birthday", 0xA: "Collapsed", 0xB: "Farewell (end of life)",
    0xC: "Opening story + SIGN NAME", 0xD: "Password", 0xE: "Link session",
}


def e(s):
    return html.escape(str(s))


def table(head, rows, cls=""):
    out = ['<table class="%s"><thead><tr>' % cls]
    out += ["<th>%s</th>" % e(h) for h in head]
    out.append("</tr></thead><tbody>")
    for r in rows:
        out.append("<tr>" + "".join("<td>%s</td>" % c for c in r) + "</tr>")
    out.append("</tbody></table>")
    return "".join(out)


def gen_sfr():
    rows = [("$%02X" % a, "<code>%s</code>" % e(n), e(d)) for a, (n, d) in sorted(sym.SFR.items())]
    return table(["Addr", "Name", "Function"], rows)


def gen_ram():
    rows = [("$%02X" % a, "<code>%s</code>" % e(v[0]), e(v[1] if len(v) > 1 else ""))
            for a, v in sorted(sym.RAM.items())]
    return table(["Addr", "Name", "Meaning"], rows, "compact")


def gen_routines():
    rows = [("$%04X" % a, "<code>%s</code>" % e(n), e(d.replace("\n", " ")))
            for a, (n, d) in sorted(sym.ROUTINES.items())]
    return table(["Addr", "Name", "What it does"], rows, "compact")


def gen_opcodes():
    src = open(os.path.join(ROOT, "cores", "SPLB20.py")).read()
    bodies = {}
    for m in re.finditer(r"    def (_\w+)\(self\):\n(.*?)(?=\n    def |\Z)", src, re.S):
        bodies[m.group(1)] = m.group(2)
    rows = []
    for op, (mn, mode) in sorted(elfin_disasm.OPS.items()):
        mode_name = ["impl", "#imm", "zp", "zp,X", "abs", "abs,X", "(ind)", "(zp,X)", "rel", "A"][mode]
        fn = "_%s_%s" % (mn, {0: "", 1: "imm", 2: "zp", 3: "zp_x", 4: "abs", 5: "abs_x", 6: "ind",
                              7: "ind_x", 8: "", 9: "a"}[mode])
        fn = fn.rstrip("_")
        body = bodies.get(fn, "")
        cyc = sorted(set(re.findall(r"return (\d+)", body)))
        if mode == 8:
            cyc_s = "2 / 3 (+1 page)"
        elif "(base ^ addr) > 255" in body:
            cyc_s = "%s (+1 page)" % "/".join(cyc)
        else:
            cyc_s = "/".join(cyc) or "?"
        rows.append(("$%02X" % op, "<code>%s</code>" % mn, mode_name, cyc_s))
    half = (len(rows) + 1) // 2
    left = table(["Op", "Mn", "Mode", "Cycles"], rows[:half], "compact")
    right = table(["Op", "Mn", "Mode", "Cycles"], rows[half:], "compact")
    return '<div class="cols2">%s%s</div>' % (left, right)


def gen_matrix():
    out = ['<table class="matrix"><thead><tr><th></th>']
    out += ["<th>%d</th>" % x for x in range(elfin_lcd.MATRIX_W)]
    out.append("</tr></thead><tbody>")
    for y in range(elfin_lcd.MATRIX_H):
        out.append("<tr><th>%d</th>" % y)
        for x in range(elfin_lcd.MATRIX_W):
            b, bit = elfin_lcd.MATRIX[(x, y)]
            out.append("<td>%02X<sub>%d</sub></td>" % (b, bit))
        out.append("</tr>")
    out.append("</tbody></table>")
    return "".join(out)


def gen_icons():
    rows = []
    for key in sorted(elfin_lcd.ICONS, key=lambda k: elfin_lcd.ICON_NAMES[k]):
        rows.append(("<code>%s</code>" % elfin_lcd.ICON_NAMES[key], "$%02X" % key[0], str(key[1]),
                     "$%02X" % (1 << key[1])))
    return table(["Icon", "LCD byte", "Bit", "Mask"], rows, "compact")


def gen_modes():
    rom = elfin_disasm.Rom(open(os.path.join(ROOT, "assets", "ElfinTwins.bin"), "rb").read())
    rows = []
    for m in range(15):
        tick = (rom.w(0x957F + 2 * m) + 1) & 0xFFFF
        key = (rom.w(0x959D + 2 * m) + 1) & 0xFFFF
        tn = sym.ROUTINES.get(tick, ("",))[0]
        kn = sym.ROUTINES.get(key, ("",))[0]
        rows.append(("$%X" % m, e(MODE_NAMES[m]), "$%04X <code>%s</code>" % (tick, e(tn)),
                     "$%04X <code>%s</code>" % (key, e(kn))))
    return table(["Mode", "Meaning", "Tick handler (T_957F)", "Key handler (T_959D)"], rows)


def gen_stats():
    rom = elfin_disasm.Rom(open(os.path.join(ROOT, "assets", "ElfinTwins.bin"), "rb").read())
    code, labels, tables, refs = elfin_disasm.analyse(rom)
    nbytes = sum(v[2] for v in code.values())
    nsub = sum(1 for v in labels.values() if v == "sub")
    return ("%d bytes of code reachable from the vectors and jump tables (%.1f%% of the ROM), "
            "%d subroutines, %d jump tables; the rest is graphics, animation scripts, melodies "
            "and lookup tables." % (nbytes, 100.0 * nbytes / 0x8000, nsub, len(tables)))


def main():
    s = open(SRC, encoding="utf-8").read()
    for name, fn in (("SFR", gen_sfr), ("RAM", gen_ram), ("ROUTINES", gen_routines),
                     ("OPCODES", gen_opcodes), ("MATRIX", gen_matrix), ("ICONS", gen_icons),
                     ("MODES", gen_modes), ("STATS", gen_stats)):
        s = s.replace("<!--@%s-->" % name, fn())
    open(OUT, "w", encoding="utf-8").write(s)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
