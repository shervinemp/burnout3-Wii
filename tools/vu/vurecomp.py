"""VU microcode static recompiler.

    python -I tools/vu/vurecomp.py ELF OUT_DIR --range LO HI [--vu0 CHAIN,...]

(tools/recomp runs it too, from VU_CHAINS / VU0_CHAINS / VU_ENTRIES in the
title config, writing the files next to the EE code.)

Finds the VIF MPG uploads in [LO, HI) of the ELF (a chain of MPG commands
starting at micro address 0 is one microcode image) and compiles each image
to a C function (gen/vu<unit>_<chain>.c, table in gen/vu_progs.c) that
runtime/vu.c calls instead of interpreting when micro memory holds that code.

The interpreter's timing model (runtime/vu.c) is evaluated at compile time:
the cycle counter, VF write latencies and stalls, the Q (DIV) and P (EFU)
latencies, the 4-cycle delay of MAC and clip flags and the VI value a branch
sees after an integer op are all static along a path, so each block is
compiled once per pipeline state it is entered with ("variants"). The
generated code then only computes values: a pending Q lands as "Q = QN" at
the right instruction, a flag update as "MAC = m_<pc>" and so on.

Programs start with a settled pipeline (the interpreter drains it at the end
of every run). Entries are address 0, every address after an E-bit pair (where
MSCNT continues) and --entry hints. JR/JALR targets are dispatched over the
return addresses of BAL/JALR, the constants loaded into the jump registers and
the entries; anything else falls back to the interpreter.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "recomp"))
from elf import Image  # noqa: E402

L = "xyzw"


class Unsupported(Exception):
    pass


def lanes(dest):
    return [k for k in range(4) if dest & (8 >> k)]


def imm11(op):
    return (op & 0x7FF) - (0x800 if op & 0x400 else 0)


def first_lane(dest):
    return 3 if dest & 1 else 2 if dest & 2 else 1 if dest & 4 else 0


def u32(x):
    return f"0x{x & 0xFFFFFFFF:08X}u"


# ---- images ---------------------------------------------------------------------------------

def find_images(img, lo, hi):
    """[(chain address, {pc: (lower, upper)})] for MPG chains in [lo, hi)"""
    mpgs = []
    a = lo
    while a < hi:
        w = img.u32(a)
        if (w >> 24) & 0x7F == 0x4A:
            num = (w >> 16) & 0xFF or 256
            mpgs.append((a, (w & 0xFFFF) * 8, num))
            a += 4 + num * 8
            continue
        a += 4
    images = []
    for a, addr, num in mpgs:
        if addr == 0 or not images:
            # the DMA tag and VIF codes in front of the first MPG: 12 bytes
            images.append((a - 12, {}))
        code = images[-1][1]
        for k in range(num):
            code[addr + 8 * k] = (img.u32(a + 4 + 8 * k), img.u32(a + 8 + 8 * k))
    return images


# ---- abstract pipeline state ------------------------------------------------------------------

class St:
    """Timing state during the simulation of a block; cycles are absolute
    within the block (block start = 0)."""

    def __init__(self):
        self.now = 0
        self.vf = {}        # reg -> [ready cycle per lane]
        self.q = None       # cycle the pending Q lands
        self.p = None
        self.fq = []        # [(ready, 'M' | 'C', producer pc)]
        self.bk = None      # (instructions left, VI reg) of the backed-up branch operand
        self.end = 0        # 1: the next instruction is the last one
        self.mdef = False   # MAC holds a result of this run (else the last run's)

    @staticmethod
    def from_key(key):
        s = St()
        vf, s.q, s.p, fq, s.bk, s.end, s.mdef = key
        s.vf = {r: list(v) for r, v in vf}
        s.fq = list(fq)
        return s

    def key(self):
        """relative state; call settle() first"""
        n = self.now
        vf = tuple(sorted((r, tuple(max(0, x - n) for x in v)) for r, v in self.vf.items()
                          if max(v) > n))
        return (vf, None if self.q is None else self.q - n, None if self.p is None else self.p - n,
                tuple((r - n, k, pc) for r, k, pc in self.fq), self.bk, self.end, self.mdef)


# ---- compiler -----------------------------------------------------------------------------------

class Prog:
    def __init__(self, unit, chain, code, entries, exact_names=False, native_subs=None):
        self.native_subs = native_subs or {}    # subroutine address -> C function (int f(VuCtx *))
        self.unit = unit
        self.chain = chain
        self.code = code              # pc -> (lower, upper)
        self.size = (max(code) + 8) if code else 0
        self.mask = 0x3FFF if unit else 0xFFF
        self.memq = 1024 if unit else 256
        self.entries = sorted(set(entries))
        self.out = []
        self.variants = {}            # (pc, key) -> label
        self.work = []
        self.vf_used = set()
        self.vf_written = set()
        self.vf_wl = {}               # register -> the lanes written
        self.vi_used = set()
        self.vi_written = set()
        self.flag_vars = set()
        self.long_vars = set()    # temps used after the delay slot
        self.jr_targets = self.find_jr_targets()
        self.tmp = 0

    # -- static analysis --

    def find_jr_targets(self):
        regs = set()
        targets = set(self.entries)
        for pc, (lo, up) in self.code.items():
            if up >> 31:
                continue
            opc = lo >> 25
            if opc in (0x24, 0x25):
                regs.add((lo >> 11) & 15)
            if opc in (0x21, 0x25):
                targets.add(pc + 16)
        for pc, (lo, up) in self.code.items():
            if up >> 31:
                continue
            opc = lo >> 25
            if opc == 0x08 and (lo >> 11) & 15 == 0 and (lo >> 16) & 15 in regs:
                c = (((lo >> 10) & 0x7800) | (lo & 0x7FF)) * 8
                if c in self.code:
                    targets.add(c)
        return sorted(t for t in targets if t in self.code)

    def reach(self, entry):
        """instruction addresses a run from entry can execute (the interpreter's
        control flow: delay slots, branches in delay slots ignored, one more
        instruction after the E bit)"""
        seen = set()
        visited = set()
        stack = [(entry, None, 0)]
        while stack:
            item = stack.pop()
            pc, pend, end = item
            if item in visited or pc not in self.code:
                continue
            visited.add(item)
            seen.add(pc)
            if end:
                continue
            e = 1 if self.code[pc][1] & 0x40000000 else 0
            nxt = (pc + 8) & self.mask
            if pend is not None:
                for t in pend:
                    stack.append((t, None, e))
                continue
            br = self.branch_of(pc)
            if br is not None:
                stack.append((nxt, tuple(br) + ((nxt + 8,) if self.cond_of(pc) else ()), e))
            else:
                stack.append((nxt, None, e))
        return seen

    def branch_of(self, pc):
        lo, up = self.code[pc]
        if up >> 31:
            return None
        opc = lo >> 25
        if opc in (0x20, 0x21, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F):
            return [(pc + 8 + imm11(lo) * 8) & self.mask]
        if opc in (0x24, 0x25):
            return list(self.jr_targets)
        return None

    def cond_of(self, pc):
        lo, up = self.code[pc]
        return not (up >> 31) and (lo >> 25) in (0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F)

    def leaders(self):
        ld = set(self.entries) | set(self.jr_targets)
        for pc in self.code:
            br = self.branch_of(pc)
            if br and (self.code[pc][0] >> 25) not in (0x24, 0x25):
                ld.update(br)
                ld.add(pc + 16)
        return ld

    # -- emission helpers --

    def emit(self, s):
        self.out.append(s)

    def vf(self, r, k):
        if r == 0:
            return "0.0f" if k < 3 else "1.0f"
        self.vf_used.add(r)
        return f"f{r}{L[k]}"

    def vfw(self, r, k):
        self.vf_used.add(r)
        self.vf_written.add(r)
        self.vf_wl.setdefault(r, set()).add(k)
        return f"f{r}{L[k]}"

    def vi(self, n):
        n &= 15
        if n == 0:
            return "0"
        self.vi_used.add(n)
        return f"v{n}"

    def set_vi(self, n, expr):
        n &= 15
        if n:
            self.vi_used.add(n)
            self.vi_written.add(n)
            self.emit(f"v{n} = ({expr}) & 0xFFFF;")

    def new_tmp(self):
        self.tmp += 1
        return f"t{self.tmp}"

    # -- timing --

    def stall_on(self, st, reg, mask):
        if not reg or reg not in st.vf:
            return
        for k in range(4):
            if mask & (8 >> k) and st.vf[reg][k] > st.now:
                st.now = st.vf[reg][k]

    def vf_mark(self, st, reg, dest):
        if not reg:
            return
        v = st.vf.setdefault(reg, [0, 0, 0, 0])
        for k in lanes(dest):
            v[k] = st.now + 4

    def q_sync(self, st):
        if st.q is not None and st.now >= st.q:
            self.emit("Q = QN;")
            st.q = None

    def p_sync(self, st):
        if st.p is not None and st.now >= st.p:
            self.emit("P = PN;")
            st.p = None

    def flags_sync(self, st):
        while st.fq and st.fq[0][0] <= st.now:
            _, kind, pc = st.fq.pop(0)
            if kind == "M":
                self.emit(f"MAC = m_{pc:04X};")
                st.mdef = True
            else:
                self.emit(f"CLIP = c_{pc:04X};")

    def flags_push(self, st, kind, pc):
        # land what is due first: flags are only seen by reads, so landing
        # early is the same, and it keeps the in-flight list short
        self.flags_sync(st)
        for _, k, p in st.fq:
            if k == kind and p == pc:
                raise Unsupported(f"flag update of {pc:04X} twice in flight")
        st.fq.append((st.now + 4, kind, pc))
        self.flag_vars.add(f"{'m' if kind == 'M' else 'c'}_{pc:04X}")

    def settle(self, st):
        """land what is due (at a block boundary)"""
        self.q_sync(st)
        self.p_sync(st)
        self.flags_sync(st)

    # -- upper --

    def upper(self, pc, op, st):
        """Emit the FMAC computation into temporaries. Returns the commit
        actions: (target, dest, {lane: temp}, flags, clip temp)."""
        dest, ft, fs, fd = (op >> 21) & 15, (op >> 16) & 31, (op >> 11) & 31, (op >> 6) & 31
        code = op & 0x3F
        src, kind, fn, acc_out = "vec", "plain", None, False
        bc = op & 3
        if code >= 0x3C:
            x = ((op >> 4) & 0x7C) | (op & 3)
            acc_out = True
            base = x & ~3
            if base == 0x00:
                fn, src = "+", "bc"
            elif base == 0x04:
                fn, src = "-", "bc"
            elif base == 0x08:
                fn, src, kind = "*", "bc", "madd"
            elif base == 0x0C:
                fn, src, kind = "*", "bc", "msub"
            elif base == 0x10:                                  # ITOF
                sc = ["1.0f", "(1.0f / 16)", "(1.0f / 4096)", "(1.0f / 32768)"][bc]
                self.stall_on(st, fs, dest)
                t = {k: f"(float)(int32_t)F2U({self.vf(fs, k)}) * {sc}" for k in lanes(dest)}
                return self.temps(("vf", ft), dest, t, False, None)
            elif base == 0x14:                                  # FTOI
                sc = ["", " * 16.0f", " * 4096.0f", " * 32768.0f"][bc]
                self.stall_on(st, fs, dest)
                t = {k: f"U2F((uint32_t)VFTOI({self.vf(fs, k)}{sc}))" for k in lanes(dest)}
                return self.temps(("vf", ft), dest, t, False, None)
            elif base == 0x18:
                fn, src = "*", "bc"
            elif x == 0x1C:
                fn, src = "*", "q"
            elif x == 0x1D:                                     # ABS
                self.stall_on(st, fs, dest)
                t = {k: f"fabsf({self.vf(fs, k)})" for k in lanes(dest)}
                return self.temps(("vf", ft), dest, t, False, None)
            elif x == 0x1E:
                fn, src = "*", "i"
            elif x == 0x1F:                                     # CLIP
                self.stall_on(st, fs, 0xE)
                self.stall_on(st, ft, 0x1)
                cb = self.new_tmp()
                w = self.new_tmp()
                self.emit(f"float {w} = fabsf({self.vf(ft, 3)});")
                self.emit(f"uint32_t {cb} = ({self.vf(fs, 0)} > {w}) | ({self.vf(fs, 0)} < -{w}) << 1 | "
                          f"({self.vf(fs, 1)} > {w}) << 2 | ({self.vf(fs, 1)} < -{w}) << 3 | "
                          f"({self.vf(fs, 2)} > {w}) << 4 | ({self.vf(fs, 2)} < -{w}) << 5;")
                return (None, dest, {}, False, cb)
            elif 0x20 <= x <= 0x27:
                fn = "+" if x in (0x20, 0x22) else "-" if x in (0x24, 0x26) else "*"
                src = "q" if x in (0x20, 0x21, 0x24, 0x25) else "i"
                kind = "madd" if x in (0x21, 0x23) else "msub" if x in (0x25, 0x27) else "plain"
            elif x == 0x28:
                fn = "+"
            elif x == 0x29:
                fn, kind = "*", "madd"
            elif x == 0x2A:
                fn = "*"
            elif x == 0x2C:
                fn = "-"
            elif x == 0x2D:
                fn, kind = "*", "msub"
            elif x == 0x2E:                                     # OPMULA
                self.stall_on(st, fs, 0xE)
                self.stall_on(st, ft, 0xE)
                perm = {0: (1, 2), 1: (2, 0), 2: (0, 1)}
                t = {k: f"VCL({self.vf(fs, perm[k][0])} * {self.vf(ft, perm[k][1])})"
                     for k in lanes(dest) if k < 3}
                if dest & 1:
                    t[3] = "acw"
                return self.temps(("acc",), dest, t, True, None)
            elif x == 0x2F:
                return None
            else:
                raise Unsupported(f"upper special {x:02X} at {pc:04X}")
        else:
            g = code >> 2
            if g == 0:
                fn, src = "+", "bc"
            elif g == 1:
                fn, src = "-", "bc"
            elif g == 2:
                fn, src, kind = "*", "bc", "madd"
            elif g == 3:
                fn, src, kind = "*", "bc", "msub"
            elif g == 4:
                kind, src = "max", "bc"
            elif g == 5:
                kind, src = "min", "bc"
            elif g == 6:
                fn, src = "*", "bc"
            elif g == 7:
                fn, src, kind = [("*", "q", "plain"), (None, "i", "max"), ("*", "i", "plain"), (None, "i", "min")][bc]
            elif g == 8:
                fn, src, kind = [("+", "q", "plain"), ("*", "q", "madd"), ("+", "i", "plain"), ("*", "i", "madd")][bc]
            elif g == 9:
                fn, src, kind = [("-", "q", "plain"), ("*", "q", "msub"), ("-", "i", "plain"), ("*", "i", "msub")][bc]
            elif g == 0xA:
                fn, src, kind = [("+", "vec", "plain"), ("*", "vec", "madd"), ("*", "vec", "plain"),
                                 (None, "vec", "max")][bc]
            elif g == 0xB:
                if bc == 2:                                     # OPMSUB
                    self.stall_on(st, fs, 0xE)
                    self.stall_on(st, ft, 0xE)
                    perm = {0: (1, 2), 1: (2, 0), 2: (0, 1)}
                    t = {k: f"VCL(ac{L[k]} - {self.vf(fs, perm[k][0])} * {self.vf(ft, perm[k][1])})"
                         for k in lanes(dest) if k < 3}
                    if dest & 1:
                        t[3] = self.vf(fd, 3)
                    return self.temps(("vf", fd), dest, t, True, None)
                fn, src, kind = [("-", "vec", "plain"), ("*", "vec", "msub"), None, (None, "vec", "min")][bc]
            else:
                raise Unsupported(f"upper {code:02X} at {pc:04X}")

        target = ("acc",) if acc_out else ("vf", fd)
        self.stall_on(st, fs, dest)
        if src in ("vec", "bc"):
            self.stall_on(st, ft, (8 >> bc) if src == "bc" else dest)

        def b(k):
            if src == "vec":
                return self.vf(ft, k)
            if src == "bc":
                return self.vf(ft, bc)
            if src == "q":
                return "Q"
            return "I"

        if kind in ("max", "min"):
            f = "fp_max" if kind == "max" else "fp_min"
            t = {k: f"U2F({f}(F2U({self.vf(fs, k)}), F2U({b(k)})))" for k in lanes(dest)}
            return self.temps(target, dest, t, False, None)
        if src == "q":
            self.q_sync(st)
        t = {}
        for k in lanes(dest):
            a, bb = f"VIN({self.vf(fs, k)})", f"VIN({b(k)})"
            if kind == "madd":
                t[k] = f"VCL(VIN(ac{L[k]}) + VCL({a} * {bb}))"
            elif kind == "msub":
                t[k] = f"VCL(VIN(ac{L[k]}) - VCL({a} * {bb}))"
            else:
                t[k] = f"VCL({a} {fn} {bb})"
        return self.temps(target, dest, t, True, None)

    def temps(self, target, dest, exprs, flags, clip):
        names = {}
        for k, e in exprs.items():
            n = self.new_tmp()
            self.emit(f"float {n} = {e};")
            names[k] = n
        return (target, dest, names, flags, clip)

    def commit(self, pc, st, up):
        if up is None:
            return
        target, dest, names, flags, clip = up
        if target is not None and target[0] == "vf":
            reg = target[1]
            if reg:
                for k, n in names.items():
                    self.emit(f"{self.vfw(reg, k)} = {n};")
                self.vf_mark(st, reg, dest)
        elif target is not None and target[0] == "acc":
            for k, n in names.items():
                self.emit(f"ac{L[k]} = {n};")
        if flags:
            args = ", ".join(names.get(k, "0.0f") for k in range(4))
            self.emit(f"m_{pc:04X} = vu_mac({dest}, {args});")
            self.flags_push(st, "M", pc)
        if clip is not None:
            prev = "CLIP"
            for _, k, p in st.fq:
                if k == "C":
                    prev = f"c_{p:04X}"
            self.emit(f"c_{pc:04X} = (({prev}) << 6 | {clip}) & 0xFFFFFF;")
            self.flags_push(st, "C", pc)

    # -- lower --

    def lower_stalls(self, op, st):
        opc, dest, ft, fs = op >> 25, (op >> 21) & 15, (op >> 16) & 31, (op >> 11) & 31
        if opc == 0x01:
            self.stall_on(st, fs, dest)
            return
        if opc != 0x40 or (op & 0x3C) != 0x3C:
            return
        x = ((op >> 4) & 0x7C) | (op & 3)
        fsf, ftf = (op >> 21) & 3, (op >> 23) & 3
        if x in (0x30, 0x35, 0x37):
            self.stall_on(st, fs, dest)
        elif x == 0x31:
            self.stall_on(st, fs, ((dest >> 1) | (dest << 3)) & 15)
        elif x in (0x38, 0x3A, 0x3B, 0x39):
            if x in (0x38, 0x3A):
                self.stall_on(st, fs, 8 >> fsf)
                self.stall_on(st, ft, 8 >> ftf)
            if x == 0x39:
                self.stall_on(st, ft, 8 >> ftf)
            if st.q is not None and st.now < st.q:
                st.now = st.q
        elif x in (0x3C, 0x42, 0x43):
            self.stall_on(st, fs, 8 >> fsf)
        elif x in (0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x78, 0x79, 0x7A, 0x7C, 0x7D, 0x7E, 0x7B):
            if x != 0x7B:
                self.stall_on(st, fs, 0xF if x == 0x76 else 0xC if x == 0x74 else 0xA if x == 0x75
                              else 0xE if x < 0x78 else 8 >> fsf)
            if st.p is not None and st.now < st.p:
                st.now = st.p

    def vi_backup(self, st, n):
        n &= 15
        if not n:
            return
        if st.bk and st.bk[0] and st.bk[1] == n:
            st.bk = (2, n)
            return
        st.bk = (2, n)
        self.emit(f"BK = {self.vi(n)};")

    def bvi(self, st, n):
        n &= 15
        if st.bk and st.bk[0] and st.bk[1] == n:
            return "BK"
        return self.vi(n)

    def addr(self, expr):
        return f"(({expr}) & {self.memq - 1})"

    def vf_write(self, st, reg, dest, vals):
        """lower-unit VF write: vals[lane] are expressions of the old state"""
        if not reg:
            return
        names = {}
        for k in lanes(dest):
            n = self.new_tmp()
            self.emit(f"float {n} = {vals[k]};")
            names[k] = n
        for k, n in names.items():
            self.emit(f"{self.vfw(reg, k)} = {n};")
        self.vf_mark(st, reg, dest)

    def load_q(self, st, ft, dest, a):
        if not ft:
            return
        t = self.new_tmp()
        self.emit(f"uint32_t {t} = {self.addr(a)} * 4;")
        self.vf_write(st, ft, dest, {k: f"mf[{t} + {k}]" for k in range(4)})

    def store_q(self, st, fs, dest, a):
        self.stall_on(st, fs, dest)
        t = self.new_tmp()
        self.emit(f"uint32_t {t} = {self.addr(a)} * 4;")
        for k in lanes(dest):
            self.emit(f"mf[{t} + {k}] = {self.vf(fs, k)};")

    def div_issue(self, st, expr, latency):
        if st.q is not None and st.now < st.q:
            st.now = st.q
        self.q_sync(st)
        self.emit(f"QN = VCL({expr});")
        st.q = st.now + latency

    def efu_issue(self, st, expr, latency):
        if st.p is not None and st.now < st.p:
            st.now = st.p
        self.p_sync(st)
        self.emit(f"PN = VCL({expr});")
        st.p = st.now + latency

    def lower(self, pc, op, st):
        """Emit the lower op. Returns (branch kind, target, condition var or jump var, kick var)."""
        opc, dest, ft, fs = op >> 25, (op >> 21) & 15, (op >> 16) & 31, (op >> 11) & 31
        it, is_ = ft & 15, fs & 15
        tgt = (pc + 8 + imm11(op) * 8) & self.mask
        if opc == 0x00:
            self.load_q(st, ft, dest, f"{self.vi(is_)} + {imm11(op)}")
        elif opc == 0x01:
            self.store_q(st, fs, dest, f"{self.vi(it)} + {imm11(op)}")
        elif opc == 0x04:
            self.set_vi(it, f"mw[{self.addr(f'{self.vi(is_)} + {imm11(op)}')} * 4 + {first_lane(dest)}]")
        elif opc == 0x05:
            t = self.new_tmp()
            self.emit(f"uint32_t {t} = {self.addr(f'{self.vi(is_)} + {imm11(op)}')} * 4;")
            for k in lanes(dest):
                self.emit(f"mw[{t} + {k}] = {self.vi(it)};")
        elif opc in (0x08, 0x09):
            imm = ((op >> 10) & 0x7800) | (op & 0x7FF)
            self.vi_backup(st, it)
            self.set_vi(it, f"{self.vi(is_)} {'+' if opc == 0x08 else '-'} {imm}")
        elif opc in (0x10, 0x11, 0x12, 0x13):
            imm = op & 0xFFFFFF
            self.flags_sync(st)
            if opc == 0x10:
                self.set_vi(1, f"(CLIP & 0xFFFFFF) == {u32(imm)}")
            elif opc == 0x11:
                self.emit(f"CLIP = {u32(imm)};")
            elif opc == 0x12:
                self.set_vi(1, f"(CLIP & {u32(imm)}) != 0")
            else:
                self.set_vi(1, f"((CLIP | {u32(imm)}) & 0xFFFFFF) == 0xFFFFFF")
        elif opc in (0x14, 0x15, 0x16, 0x17):
            raise Unsupported(f"status flag op at {pc:04X}")
        elif opc in (0x18, 0x1A, 0x1B):
            self.flags_sync(st)
            if not st.mdef:
                self.mac_live_in = True     # reads what the last run left
            if opc == 0x18:
                self.set_vi(it, f"(MAC & 0xFFFF) == {self.vi(is_)}")
            elif opc == 0x1A:
                self.set_vi(it, f"MAC & {self.vi(is_)}")
            else:
                self.set_vi(it, f"(MAC & 0xFFFF) | {self.vi(is_)}")
        elif opc == 0x1C:
            self.flags_sync(st)
            self.set_vi(it, "CLIP & 0xFFF")
        elif opc == 0x20:
            return ("b", tgt, None, None)
        elif opc == 0x21:
            self.set_vi(it, str((pc + 16) // 8))
            if tgt in self.native_subs:
                return ("nsub", tgt, self.native_subs[tgt], None)
            return ("b", tgt, None, None)
        elif opc in (0x24, 0x25):
            j = self.new_tmp()
            self.long_vars.add(j)
            self.emit(f"{j} = ({self.vi(is_)} * 8u) & {self.mask};")
            if opc == 0x25:
                self.set_vi(it, str((pc + 16) // 8))
            return ("jr", None, j, None)
        elif opc in (0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F):
            a, b = self.bvi(st, is_), self.bvi(st, it)
            cond = {0x28: f"{b} == {a}", 0x29: f"{b} != {a}", 0x2C: f"(int16_t){a} < 0",
                    0x2D: f"(int16_t){a} > 0", 0x2E: f"(int16_t){a} <= 0", 0x2F: f"(int16_t){a} >= 0"}[opc]
            c = self.new_tmp()
            self.long_vars.add(c)
            self.emit(f"{c} = {cond};")
            return ("cb", tgt, c, None)
        elif opc == 0x40:
            return self.lower_40(pc, op, st)
        else:
            raise Unsupported(f"lower {opc:02X} at {pc:04X}")
        return None

    def lower_40(self, pc, op, st):
        dest, ft, fs, fd = (op >> 21) & 15, (op >> 16) & 31, (op >> 11) & 31, (op >> 6) & 31
        it, is_, id_ = ft & 15, fs & 15, fd & 15
        sub = op & 0x3F
        if sub in (0x30, 0x31, 0x34, 0x35):
            self.vi_backup(st, id_)
            o = {0x30: "+", 0x31: "-", 0x34: "&", 0x35: "|"}[sub]
            self.set_vi(id_, f"{self.vi(is_)} {o} {self.vi(it)}")
            return None
        if sub == 0x32:
            self.vi_backup(st, it)
            imm5 = fd - 32 if fd & 16 else fd
            self.set_vi(it, f"{self.vi(is_)} + {imm5}")
            return None
        if sub < 0x3C:
            raise Unsupported(f"lower op {sub:02X} at {pc:04X}")
        x = ((op >> 4) & 0x7C) | (op & 3)
        fsf, ftf = (op >> 21) & 3, (op >> 23) & 3
        if x == 0x30:                                       # MOVE
            self.stall_on(st, fs, dest)
            self.vf_write(st, ft, dest, {k: self.vf(fs, k) for k in range(4)})
        elif x == 0x31:                                     # MR32
            self.stall_on(st, fs, ((dest >> 1) | (dest << 3)) & 15)
            self.vf_write(st, ft, dest, {k: self.vf(fs, (k + 1) & 3) for k in range(4)})
        elif x == 0x34:                                     # LQI
            self.load_q(st, ft, dest, self.vi(is_))
            self.vi_backup(st, is_)
            self.set_vi(is_, f"{self.vi(is_)} + 1")
        elif x == 0x35:                                     # SQI
            self.store_q(st, fs, dest, self.vi(it))
            self.vi_backup(st, it)
            self.set_vi(it, f"{self.vi(it)} + 1")
        elif x == 0x36:                                     # LQD
            self.vi_backup(st, is_)
            self.set_vi(is_, f"{self.vi(is_)} - 1")
            self.load_q(st, ft, dest, self.vi(is_))
        elif x == 0x37:                                     # SQD
            self.vi_backup(st, it)
            self.set_vi(it, f"{self.vi(it)} - 1")
            self.store_q(st, fs, dest, self.vi(it))
        elif x == 0x38:                                     # DIV
            self.stall_on(st, fs, 8 >> fsf)
            self.stall_on(st, ft, 8 >> ftf)
            self.div_issue(st, f"vu_div({self.vf(fs, fsf)}, {self.vf(ft, ftf)})", 7)
        elif x == 0x39:                                     # SQRT
            self.stall_on(st, ft, 8 >> ftf)
            self.div_issue(st, f"sqrtf(fabsf(VIN({self.vf(ft, ftf)})))", 7)
        elif x == 0x3A:                                     # RSQRT
            self.stall_on(st, fs, 8 >> fsf)
            self.stall_on(st, ft, 8 >> ftf)
            self.div_issue(st, f"vu_rsqrt({self.vf(fs, fsf)}, {self.vf(ft, ftf)})", 13)
        elif x == 0x3B:                                     # WAITQ
            if st.q is not None and st.now < st.q:
                st.now = st.q
            self.q_sync(st)
        elif x == 0x3C:                                     # MTIR
            self.stall_on(st, fs, 8 >> fsf)
            self.vi_backup(st, it)
            self.set_vi(it, f"F2U({self.vf(fs, fsf)})")
        elif x == 0x3D:                                     # MFIR
            self.vf_write(st, ft, dest, {k: f"U2F((uint32_t)(int32_t)(int16_t){self.vi(is_)})" for k in range(4)})
        elif x == 0x3E:                                     # ILWR
            self.set_vi(it, f"mw[{self.addr(self.vi(is_))} * 4 + {first_lane(dest)}]")
        elif x == 0x3F:                                     # ISWR
            t = self.new_tmp()
            self.emit(f"uint32_t {t} = {self.addr(self.vi(is_))} * 4;")
            for k in lanes(dest):
                self.emit(f"mw[{t} + {k}] = {self.vi(it)};")
        elif x in (0x40, 0x41):                             # RNEXT RGET
            if x == 0x40:
                self.emit("R = ps2_vu_rnext(R);")
            self.vf_write(st, ft, dest, {k: "U2F(R)" for k in range(4)})
        elif x == 0x42:
            self.stall_on(st, fs, 8 >> fsf)
            self.emit(f"R = 0x3F800000u | (F2U({self.vf(fs, fsf)}) & 0x7FFFFF);")
        elif x == 0x43:
            self.stall_on(st, fs, 8 >> fsf)
            self.emit(f"R = 0x3F800000u | ((R ^ F2U({self.vf(fs, fsf)})) & 0x7FFFFF);")
        elif x == 0x64:                                     # MFP
            self.p_sync(st)
            self.vf_write(st, ft, dest, {k: "P" for k in range(4)})
        elif x in (0x68, 0x69):
            self.set_vi(it, f"vif_{'top' if x == 0x68 else 'itop'}({self.unit})")
        elif x == 0x6C:                                     # XGKICK
            k = self.new_tmp()
            self.emit(f"uint32_t {k} = {self.vi(is_)};")
            return ("kick", None, None, k)
        elif x in (0x70, 0x71, 0x72, 0x73, 0x76):
            self.stall_on(st, fs, 0xF if x == 0x76 else 0xE)
            a = [f"VIN({self.vf(fs, k)})" for k in range(4)]
            sq = f"({a[0]} * {a[0]} + {a[1]} * {a[1]} + {a[2]} * {a[2]})"
            e, lat = {0x70: (sq, 11), 0x71: (f"vu_rcp0({sq})", 18), 0x72: (f"sqrtf({sq})", 18),
                      0x73: (f"vu_rsqrt0({sq})", 24), 0x76: (f"({a[0]} + {a[1]} + {a[2]} + {a[3]})", 12)}[x]
            self.efu_issue(st, e, lat)
        elif x in (0x74, 0x75):
            self.stall_on(st, fs, 0xC if x == 0x74 else 0xA)
            other = 1 if x == 0x74 else 2
            self.efu_issue(st, f"vu_eatan2(VIN({self.vf(fs, other)}), VIN({self.vf(fs, 0)}))", 54)
        elif x in (0x78, 0x79, 0x7A, 0x7C, 0x7D, 0x7E):
            self.stall_on(st, fs, 8 >> fsf)
            s = f"VIN({self.vf(fs, fsf)})"
            e, lat = {0x78: (f"vu_esqrt({s})", 12), 0x79: (f"vu_ersqrt({s})", 18), 0x7A: (f"vu_rcp0({s})", 12),
                      0x7C: (f"vu_esin({s})", 29), 0x7D: (f"vu_eatan({s})", 54), 0x7E: (f"vu_eexp({s})", 44)}[x]
            self.efu_issue(st, e, lat)
        elif x == 0x7B:                                     # WAITP
            if st.p is not None and st.now < st.p:
                st.now = st.p
            self.p_sync(st)
        elif op == 0x8000033C or x == 0x2F:
            pass
        else:
            raise Unsupported(f"lower ext {x:02X} at {pc:04X}")
        return None

    # -- blocks --

    def variant(self, pc, key):
        k = (pc, key)
        if k not in self.variants:
            n = sum(1 for p, _ in self.variants if p == pc)
            self.variants[k] = f"L{pc:04X}_{n}"
            self.work.append(k)
        return self.variants[k]

    def step(self, pc, st):
        """one instruction pair; returns the lower's control info"""
        lo, up = self.code[pc]
        self.emit(f"/* {pc:04X} */ {{")
        if not up >> 31:
            self.lower_stalls(lo, st)
        u = self.upper(pc, up, st)
        info = None
        if up >> 31:
            self.emit(f"I = U2F({u32(lo)});")
        else:
            info = self.lower(pc, lo, st)
        self.commit(pc, st, u)
        if info and info[0] == "kick":
            if self.unit == 1:
                self.emit(f"VU_XGKICK(mw, {info[3]});")
            info = None
        if st.bk and st.bk[0]:
            st.bk = (st.bk[0] - 1, st.bk[1]) if st.bk[0] > 1 else None
        st.now += 1
        self.emit("}")
        return info

    def exit_code(self, st, tpc_expr, fallback=False):
        """leave the program: drain the pipeline like the interpreter (8 cycles)"""
        st.now += 8
        self.flags_sync(st)
        for f in list(st.fq):
            self.emit(f"MAC = m_{f[2]:04X};" if f[1] == "M" else f"CLIP = c_{f[2]:04X};")
        st.fq = []
        ql = 0 if st.q is None else max(0, st.q - st.now)
        pl = 0 if st.p is None else max(0, st.p - st.now)
        if st.q is not None and (ql == 0 or fallback):
            self.emit("Q = QN;")
            ql = 0
        if st.p is not None and (pl == 0 or fallback):
            self.emit("P = PN;")
            pl = 0
        self.emit(f"tpc = {tpc_expr}; ql = {ql}; pl = {pl}; fb = {int(fallback)}; goto done;")

    def transfer(self, st, kind, target, var, fall):
        """after the delay slot: go to the successor(s)"""
        self.settle(st)
        key = st.key()
        if kind == "b":
            self.emit(f"goto {self.target(target, key)};")
        elif kind == "nsub":
            # a subroutine done in C: the registers go to the context and
            # back; when the C function declines (0) the microcode runs
            self.native_used.add(var)
            self.emit("/*SPILL*/")
            self.emit(f"if ({var}(ctx)) {{")
            self.emit("/*RELOAD*/")
            self.emit(f"goto {self.target(fall, St().key())};")
            self.emit("}")
            self.emit(f"goto {self.target(target, key)};")
        elif kind == "cb":
            self.emit(f"if ({var}) goto {self.target(target, key)};")
            self.emit(f"goto {self.target(fall, key)};")
        else:
            self.emit(f"switch ({var}) {{")
            for t in self.jr_targets:
                self.emit(f"case 0x{t:04X}: goto {self.target(t, key)};")
            self.emit(f"default: {{ tpc = {var}; ql = 0; pl = 0; fb = 1; goto done_fb; }}")
            self.emit("}")

    def target(self, pc, key):
        if pc not in self.code:
            return self.outside(pc)
        return self.variant(pc, key)

    def outside(self, pc):
        lab = f"X{pc:04X}"
        if lab not in self.outside_labels:
            self.outside_labels[lab] = pc
        return lab

    def block(self, pc, key, leaders):
        st = St.from_key(key)
        self.emit(f"{self.variants[(pc, key)]}:;")
        pend = None
        while True:
            if pc not in self.code:
                self.emit(f"goto {self.outside(pc)};")
                return
            lo, up = self.code[pc]
            info = self.step(pc, st)
            if st.end:
                if pend is None:
                    self.exit_code(st, f"0x{(pc + 8) & self.mask:04X}")
                elif pend[0] == "b":
                    self.exit_code(st, f"0x{pend[1]:04X}")
                elif pend[0] == "cb":
                    self.exit_code(st, f"{pend[2]} ? 0x{pend[1]:04X} : 0x{(pc + 8) & self.mask:04X}")
                else:
                    self.exit_code(st, pend[2])
                return
            if up & 0x40000000:
                st.end = 1
            if pend is not None:
                self.transfer(st, pend[0], pend[1], pend[2], (pc + 8) & self.mask)
                return
            if info is not None:
                pend = info
                pc = (pc + 8) & self.mask
                continue
            pc = (pc + 8) & self.mask
            if pc in leaders:
                self.settle(st)
                self.emit(f"goto {self.target(pc, st.key())};")
                return

    def compile(self, name, live=None):
        """live (VU0 routines the EE calls: (vf: {register: lanes}, vi: {numbers})): what is read after
        the routine before it is written again. A run that ends at its end then stores only that of
        what it wrote (and Q): the rest, with whatever was worked out for it alone, is the compiler's
        to drop. A run that leaves the code for the interpreter stores everything, as without."""
        self.mac_live_in = False
        self.native_used = set()
        leaders = self.leaders()
        self.outside_labels = {}
        settled = St().key()
        for e in self.entries:
            self.variant(e, settled)
        body = []
        while self.work:
            pc, key = self.work.pop()
            self.out = []
            self.block(pc, key, leaders)
            body.extend(self.out)
        out = []
        for fnn in sorted(self.native_used):
            out.append(f"int {fnn}(VuCtx *ctx);")
        out.append(f"void {name}(VuCtx *ctx, uint32_t entry)")
        out.append("{")
        out.append("    float *const mf = (float *)ctx->mem;")
        out.append("    uint32_t *const mw = ctx->mem;")
        for r in sorted(self.vf_used):
            out.append("    float " + ", ".join(f"f{r}{L[k]} = ctx->vf[{r}][{k}]" for k in range(4)) + ";")
        for n in sorted(self.vi_used):
            out.append(f"    uint32_t v{n} = ctx->vi[{n}];")
        out.append("    float acx = ctx->acc[0], acy = ctx->acc[1], acz = ctx->acc[2], acw = ctx->acc[3];")
        out.append("    float Q = *ctx->q, QN = 0, P = *ctx->p, PN = 0, I = *ctx->i;")
        out.append("    uint32_t R = *ctx->r, MAC = ctx->mac, CLIP = ctx->clip, BK = 0, tpc;")
        out.append("    int ql, pl, fb;")
        for v in sorted(self.long_vars, key=lambda v: int(v[1:])):
            out.append(f"    uint32_t {v};")
        for v in sorted(self.flag_vars):
            out.append(f"    uint32_t {v} = 0;")
        out.append("    switch (entry) {")
        for e in self.entries:
            out.append(f"    case 0x{e:04X}: goto {self.variants[(e, settled)]};")
        out.append("    default: tpc = entry; ql = 0; pl = 0; fb = 1; goto done_fb;")
        out.append("    }")
        if self.native_used:
            spill, reload = [], []
            for r in sorted(self.vf_used):
                spill.append(" ".join(f"ctx->vf[{r}][{k}] = f{r}{L[k]};" for k in range(4)))
                reload.append(" ".join(f"f{r}{L[k]} = ctx->vf[{r}][{k}];" for k in range(4)))
            for n in sorted(self.vi_used):
                spill.append(f"ctx->vi[{n}] = v{n};")
                reload.append(f"v{n} = ctx->vi[{n}];")
            spill.append("ctx->acc[0] = acx; ctx->acc[1] = acy; ctx->acc[2] = acz; ctx->acc[3] = acw;")
            spill.append("*ctx->q = Q; *ctx->p = P; *ctx->i = I; *ctx->r = R; ctx->mac = MAC; ctx->clip = CLIP;")
            reload.append("acx = ctx->acc[0]; acy = ctx->acc[1]; acz = ctx->acc[2]; acw = ctx->acc[3];")
            reload.append("Q = *ctx->q; P = *ctx->p; I = *ctx->i; R = *ctx->r; MAC = ctx->mac; CLIP = ctx->clip;")
            sub = []
            for line in body:
                if line == "/*SPILL*/":
                    sub.extend(spill)
                elif line == "/*RELOAD*/":
                    sub.extend("    " + x for x in reload)
                else:
                    sub.append(line)
            body = sub
        out.extend("    " + s for s in body)
        for lab, pc in sorted(self.outside_labels.items()):
            out.append(f"{lab}: tpc = 0x{pc:04X}; ql = 0; pl = 0; fb = 1; goto done_fb;")
        out.append("done_fb:")
        out.append("    fb = 1;")
        if live is not None:
            for r in sorted(self.vf_written):
                out.append("    " + " ".join(f"ctx->vf[{r}][{k}] = f{r}{L[k]};" for k in range(4)))
            for n in sorted(self.vi_written):
                out.append(f"    ctx->vi[{n}] = v{n};")
            out.append("    ctx->acc[0] = acx; ctx->acc[1] = acy; ctx->acc[2] = acz; ctx->acc[3] = acw;")
            out.append("    *ctx->q = Q; *ctx->p = P; *ctx->i = I; *ctx->r = R;")
            out.append("    ctx->qn = QN; ctx->pn = PN; ctx->q_left = ql; ctx->p_left = pl;")
            out.append("    ctx->mac = MAC; ctx->clip = CLIP; ctx->tpc = tpc; ctx->fallback = fb;")
            out.append("    return;")
            out.append("done:")
            lvf, lvi = live
            self.dead = [0, 0, 0, 0]
            self.dead_vi = 0
            for r in sorted(self.vf_written):
                keep = [k for k in sorted(self.vf_wl.get(r, ())) if k in lvf.get(r, ())]
                for k in sorted(self.vf_wl.get(r, ())):
                    if k not in keep:
                        self.dead[k] |= 1 << r
                if keep:
                    out.append("    " + " ".join(f"ctx->vf[{r}][{k}] = f{r}{L[k]};" for k in keep))
            for n in sorted(self.vi_written):
                if n in lvi:
                    out.append(f"    ctx->vi[{n}] = v{n};")
                else:
                    self.dead_vi |= 1 << n
            out.append("    *ctx->q = Q;")
            out.append("    ctx->qn = QN; ctx->pn = PN; ctx->q_left = ql; ctx->p_left = pl; ctx->tpc = tpc; ctx->fallback = fb;")
            out.append("}")
            return out
        out.append("done:")
        for r in sorted(self.vf_written):
            out.append("    " + " ".join(f"ctx->vf[{r}][{k}] = f{r}{L[k]};" for k in range(4)))
        for n in sorted(self.vi_written):
            out.append(f"    ctx->vi[{n}] = v{n};")
        out.append("    ctx->acc[0] = acx; ctx->acc[1] = acy; ctx->acc[2] = acz; ctx->acc[3] = acw;")
        out.append("    *ctx->q = Q; *ctx->p = P; *ctx->i = I; *ctx->r = R;")
        out.append("    ctx->qn = QN; ctx->pn = PN; ctx->q_left = ql; ctx->p_left = pl;")
        # VU1's MAC flags are not read by the EE: when no run reads what the
        # last one left, the flags only matter inside a run and the compiler
        # drops the ones nothing reads
        if self.unit == 1 and not self.mac_live_in:
            out.append("    ctx->clip = CLIP; ctx->tpc = tpc; ctx->fallback = fb;   /* MAC: not read across runs */")
        else:
            out.append("    ctx->mac = MAC; ctx->clip = CLIP; ctx->tpc = tpc; ctx->fallback = fb;")
        out.append("}")
        return out

