"""VU microcode disassembler.

    python -I tools/vu/vudis.py FILE [--offset N] [--start ADDR] [--count N]

FILE holds micro memory as little-endian words (lower word at 8n, upper at
8n+4), e.g. a slice of a VU memory dump or of the ELF's MPG data. Addresses
are bytes in micro memory.
"""
import argparse
import struct

DEST = lambda op: "".join(c for c, b in zip("xyzw", (8, 4, 2, 1)) if (op >> 21) & b)
BC = "xyzw"


def fmac(op):
    ft, fs, fd = (op >> 16) & 31, (op >> 11) & 31, (op >> 6) & 31
    d = DEST(op)
    code = op & 0x3F
    if code >= 0x3C:
        x = ((op >> 4) & 0x7C) | (op & 3)
        bc = BC[op & 3]
        names = {0x00: "adda", 0x04: "suba", 0x08: "madda", 0x0C: "msuba", 0x18: "mula"}
        base = x & ~3
        if base in names and x < 0x1C:
            return f"{names[base]}{bc}.{d} acc, vf{fs}, vf{ft}{bc}"
        if 0x10 <= x <= 0x13:
            return f"itof{[0, 4, 12, 15][x & 3]}.{d} vf{ft}, vf{fs}"
        if 0x14 <= x <= 0x17:
            return f"ftoi{[0, 4, 12, 15][x & 3]}.{d} vf{ft}, vf{fs}"
        special = {0x1C: "mulaq", 0x1D: "abs", 0x1E: "mulai", 0x1F: "clipw", 0x20: "addaq", 0x21: "maddaq",
                   0x22: "addai", 0x23: "maddai", 0x24: "subaq", 0x25: "msubaq", 0x26: "subai", 0x27: "msubai",
                   0x28: "adda", 0x29: "madda", 0x2A: "mula", 0x2C: "suba", 0x2D: "msuba", 0x2E: "opmula",
                   0x2F: "nop"}
        n = special.get(x, f"upper?{x:02X}")
        if n == "nop":
            return "nop"
        if n == "abs":
            return f"abs.{d} vf{ft}, vf{fs}"
        if n == "clipw":
            return f"clipw.xyz vf{fs}, vf{ft}w"
        return f"{n}.{d} acc, vf{fs}, vf{ft}"
    bc = BC[op & 3]
    groups = ["add", "sub", "madd", "msub", "max", "mini", "mul"]
    g = code >> 2
    if g < 7:
        return f"{groups[g]}{bc}.{d} vf{fd}, vf{fs}, vf{ft}{bc}"
    table = {0x1C: "mulq", 0x1D: "maxi", 0x1E: "muli", 0x1F: "minii", 0x20: "addq", 0x21: "maddq", 0x22: "addi",
             0x23: "maddi", 0x24: "subq", 0x25: "msubq", 0x26: "subi", 0x27: "msubi", 0x28: "add", 0x29: "madd",
             0x2A: "mul", 0x2B: "max", 0x2C: "sub", 0x2D: "msub", 0x2E: "opmsub", 0x2F: "mini"}
    n = table.get(code, f"upper?{code:02X}")
    if n.endswith("q") or n.endswith("i") and n not in ("mini",):
        src = "Q" if n.endswith("q") else "I"
        return f"{n}.{d} vf{fd}, vf{fs}, {src}"
    return f"{n}.{d} vf{fd}, vf{fs}, vf{ft}"


