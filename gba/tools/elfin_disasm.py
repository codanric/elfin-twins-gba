#!/usr/bin/env python3
"""
Analysis disassembler for the Elfin Twins ROM (SPLB20, 6502 subset).

Unlike the listing view in BrickEmuPy it follows the ROM's jump tables
("lda tbl+1,x / pha / lda tbl,x / pha / rts"), names subroutines, branch
targets and tables, and annotates RAM / SFR accesses with the symbols in
elfin_symbols.py.

    python3 gba/tools/elfin_disasm.py [-o gba/docs/analysis/elfin_twins.asm]
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import elfin_symbols as sym  # noqa: E402

ROM_PATH = os.path.join(HERE, "..", "..", "assets", "ElfinTwins.bin")
BASE = 0x8000

# opcode -> (mnemonic, mode). Only the 70 opcodes the SPLB20 implements.
IMP, IMM, ZP, ZPX, ABS, ABX, IND, INX, REL, ACC = range(10)
SIZE = {IMP: 1, ACC: 1, IMM: 2, ZP: 2, ZPX: 2, INX: 2, REL: 2, ABS: 3, ABX: 3, IND: 3}
OPS = {
    0x00: ("brk", IMP), 0x05: ("ora", ZP), 0x08: ("php", IMP), 0x09: ("ora", IMM),
    0x10: ("bpl", REL), 0x18: ("clc", IMP), 0x20: ("jsr", ABS), 0x24: ("bit", ZP),
    0x25: ("and", ZP), 0x26: ("rol", ZP), 0x28: ("plp", IMP), 0x29: ("and", IMM),
    0x2A: ("rol", ACC), 0x2C: ("bit", ABS), 0x30: ("bmi", REL), 0x38: ("sec", IMP),
    0x40: ("rti", IMP), 0x45: ("eor", ZP), 0x48: ("pha", IMP), 0x49: ("eor", IMM),
    0x4C: ("jmp", ABS), 0x50: ("bvc", REL), 0x55: ("eor", ZPX), 0x58: ("cli", IMP),
    0x60: ("rts", IMP), 0x65: ("adc", ZP), 0x66: ("ror", ZP), 0x68: ("pla", IMP),
    0x69: ("adc", IMM), 0x6A: ("ror", ACC), 0x6C: ("jmp", IND), 0x70: ("bvs", REL),
    0x78: ("sei", IMP), 0x81: ("sta", INX), 0x85: ("sta", ZP), 0x86: ("stx", ZP),
    0x8A: ("txa", IMP), 0x8E: ("stx", ABS), 0x90: ("bcc", REL), 0x95: ("sta", ZPX),
    0x9A: ("txs", IMP), 0xA1: ("lda", INX), 0xA2: ("ldx", IMM), 0xA5: ("lda", ZP),
    0xA6: ("ldx", ZP), 0xA9: ("lda", IMM), 0xAA: ("tax", IMP), 0xAD: ("lda", ABS),
    0xAE: ("ldx", ABS), 0xB0: ("bcs", REL), 0xB5: ("lda", ZPX), 0xB8: ("clv", IMP),
    0xBA: ("tsx", IMP), 0xBD: ("lda", ABX), 0xC5: ("cmp", ZP), 0xC6: ("dec", ZP),
    0xC9: ("cmp", IMM), 0xCA: ("dex", IMP), 0xD0: ("bne", REL), 0xD5: ("cmp", ZPX),
    0xD6: ("dec", ZPX), 0xE0: ("cpx", IMM), 0xE4: ("cpx", ZP), 0xE5: ("sbc", ZP),
    0xE6: ("inc", ZP), 0xE8: ("inx", IMP), 0xE9: ("sbc", IMM), 0xEA: ("nop", IMP),
    0xF0: ("beq", REL), 0xF8: ("sed", IMP),
}
ENDS = {"rts", "rti", "jmp", "brk"}


class Rom:
    def __init__(self, data):
        self.d = data

    def b(self, a):
        return self.d[(a - BASE) & 0x7FFF]

    def w(self, a):
        return self.b(a) | (self.b(a + 1) << 8)


def decode(rom, a):
    op = rom.b(a)
    if op not in OPS:
        return None
    mn, mode = OPS[op]
    n = SIZE[mode]
    arg = None
    if n == 2:
        arg = rom.b(a + 1)
    elif n == 3:
        arg = rom.w(a + 1)
    return mn, mode, n, arg


def analyse(rom):
    code = {}          # addr -> (mn, mode, n, arg)
    labels = {}        # addr -> kind ('sub', 'loc', 'vec', 'tbl-entry')
    tables = {}        # table addr -> list of targets
    refs = {}          # target -> set of source addresses
    todo = []

    for name, vec in (("nmi", 0xFFFA), ("reset", 0xFFFC), ("irq", 0xFFFE)):
        t = rom.w(vec)
        labels[t] = "vec"
        todo.append(t)
    for t in getattr(sym, "ENTRIES", ()):      # entry points not reached from the vectors
        labels[t] = "sub"
        todo.append(t)

    def add(t, kind, src):
        if t < BASE:
            return
        refs.setdefault(t, set()).add(src)
        if t not in labels or kind == "sub":
            labels[t] = kind
        todo.append(t)

    while todo:
        a = todo.pop()
        while a not in code and BASE <= a <= 0xFFFF:
            dec = decode(rom, a)
            if dec is None:
                break
            mn, mode, n, arg = dec
            code[a] = dec
            nxt = a + n
            if mode == REL:
                t = (nxt + (arg - 256 if arg & 0x80 else arg)) & 0xFFFF
                add(t, "loc", a)
            if mn == "jsr":
                add(arg, "sub", a)
            if mn == "jmp" and mode == ABS:
                add(arg, "loc", a)
            # jump table: lda hi,x / pha / lda lo,x / pha / rts
            if (mn == "lda" and mode == ABX and rom.b(nxt) == 0x48 and rom.b(nxt + 1) == 0xBD
                    and rom.b(nxt + 4) == 0x48 and rom.b(nxt + 5) == 0x60):
                lo_tbl = rom.w(nxt + 2)
                targets = []
                for i in range(64):
                    ent = rom.w(lo_tbl + 2 * i)
                    t = (ent + 1) & 0xFFFF
                    if t < BASE or t >= 0xFFF0 or decode(rom, t) is None:
                        break
                    if lo_tbl + 2 * i in code:
                        break
                    targets.append(t)
                tables[lo_tbl] = targets
                for t in targets:
                    add(t, "loc", a)
            if mn in ENDS:
                break
            a = nxt
    return code, labels, tables, refs


def foreign(a):
    return any(lo <= a < hi for lo, hi in getattr(sym, "FOREIGN_CODE", ()))


def zp_symbol(z, a):
    """RAM/SFR symbol for zero page address z used by the instruction at a."""
    if foreign(a):
        return sym.SFR.get(z)
    return sym.RAM.get(z) or sym.SFR.get(z)


def fmt_operand(mn, mode, arg, a, n, labels):
    def name_zp(z):
        s = zp_symbol(z, a)
        return s[0] if s else "$%02X" % z

    def name_abs(t):
        if t in sym.ROUTINES:
            return sym.ROUTINES[t][0]
        if t in labels:
            return ("S_%04X" if labels[t] == "sub" else "L_%04X") % t
        if t < 0x100:
            return name_zp(t)
        return "$%04X" % t

    if mode in (IMP,):
        return ""
    if mode == ACC:
        return "A"
    if mode == IMM:
        return "#$%02X" % arg
    if mode == ZP:
        return name_zp(arg)
    if mode == ZPX:
        return name_zp(arg) + ",X"
    if mode == INX:
        return "(%s,X)" % name_zp(arg)
    if mode == ABS:
        return name_abs(arg)
    if mode == ABX:
        return name_abs(arg) + ",X"
    if mode == IND:
        return "(%s)" % name_abs(arg)
    if mode == REL:
        t = (a + n + (arg - 256 if arg & 0x80 else arg)) & 0xFFFF
        return name_abs(t)
    return ""


def comment_for(mn, mode, arg, a=0):
    if mode in (ZP, ZPX, INX) and arg is not None:
        s = zp_symbol(arg, a)
        if s and len(s) > 1:
            return s[1]
    return ""


def listing(rom, code, labels, tables, refs):
    out = []
    out.append("; Elfin Twins GM-021 - annotated disassembly")
    out.append("; generated by gba/tools/elfin_disasm.py from assets/ElfinTwins.bin")
    out.append("; %d bytes of code found, %d subroutines, %d jump tables"
               % (sum(v[2] for v in code.values()), sum(1 for v in labels.values() if v == "sub"),
                  len(tables)))
    out.append("")
    covered = set()
    for a, v in code.items():
        for i in range(v[2]):
            covered.add(a + i)
    table_bytes = {}
    for t, ents in tables.items():
        for i in range(len(ents)):
            table_bytes[t + 2 * i] = ents[i]
    a = BASE
    data_run = []

    def flush():
        if data_run:
            start = data_run[0]
            for i in range(0, len(data_run), 16):
                chunk = data_run[i:i + 16]
                out.append("%04X:  .db %s" % (chunk[0], ", ".join("$%02X" % rom.b(x) for x in chunk)))
            data_run.clear()

    data = getattr(sym, "DATA", {})
    while a <= 0xFFFF:
        if a in data and a not in code:
            flush()
            name, desc, length, kind = data[a]
            out.append("")
            out.append("; " + "-" * 70)
            for line in desc.split("\n"):
                out.append("; " + line)
            if kind == "text20":
                out.append(';   "%s"' % "".join(chr(rom.b(a + i) - 0x20) for i in range(length)))
            out.append("%s:" % name)
            if kind == "vectors":
                for v in range(a, a + length, 2):
                    t = rom.w(v)
                    nm = sym.ROUTINES.get(t, (("S_%04X" if labels.get(t) == "sub" else "L_%04X") % t,))[0]
                    out.append("%04X:  .dw %-24s; %s" % (v, nm if t >= BASE else "$%04X" % t,
                                                        sym.VECTOR_NAMES.get(v, "")))
            else:
                for i in range(0, length, 16):
                    n = min(16, length - i)
                    line = "%04X:  .db %s" % (a + i, ", ".join("$%02X" % rom.b(a + i + j) for j in range(n)))
                    if kind == "text20":
                        line += "  ; " + "".join(chr(rom.b(a + i + j) - 0x20) for j in range(n))
                    out.append(line)
            a += length
            continue
        if a in table_bytes:
            flush()
            if a in tables:
                out.append("")
                out.append("T_%04X:        ; jump table (entries are address-1)" % a)
            t = table_bytes[a]
            nm = sym.ROUTINES.get(t, ("L_%04X" % t,))[0]
            out.append("%04X:  .dw %s-1" % (a, nm))
            a += 2
            continue
        if a in code:
            flush()
            mn, mode, n, arg = code[a]
            if a in labels or a in sym.ROUTINES:
                nm, desc = sym.ROUTINES.get(a, (("S_%04X" if labels.get(a) == "sub" else "L_%04X") % a, ""))
                if labels.get(a) == "sub" or a in sym.ROUTINES:
                    out.append("")
                    out.append("; " + "-" * 70)
                    if desc:
                        for line in desc.split("\n"):
                            out.append("; " + line)
                    callers = sorted(refs.get(a, []))
                    if callers:
                        out.append("; called from: " + " ".join("%04X" % c for c in callers[:12])
                                   + (" ..." if len(callers) > 12 else ""))
                out.append("%s:" % nm)
            raw = " ".join("%02X" % rom.b(a + i) for i in range(n))
            text = "%-4s %s" % (mn, fmt_operand(mn, mode, arg, a, n, labels))
            cm = sym.COMMENTS.get(a) or comment_for(mn, mode, arg, a)
            out.append("%04X:  %-9s %-28s%s" % (a, raw, text, ("; " + cm) if cm else ""))
            a += n
            continue
        data_run.append(a)
        a += 1
    flush()
    return out, covered


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", default=os.path.join(HERE, "..", "docs", "analysis", "elfin_twins.asm"))
    args = ap.parse_args()
    rom = Rom(open(ROM_PATH, "rb").read())
    code, labels, tables, refs = analyse(rom)
    out, covered = listing(rom, code, labels, tables, refs)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w") as f:
        f.write("\n".join(out) + "\n")
    print("code bytes: %d (%.1f%% of ROM), subroutines: %d, tables: %d -> %s"
          % (len(covered), 100 * len(covered) / 0x8000,
             sum(1 for v in labels.values() if v == "sub"), len(tables), args.out))


if __name__ == "__main__":
    main()