def ranges(pcs):
    """[(addr, count)] runs of consecutive instruction addresses"""
    out = []
    for pc in sorted(pcs):
        if out and out[-1][0] + out[-1][1] * 8 == pc:
            out[-1][1] += 1
        else:
            out.append([pc, 1])
    return out


def vcallms_targets(img, text_ranges):
    """micro addresses the EE code starts VU0 at (VCALLMS immediates)"""
    out = set()
    for lo, hi in text_ranges:
        for a in range(lo, hi, 4):
            w = img.u32(a)
            if (w & 0xFE00003F) == 0x4A000038:
                out.add(((w >> 6) & 0x7FFF) * 8)
    return out


def read_vu0_live(path):
    """{entry: ({register: lanes}, {vi numbers})} from the table work/vu0live.py prints: a line
    "@EEEE (...): vf2.xyz vf5 vi1 Q" an entry (the registers read after a call of it)."""
    import re
    live = {}
    for line in open(path):
        m = re.match(r"@([0-9A-Fa-f]{4}) \(.*?\): (.*)", line)
        if not m:
            continue
        vf, vi = {}, set()
        for tok in m.group(2).split():
            if tok.startswith("vf"):
                r, _, lanes = tok[2:].partition(".")
                vf.setdefault(int(r), set()).update("xyzw".index(c) for c in (lanes or "xyzw"))
            elif tok.startswith("vi"):
                vi.add(int(tok[2:]))
        live[int(m.group(1), 16)] = (vf, vi)
    return live