def lower(op, pc):
    opc = op >> 25
    ft, fs, fd = (op >> 16) & 31, (op >> 11) & 31, (op >> 6) & 31
    it, is_ = ft & 15, fs & 15
    d = DEST(op)
    imm11 = (op & 0x7FF) - (0x800 if op & 0x400 else 0)
    tgt = pc + 8 + imm11 * 8
    m = {0x00: f"lq.{d} vf{ft}, {imm11}(vi{is_})", 0x01: f"sq.{d} vf{fs}, {imm11}(vi{it})",
         0x04: f"ilw.{d} vi{it}, {imm11}(vi{is_})", 0x05: f"isw.{d} vi{it}, {imm11}(vi{is_})",
         0x08: f"iaddiu vi{it}, vi{is_}, {((op >> 10) & 0x7800) | (op & 0x7FF)}",
         0x09: f"isubiu vi{it}, vi{is_}, {((op >> 10) & 0x7800) | (op & 0x7FF)}",
         0x10: f"fceq vi1, {op & 0xFFFFFF:06X}", 0x11: f"fcset {op & 0xFFFFFF:06X}",
         0x12: f"fcand vi1, {op & 0xFFFFFF:06X}", 0x13: f"fcor vi1, {op & 0xFFFFFF:06X}",
         0x14: f"fseq vi{it}, {((op >> 10) & 0x800) | (op & 0x7FF):03X}",
         0x15: f"fsset {((op >> 10) & 0x800) | (op & 0x7FF):03X}",
         0x16: f"fsand vi{it}, {((op >> 10) & 0x800) | (op & 0x7FF):03X}",
         0x17: f"fsor vi{it}, {((op >> 10) & 0x800) | (op & 0x7FF):03X}",
         0x18: f"fmeq vi{it}, vi{is_}", 0x1A: f"fmand vi{it}, vi{is_}", 0x1B: f"fmor vi{it}, vi{is_}",
         0x1C: f"fcget vi{it}", 0x20: f"b {tgt:04X}", 0x21: f"bal vi{it}, {tgt:04X}",
         0x24: f"jr vi{is_}", 0x25: f"jalr vi{it}, vi{is_}",
         0x28: f"ibeq vi{it}, vi{is_}, {tgt:04X}", 0x29: f"ibne vi{it}, vi{is_}, {tgt:04X}",
         0x2C: f"ibltz vi{is_}, {tgt:04X}", 0x2D: f"ibgtz vi{is_}, {tgt:04X}",
         0x2E: f"iblez vi{is_}, {tgt:04X}", 0x2F: f"ibgez vi{is_}, {tgt:04X}"}
    if opc in m:
        return m[opc]
    if opc != 0x40:
        return f"lower?{opc:02X}"
    sub = op & 0x3F
    imm5 = fd - 32 if fd & 16 else fd
    small = {0x30: f"iadd vi{fd & 15}, vi{is_}, vi{it}", 0x31: f"isub vi{fd & 15}, vi{is_}, vi{it}",
             0x32: f"iaddi vi{it}, vi{is_}, {imm5}", 0x34: f"iand vi{fd & 15}, vi{is_}, vi{it}",
             0x35: f"ior vi{fd & 15}, vi{is_}, vi{it}"}
    if sub in small:
        return small[sub]
    if sub < 0x3C:
        return f"lowerop?{sub:02X}"
    x = ((op >> 4) & 0x7C) | (op & 3)
    fsf, ftf = BC[(op >> 21) & 3], BC[(op >> 23) & 3]
    ext = {0x30: f"move.{d} vf{ft}, vf{fs}", 0x31: f"mr32.{d} vf{ft}, vf{fs}",
           0x34: f"lqi.{d} vf{ft}, (vi{is_}++)", 0x35: f"sqi.{d} vf{fs}, (vi{it}++)",
           0x36: f"lqd.{d} vf{ft}, (--vi{is_})", 0x37: f"sqd.{d} vf{fs}, (--vi{it})",
           0x38: f"div Q, vf{fs}{fsf}, vf{ft}{ftf}", 0x39: f"sqrt Q, vf{ft}{ftf}",
           0x3A: f"rsqrt Q, vf{fs}{fsf}, vf{ft}{ftf}", 0x3B: "waitq", 0x3C: f"mtir vi{it}, vf{fs}{fsf}",
           0x3D: f"mfir.{d} vf{ft}, vi{is_}", 0x3E: f"ilwr.{d} vi{it}, (vi{is_})", 0x3F: f"iswr.{d} vi{it}, (vi{is_})",
           0x40: f"rnext.{d} vf{ft}, R", 0x41: f"rget.{d} vf{ft}, R", 0x42: f"rinit R, vf{fs}{fsf}",
           0x43: f"rxor R, vf{fs}{fsf}", 0x64: f"mfp.{d} vf{ft}, P", 0x68: f"xtop vi{it}", 0x69: f"xitop vi{it}",
           0x6C: f"xgkick vi{is_}", 0x70: f"esadd P, vf{fs}", 0x71: f"ersadd P, vf{fs}", 0x72: f"eleng P, vf{fs}",
           0x73: f"erleng P, vf{fs}", 0x74: f"eatanxy P, vf{fs}", 0x75: f"eatanxz P, vf{fs}", 0x76: f"esum P, vf{fs}",
           0x78: f"esqrt P, vf{fs}{fsf}", 0x79: f"ersqrt P, vf{fs}{fsf}", 0x7A: f"ercpr P, vf{fs}{fsf}",
           0x7B: "waitp", 0x7C: f"esin P, vf{fs}{fsf}", 0x7D: f"eatan P, vf{fs}{fsf}", 0x7E: f"eexp P, vf{fs}{fsf}"}
    if op == 0x8000033C:
        return "nop"
    return ext.get(x, f"lowerext?{x:02X}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--offset", type=lambda s: int(s, 0), default=0, help="file offset of micro address 0")
    ap.add_argument("--start", type=lambda s: int(s, 16), default=0)
    ap.add_argument("--count", type=int, default=64)
    a = ap.parse_args()
    data = open(a.file, "rb").read()
    for k in range(a.count):
        pc = a.start + 8 * k
        o = a.offset + pc
        if o + 8 > len(data):
            break
        lo, up = struct.unpack_from("<II", data, o)
        flags = "".join(f for f, b in zip("IEMDT", (31, 30, 29, 28, 27)) if (up >> b) & 1)
        low = f"loi {struct.unpack('<f', struct.pack('<I', lo))[0]:g}" if up >> 31 else lower(lo, pc)
        print(f"{pc:04X}: {flags:2s} {fmac(up):36s} | {low}")


if __name__ == "__main__":
    main()
