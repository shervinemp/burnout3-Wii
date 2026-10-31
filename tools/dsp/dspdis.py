"""Disassemble a GCDSP binary the way Dolphin does (its opcode table, DSPTables.cpp), to check what gcdsptool made.

gcdsptool and Dolphin do not agree on the operands of a few rare opcodes (MSUBC/MADDC: gcdsptool's `$acx0.h` is the
bit that Dolphin's interpreter reads as ax1.h), and the instruction semantics this project relies on are Dolphin's.
Every instruction of tools/dsp/dspmix.s is looked at through this listing.

    python -I tools/dsp/dspdis.py X.bin [DSPTables.cpp]       (X.bin: gcdsptool -c X.s -o X.bin)
"""
import re
import sys

TABLE = "F:/dolphin-src/Source/Core/Core/DSP/DSPTables.cpp"

REGS = ["ar0", "ar1", "ar2", "ar3", "ix0", "ix1", "ix2", "ix3", "wr0", "wr1", "wr2", "wr3", "st0", "st1", "st2", "st3",
        "ac0.h", "ac1.h", "cr", "sr", "prod.l", "prod.m", "prod.h", "prod.m2", "ax0.l", "ax1.l", "ax0.h", "ax1.h",
        "ac0.l", "ac1.l", "ac0.m", "ac1.m", "ac0", "ac1", "ax0", "ax1"]
# parameter type -> (base register index, flip for the "other accumulator" kinds, memory reference)
BASE = {"P_REG": 0, "P_REG18": 0x18, "P_REG19": 0x19, "P_REG1A": 0x1A, "P_REG1C": 0x1C, "P_REGM18": 0x18, "P_REGM19": 0x19,
        "P_ACCL": 0x1C, "P_ACCM": 0x1E, "P_ACCH": 0x10, "P_ACC": 0x20, "P_AX": 0x22, "P_ACCM_D": 0x1E, "P_ACC_D": 0x20,
        "P_PRG": 0}
FLIP = ("P_ACCM_D", "P_ACC_D")


def load(path):
    ops, exts = [], []
    cur = None
    for line in open(path, encoding="utf-8").read().splitlines():
        if "s_opcodes_ext" in line and "const" in line:
            cur = exts
            continue
        if "s_opcodes" in line and "const" in line:
            cur = ops
            continue
        m = re.match(r'\s*\{"(\w+)",\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*(\d),\s*(\d),(.*)', line)
        if not m or cur is None:
            continue
        name, opc, mask, size, nparam, rest = m.groups()
        params = re.findall(r"\{(P_\w+),\s*(\d+),\s*(-?\d+),\s*(-?\d+),\s*(0x[0-9a-fA-F]+)\}", rest)
        flags = re.findall(r"(true|false)", re.sub(r"\{[^{}]*\}", "", rest))
        extendable = bool(flags) and flags[0] == "true"
        cur.append((name, int(opc, 16), int(mask, 16), int(size), [(t, int(sz), int(loc), int(sh), int(mk, 16)) for t, sz, loc, sh, mk in params], extendable))
    return ops, exts


def pick(table, word):
    best = None
    for t in table:
        if word & t[2] == t[1] and (best is None or bin(t[2]).count("1") > bin(best[2]).count("1")):
            best = t
    return best


def render(t, w1, w2):
    out = []
    for typ, size, loc, sh, mk in t[4]:
        v = (w2 if loc >= 1 else w1) & mk
        v = v << -sh if sh < 0 else v >> sh
        if typ in BASE:
            if typ in FLIP:
                v = (~v & 1) | BASE[typ]
            else:
                v |= BASE[typ]
            nm = REGS[v] if v < len(REGS) else "r%d" % v
            out.append(("@$" if typ == "P_PRG" else "$") + nm)
        elif typ == "P_IMM":
            if size != 2 and mk == 0x3F:
                out.append("#%d" % (v - 64 if v & 0x20 else v))
            else:
                out.append("#0x%x" % v)
        elif typ == "P_MEM":
            out.append("@0x%04x" % (v if size == 2 else (v if v < 0x80 else v | 0xFF00)))
        else:
            out.append("0x%04x" % v)
    return ", ".join(out)


def main(argv):
    ops, exts = load(argv[2] if len(argv) > 2 else TABLE)
    data = open(argv[1], "rb").read()
    words = [(data[i] << 8) | data[i + 1] for i in range(0, len(data) - 1, 2)]
    i = 0
    while i < len(words):
        w = words[i]
        t = pick(ops, w)
        if t is None:
            print("%04x %04x  ???" % (i, w))
            i += 1
            continue
        w2 = words[i + 1] if t[3] == 2 and i + 1 < len(words) else 0
        text = "%-8s %s" % (t[0], render(t, w, w2))
        if t[5] and (w & 0xFF):
            e = pick(exts, w & 0xFF)
            if e:
                text = "%s : %s %s" % (text, e[0], render(e, w & 0xFF, 0))
        print("%04x %04x %s %s" % (i, w, ("%04x" % w2) if t[3] == 2 else "    ", text))
        i += t[3]


if __name__ == "__main__":
    main(sys.argv)