def compile_all(img, rng, vu0, hints, out_dir, text_ranges=(), native_subs=None, vu0_live=None):
    """compile the images in rng (chain address range): one file per program
    (out_dir/vu<unit>_<chain>.c) and the table, out_dir/vu_progs.c. VU0 images
    also get the VCALLMS targets in text_ranges as entries. vu0_live (read_vu0_live):
    each VU0 routine in it is compiled a second time, storing only what is read after it."""
    calls = vcallms_targets(img, text_ranges)
    vu0_live = vu0_live or {}
    head = ["/* Generated by tools/vu/vurecomp.py -- do not edit. */", '#include "vu_rec.h"', ""]
    out = list(head)
    table = []
    nvar = 0
    for chain, code in find_images(img, *rng):
        unit = 0 if chain in vu0 else 1
        entries = {0} | {pc + 16 for pc, (lo, up) in code.items() if up & 0x40000000 and pc + 16 in code}
        entries |= set(hints.get(chain, []))
        if unit == 0:
            entries |= calls
        entries = sorted(e for e in entries if e in code)
        p = Prog(unit, chain, code, entries, native_subs=(native_subs or {}).get(chain))
        name = f"vu{unit}_{chain:06X}"
        lean = {}                     # entry -> (dead lanes by lane, dead vi): compiled a second time
        try:
            if unit == 0 and len(entries) > 1:
                # VU0 images are libraries of short routines (VCALLMS targets):
                # a function per entry loads and stores only the registers
                # that entry's code uses, then a dispatcher
                fn = []
                for e in entries:
                    pe = Prog(unit, chain, code, [e])
                    fe = pe.compile(f"{name}_{e:04X}")
                    fe[0] = "static " + fe[0]
                    fn += fe + [""]
                    nvar += len(pe.variants)
                    if e in vu0_live:
                        pl = Prog(unit, chain, code, [e])
                        fl = pl.compile(f"{name}_{e:04X}_l", live=vu0_live[e])
                        fl[0] = "static " + fl[0]
                        fn += fl + [""]
                        lean[e] = (pl.dead, pl.dead_vi)
                for suffix in ("", "_l") if lean else ("",):
                    fn.append(f"void {name}{suffix}(VuCtx *ctx, uint32_t entry)")
                    fn.append("{")
                    fn.append("    switch (entry) {")
                    for e in entries:
                        s = suffix if e in lean else ""
                        fn.append(f"    case 0x{e:04X}: {name}_{e:04X}{s}(ctx, entry); return;")
                    fn.append("    default: ctx->tpc = entry; ctx->q_left = ctx->p_left = 0; ctx->fallback = 1; return;")
                    fn.append("    }")
                    fn.append("}")
            else:
                fn = p.compile(name)
                nvar += len(p.variants)
        except Unsupported as e:
            print(f"VU image {chain:06X}: not compiled: {e}")
            continue
        with open(os.path.join(out_dir, f"vu{unit}_{chain:06X}.c"), "w", newline="\n") as f:
            f.write("\n".join(head + fn) + "\n")
        out.append(f"void {name}(VuCtx *ctx, uint32_t entry);")
        if lean:
            out.append(f"void {name}_l(VuCtx *ctx, uint32_t entry);")
        size = max(code) + 8
        words = []
        for pc in range(0, size, 8):
            words.extend(code.get(pc, (0, 0)))
        out.append(f"static const uint32_t {name}_code[] = {{")
        for k in range(0, len(words), 8):
            out.append("    " + ", ".join(f"0x{w:08X}" for w in words[k:k + 8]) + ",")
        out.append("};")
        ent = []
        for e in entries:
            rs = ranges(p.reach(e))
            out.append(f"static const VuRange {name}_r{e:04X}[] = {{" +
                       ", ".join(f"{{0x{r[0]:04X}, {r[1]}}}" for r in rs) + "};")
            d, dvi = lean.get(e, ([0, 0, 0, 0], 0))
            ent.append(f"{{0x{e:04X}, {len(rs)}, {name}_r{e:04X}, {{0x{d[0]:08X}, 0x{d[1]:08X}, 0x{d[2]:08X}, 0x{d[3]:08X}}}, "
                       f"0x{dvi:04X}, {1 if e in lean else 0}}}")
        out.append(f"static const VuEntry {name}_entries[] = {{{', '.join(ent)}}};")
        out.append("")
        table.append(f"    {{{unit}, 0x{chain:06X}, {name}_code, {len(entries)}, {name}_entries, {name}, "
                     f"{name + '_l' if lean else '0'}}},")
    out.append("const VuProg g_vu_progs[] = {")
    out.extend(table)
    out.append("};")
    out.append(f"const int g_vu_nprogs = {len(table)};")
    with open(os.path.join(out_dir, "vu_progs.c"), "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    print(f"{len(table)} VU microprograms, {nvar} blocks")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("out_dir")
    ap.add_argument("--range", nargs=2, required=True, type=lambda s: int(s, 16))
    ap.add_argument("--vu0", default="", help="comma-separated chain addresses of VU0 images")
    ap.add_argument("--entry", action="append", default=[], help="CHAIN:ADDR extra entry point")
    ap.add_argument("--text", nargs=2, action="append", default=[], type=lambda s: int(s, 16),
                    help="EE code range to scan for VCALLMS targets")
    a = ap.parse_args()
    hints = {}
    for e in a.entry:
        c, ad = e.split(":")
        hints.setdefault(int(c, 16), []).append(int(ad, 16))
    compile_all(Image(a.elf), a.range, {int(x, 16) for x in a.vu0.split(",") if x}, hints, a.out_dir, a.text)


if __name__ == "__main__":
    main()
