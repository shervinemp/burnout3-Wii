"""Per-instruction translation of R5900 instructions to C statements.

Registers live in function-local variables named after a *resource*:

    ("r", n, lane)   GPR n, 32-bit lane 0..3          -> r4, r4_1, r4_2, r4_3
    ("hi", lane) / ("lo", lane)                        -> hi0..hi3 / lo0..lo3
    ("sa",)                                            -> sa
    ("f", n)         FPU register (float)              -> f3
    ("facc",) ("fcc",)                                 -> facc, fcc
    ("vf", n, comp)  VU0 vector register lane          -> vf3x
    ("vacc", comp) ("vq",) ("vI",) ("vp",) ("vr",)     -> vaccx, vq, vI, vp, vr
    ("vi", n)        VU0 integer register              -> vi3
    ("c0", n)        COP0 register                     -> c0_12

The function emitter loads resources from the context at entry, stores the
dirty ones at every synchronisation point (call, return, syscall), and reloads
after calls.

Lift.lift(insn) returns (statements, reads, writes, wide_effect). wide_effect
maps a written GPR to how its upper 32 bits relate to its lower 32 bits after
the instruction: "sext", "wide", ("copy", n) or ("join", a, b). The function
emitter uses that to choose 32- or 64-bit comparisons.
"""

COMP = "xyzw"
FLOAT_RES = {"f", "facc", "vf", "vacc", "vq", "vI", "vp"}
IO_SEGMENT = 0x1


def ee_io_addr(a):
    return (a >> 28) == IO_SEGMENT


def iop_io_addr(a):
    """IOP hardware and scratchpad: 0x1F800000-0x1FFFFFFF in any segment."""
    return (a & 0x1FFFFFFF) >= 0x1F800000


IO_TEST = [ee_io_addr]          # replaced by the driver when recompiling IOP modules


def res_name(r):
    k = r[0]
    if k == "r":
        return f"r{r[1]}" if r[2] == 0 else f"r{r[1]}_{r[2]}"
    if k in ("hi", "lo"):
        return f"{k}{r[1]}"
    if k == "f":
        return f"f{r[1]}"
    if k == "vf":
        return f"vf{r[1]}{r[2]}"
    if k == "vacc":
        return f"vacc{r[1]}"
    if k == "vi":
        return f"vi{r[1]}"
    if k == "c0":
        return f"c0_{r[1]}"
    return k


def res_ctx(r):
    k = r[0]
    if k == "r":
        return f"c->r[{r[1]}][{r[2]}]"
    if k in ("hi", "lo"):
        return f"c->{k}[{r[1]}]"
    if k == "f":
        return f"c->f[{r[1]}]"
    if k == "vf":
        return f"c->vf[{r[1]}][{COMP.index(r[2])}]"
    if k == "vacc":
        return f"c->vacc[{COMP.index(r[1])}]"
    if k == "vi":
        return f"c->vi[{r[1]}]"
    if k == "c0":
        return f"c->cop0[{r[1]}]"
    return f"c->{k}"


def res_type(r):
    if r[0] in FLOAT_RES:
        return "float"
    return "uint32_t"


class LiftError(Exception):
    pass


class Lift:
    """Translates one instruction. `consts` maps GPR -> known constant value
    (from lui/ori/addiu earlier in the same basic block) for I/O detection."""

    def __init__(self):
        self.reads = set()
        self.writes = set()
        self.out = []
        self.wide = {}

    # -- resource helpers --------------------------------------------------------

    def g(self, n, lane=0):
        if n == 0:
            return "0u"
        r = ("r", n, lane)
        self.reads.add(r)
        return res_name(r)

    def setg(self, n, lane, expr):
        if n == 0:
            return
        r = ("r", n, lane)
        self.writes.add(r)
        self.out.append(f"{res_name(r)} = {expr};")

    def w32(self, n, expr, kind="sext"):
        """32-bit result, sign-extended into lane 1."""
        if n == 0:
            return
        self.setg(n, 0, f"(uint32_t)({expr})")
        self.setg(n, 1, f"SX(r{n})")
        self.wide[n] = kind

    def w32z(self, n, expr):
        """32-bit result, zero-extended into lane 1."""
        if n == 0:
            return
        self.setg(n, 0, f"(uint32_t)({expr})")
        self.setg(n, 1, "0u")
        self.wide[n] = "wide"

    def j64(self, n):
        if n == 0:
            return "0ull"
        return f"J64({self.g(n, 0)}, {self.g(n, 1)})"

    def w64(self, n, expr, kind="wide"):
        if n == 0:
            return
        self.out.append(f"{{ uint64_t _t = (uint64_t)({expr}); r{n} = (uint32_t)_t; r{n}_1 = (uint32_t)(_t >> 32); }}")
        self.writes.add(("r", n, 0))
        self.writes.add(("r", n, 1))
        self.wide[n] = kind

    def g128(self, n):
        return [self.g(n, k) for k in range(4)]

    def w128(self, n, lanes):
        if n == 0:
            return
        for k in range(4):
            self.setg(n, k, lanes[k])
        self.wide[n] = "wide"

    def res(self, r, write=False):
        if write:
            self.writes.add(r)
        else:
            self.reads.add(r)
        return res_name(r)

    def f(self, n):
        return self.res(("f", n))

    def setf(self, n, expr):
        self.out.append(f"{self.res(('f', n), True)} = {expr};")

    def fchk(self, expr):
        """Float arithmetic result, checked for Inf/NaN in debug builds (the EE
        FPU and VUs never produce them; see PS2_FCHK)."""
        return f"PS2_FCHK({expr}, 0x{self.cur_pc:08X}u)"

    def vf(self, n, comp):
        if n == 0:
            return "1.0f" if comp == "w" else "0.0f"
        return self.res(("vf", n, comp))

    def setvf(self, n, comp, expr):
        if n == 0:
            return None
        return f"{self.res(('vf', n, comp), True)} = {expr};"

    def vi(self, n):
        if n == 0:
            return "0u"
        return self.res(("vi", n))

    def setvi(self, n, expr):
        if n == 0:
            return
        self.out.append(f"{self.res(('vi', n), True)} = (uint32_t)({expr}) & 0xFFFFu;")

    # -- addresses -----------------------------------------------------------------

    def addr(self, i):
        base = self.g(i.rs)
        if i.simm == 0:
            return f"({base})"
        return f"({base} + {i.simm}u)" if i.simm > 0 else f"({base} - {-i.simm}u)"

    # -- main entry ------------------------------------------------------------------

    def lift(self, i, consts):
        self.reads = set()
        self.writes = set()
        self.out = []
        self.wide = {}
        self.consts = consts
        self.cur_pc = i.pc
        name = "op_" + i.op.replace(".", "_")
        fn = getattr(self, name, None)
        if fn is None:
            raise LiftError(f"no translation for {i.op} at {i.pc:08X}")
        fn(i)
        return self.out, self.reads, self.writes, self.wide

    def const_addr(self, i):
        """The effective address when the base register is a known constant."""
        if i.rs == 0:
            return i.simm & 0xFFFFFFFF
        v = self.consts.get(i.rs)
        if v is None:
            return None
        return (v + i.simm) & 0xFFFFFFFF

    def io(self, i):
        a = self.const_addr(i)
        if a is not None and IO_TEST[0](a):
            return a
        return None

    # ===== integer ALU ===============================================================

    def op_nop(self, i):
        pass

    def op_sync(self, i):
        pass

    op_cache = op_pref = op_sync

    def op_addiu(self, i):
        self.w32(i.rt, f"{self.g(i.rs)} + {i.simm & 0xFFFFFFFF}u")

    op_addi = op_addiu

    def op_daddiu(self, i):
        if i.simm == 0:
            self.op_move(i.rt, i.rs)
            return
        self.w64(i.rt, f"{self.j64(i.rs)} + {i.simm}ll")

    op_daddi = op_daddiu

    def op_addu(self, i):
        self.w32(i.rd, f"{self.g(i.rs)} + {self.g(i.rt)}")

    op_add = op_addu

    def op_subu(self, i):
        self.w32(i.rd, f"{self.g(i.rs)} - {self.g(i.rt)}")

    op_sub = op_subu

    def op_move(self, d, s):
        if d == 0:
            return
        if s == 0:
            self.setg(d, 0, "0u")
            self.setg(d, 1, "0u")
            self.wide[d] = "sext"
            return
        self.setg(d, 0, self.g(s, 0))
        self.setg(d, 1, self.g(s, 1))
        self.wide[d] = ("copy", s)

    def op_daddu(self, i):
        if i.rt == 0:
            self.op_move(i.rd, i.rs)
        elif i.rs == 0:
            self.op_move(i.rd, i.rt)
        else:
            self.w64(i.rd, f"{self.j64(i.rs)} + {self.j64(i.rt)}")

    op_dadd = op_daddu

    def op_dsubu(self, i):
        if i.rt == 0:
            self.op_move(i.rd, i.rs)
        else:
            self.w64(i.rd, f"{self.j64(i.rs)} - {self.j64(i.rt)}")

    op_dsub = op_dsubu

    def logic64(self, i, op, invert=False):
        d = i.rd
        if d == 0:
            return
        if op == "|" and i.rt == 0 and not invert:
            self.op_move(d, i.rs)
            return
        if op == "|" and i.rs == 0 and not invert:
            self.op_move(d, i.rt)
            return
        a0, b0 = self.g(i.rs, 0), self.g(i.rt, 0)
        a1, b1 = self.g(i.rs, 1), self.g(i.rt, 1)
        e0 = f"({a0} {op} {b0})"
        e1 = f"({a1} {op} {b1})"
        if invert:
            e0, e1 = f"~{e0}", f"~{e1}"
        self.out.append(f"{{ uint32_t _l = {e0}, _h = {e1}; r{d} = _l; r{d}_1 = _h; }}")
        self.writes.add(("r", d, 0))
        self.writes.add(("r", d, 1))
        self.wide[d] = ("join", i.rs, i.rt)

    def op_and(self, i):
        self.logic64(i, "&")

    def op_or(self, i):
        self.logic64(i, "|")

    def op_xor(self, i):
        self.logic64(i, "^")

    def op_nor(self, i):
        self.logic64(i, "|", invert=True)

    def op_andi(self, i):
        if i.rt == 0:
            return
        self.setg(i.rt, 0, f"{self.g(i.rs)} & {i.imm}u")
        self.setg(i.rt, 1, "0u")
        self.wide[i.rt] = "sext"

    def op_ori(self, i):
        if i.rt == 0:
            return
        if i.rs == 0:
            self.setg(i.rt, 0, f"{i.imm}u")
            self.setg(i.rt, 1, "0u")
            self.wide[i.rt] = "sext"
            return
        self.out.append(f"{{ uint32_t _l = {self.g(i.rs)} | {i.imm}u, _h = {self.g(i.rs, 1)}; "
                        f"r{i.rt} = _l; r{i.rt}_1 = _h; }}")
        self.writes.add(("r", i.rt, 0))
        self.writes.add(("r", i.rt, 1))
        self.wide[i.rt] = ("copy", i.rs)

    def op_xori(self, i):
        if i.rt == 0:
            return
        self.out.append(f"{{ uint32_t _l = {self.g(i.rs)} ^ {i.imm}u, _h = {self.g(i.rs, 1)}; "
                        f"r{i.rt} = _l; r{i.rt}_1 = _h; }}")
        self.writes.add(("r", i.rt, 0))
        self.writes.add(("r", i.rt, 1))
        self.wide[i.rt] = ("copy", i.rs)

    def op_lui(self, i):
        self.w32(i.rt, f"{(i.imm << 16) & 0xFFFFFFFF}u")

    # comparisons: the function emitter supplies `wide` so these pick 32/64 bits
    def cmp_operands(self, a, b, wide):
        if wide:
            return f"(int64_t){self.j64(a)}", f"(int64_t){self.j64(b)}", \
                   f"{self.j64(a)}", f"{self.j64(b)}"
        return f"(int32_t){self.g(a)}", f"(int32_t){self.g(b)}", self.g(a), self.g(b)

    def op_slt(self, i):
        sa, sb, _, _ = self.cmp_operands(i.rs, i.rt, self.is_wide(i.rs, i.rt))
        self.w32(i.rd, f"{sa} < {sb}")

    def op_sltu(self, i):
        _, _, ua, ub = self.cmp_operands(i.rs, i.rt, self.is_wide(i.rs, i.rt))
        self.w32(i.rd, f"{ua} < {ub}")

    def op_slti(self, i):
        if self.is_wide(i.rs):
            self.w32(i.rt, f"(int64_t){self.j64(i.rs)} < {i.simm}ll")
        else:
            self.w32(i.rt, f"(int32_t){self.g(i.rs)} < {i.simm}")

    def op_sltiu(self, i):
        if self.is_wide(i.rs):
            self.w32(i.rt, f"{self.j64(i.rs)} < (uint64_t){i.simm}ll")
        else:
            self.w32(i.rt, f"{self.g(i.rs)} < {i.simm & 0xFFFFFFFF}u")

    def is_wide(self, *regs):
        return any(self.wide_in(r) for r in regs if r != 0)

    def wide_in(self, r):
        return r in self.wide_regs

    # shifts
    def op_sll(self, i):
        self.w32(i.rd, f"{self.g(i.rt)} << {i.sa}" if i.sa else self.g(i.rt))

    def op_srl(self, i):
        self.w32(i.rd, f"{self.g(i.rt)} >> {i.sa}" if i.sa else self.g(i.rt))

    def op_sra(self, i):
        self.w32(i.rd, f"(int32_t){self.g(i.rt)} >> {i.sa}" if i.sa else self.g(i.rt))

    def op_sllv(self, i):
        self.w32(i.rd, f"{self.g(i.rt)} << ({self.g(i.rs)} & 31)")

    def op_srlv(self, i):
        self.w32(i.rd, f"{self.g(i.rt)} >> ({self.g(i.rs)} & 31)")

    def op_srav(self, i):
        self.w32(i.rd, f"(int32_t){self.g(i.rt)} >> ({self.g(i.rs)} & 31)")

    def op_dsll(self, i):
        self.w64(i.rd, f"{self.j64(i.rt)} << {i.sa}")

    def op_dsrl(self, i):
        self.w64(i.rd, f"{self.j64(i.rt)} >> {i.sa}")

    def op_dsra(self, i):
        self.w64(i.rd, f"(int64_t){self.j64(i.rt)} >> {i.sa}")

    def op_dsll32(self, i):
        if i.rd == 0:
            return
        self.setg(i.rd, 1, f"{self.g(i.rt, 0)} << {i.sa}" if i.sa else self.g(i.rt, 0))
        self.setg(i.rd, 0, "0u")
        self.wide[i.rd] = "wide"

    def op_dsrl32(self, i):
        if i.rd == 0:
            return
        e = f"{self.g(i.rt, 1)} >> {i.sa}" if i.sa else self.g(i.rt, 1)
        self.out.append(f"r{i.rd} = {e};")
        self.writes.add(("r", i.rd, 0))
        self.setg(i.rd, 1, "0u")
        self.wide[i.rd] = "sext" if i.sa else "wide"

    def op_dsra32(self, i):
        if i.rd == 0:
            return
        e = f"(uint32_t)((int32_t){self.g(i.rt, 1)} >> {i.sa})" if i.sa else self.g(i.rt, 1)
        self.out.append(f"r{i.rd} = {e};")
        self.writes.add(("r", i.rd, 0))
        self.setg(i.rd, 1, f"SX(r{i.rd})")
        self.wide[i.rd] = "sext"

    def op_dsllv(self, i):
        self.w64(i.rd, f"{self.j64(i.rt)} << ({self.g(i.rs)} & 63)")

    def op_dsrlv(self, i):
        self.w64(i.rd, f"{self.j64(i.rt)} >> ({self.g(i.rs)} & 63)")

    def op_dsrav(self, i):
        self.w64(i.rd, f"(int64_t){self.j64(i.rt)} >> ({self.g(i.rs)} & 63)")

    def op_movz(self, i):
        self.cmov(i, "==")

    def op_movn(self, i):
        self.cmov(i, "!=")

    def cmov(self, i, rel):
        """movz/movn: if (rt REL 0) rd = rs (64-bit move)."""
        if i.rd == 0:
            return
        if self.is_wide(i.rt):
            cond = f"{self.j64(i.rt)} {rel} 0"
        else:
            cond = f"{self.g(i.rt)} {rel} 0"
        a0, a1 = self.g(i.rs, 0), self.g(i.rs, 1)
        self.g(i.rd, 0), self.g(i.rd, 1)          # conditional write: old value stays live
        self.out.append(f"if ({cond}) {{ uint32_t _l = {a0}, _h = {a1}; r{i.rd} = _l; r{i.rd}_1 = _h; }}")
        self.writes.add(("r", i.rd, 0))
        self.writes.add(("r", i.rd, 1))
        self.wide[i.rd] = ("join", i.rd, i.rs)

    # HI/LO
    def hilo(self, base):
        return [self.res(("hi", base)), self.res(("hi", base + 1)),
                self.res(("lo", base)), self.res(("lo", base + 1))]

    def set_hilo(self, base, hi_expr, lo_expr):
        hi = res_name(("hi", base))
        lo = res_name(("lo", base))
        for r in (("hi", base), ("hi", base + 1), ("lo", base), ("lo", base + 1)):
            self.writes.add(r)
        self.out.append(f"{lo} = (uint32_t)({lo_expr}); {res_name(('lo', base + 1))} = SX({lo});")
        self.out.append(f"{hi} = (uint32_t)({hi_expr}); {res_name(('hi', base + 1))} = SX({hi});")

    def mult(self, i, base, signed, accumulate=False):
        a, b = self.g(i.rs), self.g(i.rt)
        if signed:
            prod = f"(int64_t)(int32_t){a} * (int32_t){b}"
        else:
            prod = f"(uint64_t){a} * {b}"
        if accumulate:
            hi, lo = res_name(("hi", base)), res_name(("lo", base))
            self.reads.add(("hi", base))
            self.reads.add(("lo", base))
            prod = f"(uint64_t)J64({lo}, {hi}) + (uint64_t)({prod})"
        self.out.append(f"{{ uint64_t _p = (uint64_t)({prod});")
        self.set_hilo(base, "_p >> 32", "_p")
        self.out.append("}")
        if i.rd:
            self.w32(i.rd, res_name(("lo", base)))

    def op_mult(self, i):
        self.mult(i, 0, True)

    def op_multu(self, i):
        self.mult(i, 0, False)

    def op_mult1(self, i):
        self.mult(i, 2, True)

    def op_multu1(self, i):
        self.mult(i, 2, False)

    def op_madd(self, i):
        self.mult(i, 0, True, True)

    def op_maddu(self, i):
        self.mult(i, 0, False, True)

    def op_madd1(self, i):
        self.mult(i, 2, True, True)

    def op_maddu1(self, i):
        self.mult(i, 2, False, True)

    def div(self, i, base, signed):
        a, b = self.g(i.rs), self.g(i.rt)
        fn = "ps2_div" if signed else "ps2_divu"
        self.out.append(f"{{ uint32_t _q, _r; {fn}({a}, {b}, &_q, &_r);")
        self.set_hilo(base, "_r", "_q")
        self.out.append("}")

    def op_div(self, i):
        self.div(i, 0, True)

    def op_divu(self, i):
        self.div(i, 0, False)

    def op_div1(self, i):
        self.div(i, 2, True)

    def op_divu1(self, i):
        self.div(i, 2, False)

    def mfhl(self, i, which, base):
        if i.rd == 0:
            return
        self.setg(i.rd, 0, self.res((which, base)))
        self.setg(i.rd, 1, self.res((which, base + 1)))
        self.wide[i.rd] = "sext"

    def mthl(self, i, which, base):
        self.out.append(f"{self.res((which, base), True)} = {self.g(i.rs, 0)};")
        self.out.append(f"{self.res((which, base + 1), True)} = {self.g(i.rs, 1)};")

    def op_mfhi(self, i):
        self.mfhl(i, "hi", 0)

    def op_mflo(self, i):
        self.mfhl(i, "lo", 0)

    def op_mfhi1(self, i):
        self.mfhl(i, "hi", 2)

    def op_mflo1(self, i):
        self.mfhl(i, "lo", 2)

    def op_mthi(self, i):
        self.mthl(i, "hi", 0)

    def op_mtlo(self, i):
        self.mthl(i, "lo", 0)

    def op_mthi1(self, i):
        self.mthl(i, "hi", 2)

    def op_mtlo1(self, i):
        self.mthl(i, "lo", 2)

    def op_mfsa(self, i):
        self.w32z(i.rd, self.res(("sa",)))

    def op_mtsa(self, i):
        self.out.append(f"{self.res(('sa',), True)} = {self.g(i.rs)};")

    def op_mtsab(self, i):
        self.out.append(f"{self.res(('sa',), True)} = (({self.g(i.rs)} & 15) ^ ({i.imm} & 15)) << 3;")

    def op_mtsah(self, i):
        self.out.append(f"{self.res(('sa',), True)} = (({self.g(i.rs)} & 7) ^ ({i.imm} & 7)) << 4;")

    def op_plzcw(self, i):
        if i.rd == 0:
            return
        self.setg(i.rd, 0, f"ps2_plzcw({self.g(i.rs, 0)})")
        self.setg(i.rd, 1, f"ps2_plzcw({self.g(i.rs, 1)})")
        self.wide[i.rd] = "wide"

    # ===== traps / system =============================================================

    def op_break(self, i):
        self.out.append(f"ps2_break(c, 0x{i.pc:08X}u, {(i.word >> 6) & 0xFFFFF}u);")

    def trap(self, i, cond):
        self.out.append(f"if ({cond}) ps2_trap(c, 0x{i.pc:08X}u);")

    def op_tge(self, i):
        self.trap(i, f"(int32_t){self.g(i.rs)} >= (int32_t){self.g(i.rt)}")

    def op_tgeu(self, i):
        self.trap(i, f"{self.g(i.rs)} >= {self.g(i.rt)}")

    def op_tlt(self, i):
        self.trap(i, f"(int32_t){self.g(i.rs)} < (int32_t){self.g(i.rt)}")

    def op_tltu(self, i):
        self.trap(i, f"{self.g(i.rs)} < {self.g(i.rt)}")

    def op_teq(self, i):
        self.trap(i, f"{self.g(i.rs)} == {self.g(i.rt)}")

    def op_tne(self, i):
        self.trap(i, f"{self.g(i.rs)} != {self.g(i.rt)}")

    def op_tgei(self, i):
        self.trap(i, f"(int32_t){self.g(i.rs)} >= {i.simm}")

    def op_tgeiu(self, i):
        self.trap(i, f"{self.g(i.rs)} >= {i.simm & 0xFFFFFFFF}u")

    def op_tlti(self, i):
        self.trap(i, f"(int32_t){self.g(i.rs)} < {i.simm}")

    def op_tltiu(self, i):
        self.trap(i, f"{self.g(i.rs)} < {i.simm & 0xFFFFFFFF}u")

    def op_teqi(self, i):
        self.trap(i, f"(int32_t){self.g(i.rs)} == {i.simm}")

    def op_tnei(self, i):
        self.trap(i, f"(int32_t){self.g(i.rs)} != {i.simm}")

    # COP0
    def op_mfc0(self, i):
        if i.rd == 9:
            self.w32(i.rt, "ps2_cop0_count(c)")
        else:
            self.w32(i.rt, self.res(("c0", i.rd)))

    def op_mtc0(self, i):
        self.out.append(f"{self.res(('c0', i.rd), True)} = {self.g(i.rt)};")
        if i.rd == 12:
            self.out.append(f"ps2_cop0_status_written(c, c0_12);")

    def op_ei(self, i):
        self.out.append(f"{self.res(('c0', 12), True)} = {self.res(('c0', 12))} | 0x10000u;")
        self.out.append("ps2_cop0_status_written(c, c0_12);")

    def op_di(self, i):
        self.out.append(f"{self.res(('c0', 12), True)} = {self.res(('c0', 12))} & ~0x10000u;")
        self.out.append("ps2_cop0_status_written(c, c0_12);")

    op_tlbr = op_tlbwi = op_tlbwr = op_tlbp = op_nop

    # ===== loads / stores ===============================================================

    def io_or(self, i, normal, io_fmt):
        a = self.io(i)
        if a is not None:
            return io_fmt.format(a=f"0x{a:08X}u")
        return normal

    def op_lb(self, i):
        self.w32(i.rt, f"(int8_t)LB({self.addr(i)})")

    def op_lbu(self, i):
        if i.rt == 0:
            return
        self.setg(i.rt, 0, f"LB({self.addr(i)})")
        self.setg(i.rt, 1, "0u")
        self.wide[i.rt] = "sext"

    def op_lh(self, i):
        self.w32(i.rt, self.io_or(i, f"(int16_t)LH({self.addr(i)})", "(int16_t)io_read16(c, {a})"))

    def op_lhu(self, i):
        if i.rt == 0:
            return
        self.setg(i.rt, 0, self.io_or(i, f"LH({self.addr(i)})", "io_read16(c, {a})"))
        self.setg(i.rt, 1, "0u")
        self.wide[i.rt] = "sext"

    def op_lw(self, i):
        self.w32(i.rt, self.io_or(i, f"LW({self.addr(i)})", "io_read32(c, {a})"))

    def op_lwu(self, i):
        self.w32z(i.rt, self.io_or(i, f"LW({self.addr(i)})", "io_read32(c, {a})"))

    def op_ld(self, i):
        if i.rt == 0:
            return
        a = self.io(i)
        if a is not None:
            self.w64(i.rt, f"io_read64(c, 0x{a:08X}u)")
            return
        self.out.append(f"{{ uint32_t _a = {self.addr(i)}; r{i.rt} = LW(_a); r{i.rt}_1 = LW(_a + 4); }}")
        self.writes.add(("r", i.rt, 0))
        self.writes.add(("r", i.rt, 1))
        self.wide[i.rt] = "wide"

    def op_lq(self, i):
        if i.rt == 0:
            return
        a = self.io(i)
        if a is not None:
            self.out.append(f"{{ u128 _v = io_read128(c, 0x{a:08X}u); r{i.rt} = _v.w[0]; r{i.rt}_1 = _v.w[1]; "
                            f"r{i.rt}_2 = _v.w[2]; r{i.rt}_3 = _v.w[3]; }}")
        else:
            self.out.append(f"{{ uint32_t _a = {self.addr(i)} & ~15u; r{i.rt} = LW(_a); r{i.rt}_1 = LW(_a + 4); "
                            f"r{i.rt}_2 = LW(_a + 8); r{i.rt}_3 = LW(_a + 12); }}")
        for k in range(4):
            self.writes.add(("r", i.rt, k))
        self.wide[i.rt] = "wide"

    def op_sb(self, i):
        v = self.g(i.rt)
        a = self.io(i)
        if a is not None:
            self.out.append(f"io_write8(c, 0x{a:08X}u, (uint8_t){v});")
        else:
            self.out.append(f"SB({self.addr(i)}, {v});")

    def op_sh(self, i):
        v = self.g(i.rt)
        a = self.io(i)
        if a is not None:
            self.out.append(f"io_write16(c, 0x{a:08X}u, (uint16_t){v});")
        else:
            self.out.append(f"SH({self.addr(i)}, {v});")

    def op_sw(self, i):
        v = self.g(i.rt)
        a = self.io(i)
        if a is not None:
            self.out.append(f"io_write32(c, 0x{a:08X}u, {v});")
        else:
            self.out.append(f"SW({self.addr(i)}, {v});")

    def op_sd(self, i):
        lo, hi = self.g(i.rt, 0), self.g(i.rt, 1)
        a = self.io(i)
        if a is not None:
            self.out.append(f"io_write64(c, 0x{a:08X}u, J64({lo}, {hi}));")
        else:
            self.out.append(f"{{ uint32_t _a = {self.addr(i)}; SW(_a, {lo}); SW(_a + 4, {hi}); }}")

    def op_sq(self, i):
        v = self.g128(i.rt)
        a = self.io(i)
        if a is not None:
            self.out.append(f"{{ u128 _v = {{{{ {', '.join(v)} }}}}; io_write128(c, 0x{a:08X}u, _v); }}")
        else:
            self.out.append(f"{{ uint32_t _a = {self.addr(i)} & ~15u; SW(_a, {v[0]}); SW(_a + 4, {v[1]}); "
                            f"SW(_a + 8, {v[2]}); SW(_a + 12, {v[3]}); }}")

    def op_lwl(self, i):
        self.w32(i.rt, f"ps2_lwl({self.addr(i)}, {self.g(i.rt)})")

    def op_lwr(self, i):
        # lwr with an aligned address loads the full word and sign-extends; otherwise
        # the upper 32 bits are preserved
        if i.rt == 0:
            return
        self.out.append(f"{{ uint32_t _a = {self.addr(i)}; uint32_t _v = ps2_lwr(_a, {self.g(i.rt)}); "
                        f"r{i.rt} = _v; if ((_a & 3) == 0) r{i.rt}_1 = SX(_v); }}")
        self.reads.add(("r", i.rt, 1))
        self.writes.add(("r", i.rt, 0))
        self.writes.add(("r", i.rt, 1))
        self.wide[i.rt] = "wide"

    def op_ldl(self, i):
        self.w64(i.rt, f"ps2_ldl({self.addr(i)}, {self.j64(i.rt)})")

    def op_ldr(self, i):
        self.w64(i.rt, f"ps2_ldr({self.addr(i)}, {self.j64(i.rt)})")

    def op_swl(self, i):
        self.out.append(f"ps2_swl({self.addr(i)}, {self.g(i.rt)});")

    def op_swr(self, i):
        self.out.append(f"ps2_swr({self.addr(i)}, {self.g(i.rt)});")

    def op_sdl(self, i):
        self.out.append(f"ps2_sdl({self.addr(i)}, {self.j64(i.rt)});")

    def op_sdr(self, i):
        self.out.append(f"ps2_sdr({self.addr(i)}, {self.j64(i.rt)});")

    # ===== FPU ===========================================================================

    def op_lwc1(self, i):
        a = self.io(i)
        if a is not None:
            self.setf(i.ft, f"BITS2F(io_read32(c, 0x{a:08X}u))")
        else:
            self.setf(i.ft, f"LWF({self.addr(i)})")

    def op_swc1(self, i):
        self.out.append(f"SWF({self.addr(i)}, {self.f(i.ft)});")

    def op_mtc1(self, i):
        self.setf(i.fs, f"BITS2F({self.g(i.rt)})")

    def op_mfc1(self, i):
        self.w32(i.rt, f"F2BITS({self.f(i.fs)})")

    def op_ctc1(self, i):
        if i.fs == 31:
            self.out.append(f"{self.res(('fcc',), True)} = ({self.g(i.rt)} >> 23) & 1;")

    def op_cfc1(self, i):
        if i.fs == 31:
            self.w32(i.rt, f"{self.res(('fcc',))} << 23")
        else:
            self.w32(i.rt, "0x2E30u" if i.fs == 0 else "0u")

    def fop(self, i, expr):
        self.setf(i.fd, self.fchk(expr))

    def op_add_s(self, i):
        self.fop(i, f"{self.f(i.fs)} + {self.f(i.ft)}")

    def op_sub_s(self, i):
        self.fop(i, f"{self.f(i.fs)} - {self.f(i.ft)}")

    def op_mul_s(self, i):
        self.fop(i, f"{self.f(i.fs)} * {self.f(i.ft)}")

    def op_div_s(self, i):
        self.fop(i, f"ps2_fdiv({self.f(i.fs)}, {self.f(i.ft)})")

    def op_sqrt_s(self, i):
        self.fop(i, f"ps2_fsqrt({self.f(i.ft)})")

    def op_rsqrt_s(self, i):
        self.fop(i, f"ps2_frsqrt({self.f(i.fs)}, {self.f(i.ft)})")

    def op_abs_s(self, i):
        self.fop(i, f"fabsf({self.f(i.fs)})")

    def op_neg_s(self, i):
        self.fop(i, f"-{self.f(i.fs)}")

    def op_mov_s(self, i):
        self.setf(i.fd, self.f(i.fs))

    def op_max_s(self, i):
        self.fop(i, f"ps2_fmax({self.f(i.fs)}, {self.f(i.ft)})")

    def op_min_s(self, i):
        self.fop(i, f"ps2_fmin({self.f(i.fs)}, {self.f(i.ft)})")

    def acc(self, write=False):
        return self.res(("facc",), write)

    def op_adda_s(self, i):
        self.out.append(f"{self.acc(True)} = {self.fchk(f'{self.f(i.fs)} + {self.f(i.ft)}')};")

    def op_suba_s(self, i):
        self.out.append(f"{self.acc(True)} = {self.fchk(f'{self.f(i.fs)} - {self.f(i.ft)}')};")

    def op_mula_s(self, i):
        self.out.append(f"{self.acc(True)} = {self.fchk(f'{self.f(i.fs)} * {self.f(i.ft)}')};")

    def op_madda_s(self, i):
        self.out.append(f"{self.acc(True)} = {self.fchk(f'{self.acc()} + {self.f(i.fs)} * {self.f(i.ft)}')};")

    def op_msuba_s(self, i):
        self.out.append(f"{self.acc(True)} = {self.fchk(f'{self.acc()} - {self.f(i.fs)} * {self.f(i.ft)}')};")

    def op_madd_s(self, i):
        self.fop(i, f"{self.acc()} + {self.f(i.fs)} * {self.f(i.ft)}")

    def op_msub_s(self, i):
        self.fop(i, f"{self.acc()} - {self.f(i.fs)} * {self.f(i.ft)}")

    def op_cvt_s_w(self, i):
        self.fop(i, f"(float)(int32_t)F2BITS({self.f(i.fs)})")

    def op_cvt_w_s(self, i):
        self.setf(i.fd, f"BITS2F((uint32_t)ps2_cvtws({self.f(i.fs)}))")    # an integer: not checked

    def fcmp(self, expr):
        self.out.append(f"{self.res(('fcc',), True)} = {expr};")

    def op_c_f_s(self, i):
        self.fcmp("0")

    def op_c_eq_s(self, i):
        self.fcmp(f"{self.f(i.fs)} == {self.f(i.ft)}")

    def op_c_lt_s(self, i):
        self.fcmp(f"{self.f(i.fs)} < {self.f(i.ft)}")

    def op_c_le_s(self, i):
        self.fcmp(f"{self.f(i.fs)} <= {self.f(i.ft)}")

    # ===== MMI (128-bit integer SIMD) ====================================================

    def mmi(self, i, fn, uses_hilo=False, uses_sa=False, reads_rd=False):
        s = self.g128(i.rs) if i.rs else ["0u"] * 4
        t = self.g128(i.rt) if i.rt else ["0u"] * 4
        args = [f"u128_make({', '.join(s)})", f"u128_make({', '.join(t)})"]
        if reads_rd:
            args.append(f"u128_make({', '.join(self.g128(i.rd) if i.rd else ['0u'] * 4)})")
        if uses_sa:
            args.append(self.res(("sa",)))
        if uses_hilo:
            hl = [self.res(("hi", k)) for k in range(4)] + [self.res(("lo", k)) for k in range(4)]
            for k in range(4):
                self.writes.add(("hi", k))
                self.writes.add(("lo", k))
            self.out.append(f"{{ u128 _hi = u128_make({', '.join(hl[:4])}), _lo = u128_make({', '.join(hl[4:])});")
            args += ["&_hi", "&_lo"]
            call = f"{fn}({', '.join(args)})"
            if i.rd:
                self.out.append(f"u128 _d = {call};")
                self.w128(i.rd, [f"_d.w[{k}]" for k in range(4)])
            else:
                self.out.append(f"(void){call};")
            self.out.append(" ".join(f"hi{k} = _hi.w[{k}]; lo{k} = _lo.w[{k}];" for k in range(4)) + " }")
            return
        if i.rd == 0:
            return
        self.out.append(f"{{ u128 _d = {fn}({', '.join(args)});")
        self.w128(i.rd, [f"_d.w[{k}]" for k in range(4)])
        self.out.append("}")

    def mmi_shift(self, i, fn):
        if i.rd == 0:
            return
        t = self.g128(i.rt)
        self.out.append(f"{{ u128 _d = {fn}(u128_make({', '.join(t)}), {i.sa});")
        self.w128(i.rd, [f"_d.w[{k}]" for k in range(4)])
        self.out.append("}")

    def op_pcpyld(self, i):
        if i.rd == 0:
            return
        # rd = { rt[63:0], rs[63:0] } (rt in the low doubleword)
        lanes = [self.g(i.rt, 0), self.g(i.rt, 1), self.g(i.rs, 0), self.g(i.rs, 1)]
        self.out.append(f"{{ uint32_t _a = {lanes[0]}, _b = {lanes[1]}, _c = {lanes[2]}, _e = {lanes[3]};")
        self.w128(i.rd, ["_a", "_b", "_c", "_e"])
        self.out.append("}")

    def op_pcpyud(self, i):
        if i.rd == 0:
            return
        # rd = { rs[127:64], rt[127:64] } -> low = rs.hi, high = rt.hi
        lanes = [self.g(i.rs, 2), self.g(i.rs, 3), self.g(i.rt, 2), self.g(i.rt, 3)]
        self.out.append(f"{{ uint32_t _a = {lanes[0]}, _b = {lanes[1]}, _c = {lanes[2]}, _e = {lanes[3]};")
        self.w128(i.rd, ["_a", "_b", "_c", "_e"])
        self.out.append("}")

    def op_por(self, i):
        if i.rd == 0:
            return
        if i.rt == 0:
            self.w128(i.rd, self.g128(i.rs))
            return
        self.mmi(i, "mmi_por")

    def op_qfsrv(self, i):
        self.mmi(i, "mmi_qfsrv", uses_sa=True)

    def op_pmfhi(self, i):
        if i.rd:
            self.w128(i.rd, [self.res(("hi", k)) for k in range(4)])

    def op_pmflo(self, i):
        if i.rd:
            self.w128(i.rd, [self.res(("lo", k)) for k in range(4)])

    def op_pmthi(self, i):
        for k in range(4):
            self.out.append(f"{self.res(('hi', k), True)} = {self.g(i.rs, k)};")

    def op_pmtlo(self, i):
        for k in range(4):
            self.out.append(f"{self.res(('lo', k), True)} = {self.g(i.rs, k)};")

    def op_pmfhl(self, i):
        if i.rd == 0:
            return
        hl = [self.res(("hi", k)) for k in range(4)] + [self.res(("lo", k)) for k in range(4)]
        self.out.append(f"{{ u128 _d = mmi_pmfhl(u128_make({', '.join(hl[4:])}), u128_make({', '.join(hl[:4])}), {i.sa});")
        self.w128(i.rd, [f"_d.w[{k}]" for k in range(4)])
        self.out.append("}")

    def op_pmthl(self, i):
        # PMTHL.LW: LO[31:0]=rs[31:0], HI[31:0]=rs[63:32], LO[95:64]=rs[95:64], HI[95:64]=rs[127:96]
        self.out.append(f"lo0 = {self.g(i.rs, 0)}; hi0 = {self.g(i.rs, 1)}; lo2 = {self.g(i.rs, 2)}; hi2 = {self.g(i.rs, 3)};")
        for r in (("lo", 0), ("hi", 0), ("lo", 2), ("hi", 2)):
            self.writes.add(r)


# Straightforward MMI ops: name -> (runtime function, flags)
_MMI_SIMPLE = [
    "paddw", "psubw", "pcgtw", "pmaxw", "paddh", "psubh", "pcgth", "pmaxh", "paddb", "psubb", "pcgtb",
    "paddsw", "psubsw", "pextlw", "ppacw", "paddsh", "psubsh", "pextlh", "ppach", "paddsb", "psubsb",
    "pextlb", "ppacb", "pext5", "ppac5",
    "pabsw", "pceqw", "pminw", "padsbh", "pabsh", "pceqh", "pminh", "pceqb", "padduw", "psubuw", "pextuw",
    "padduh", "psubuh", "pextuh", "paddub", "psubub", "pextub",
    "psllvw", "psrlvw", "pinth", "pand", "pxor", "pexeh", "prevh", "pexew", "prot3w",
    "psravw", "pinteh", "pnor", "pexch", "pcpyh", "pexcw",
]
_MMI_HILO = ["pmaddw", "pmsubw", "pmultw", "pdivw", "pmaddh", "phmadh", "pmsubh", "phmsbh", "pmulth",
             "pdivbw", "pmadduw", "pmultuw", "pdivuw"]
_MMI_SHIFT = ["psllh", "psrlh", "psrah", "psllw", "psrlw", "psraw"]

for _n in _MMI_SIMPLE:
    setattr(Lift, "op_" + _n, (lambda fn: lambda self, i: self.mmi(i, fn))("mmi_" + _n))
for _n in _MMI_HILO:
    setattr(Lift, "op_" + _n, (lambda fn: lambda self, i: self.mmi(i, fn, uses_hilo=True))("mmi_" + _n))
for _n in _MMI_SHIFT:
    setattr(Lift, "op_" + _n, (lambda fn: lambda self, i: self.mmi_shift(i, fn))("mmi_" + _n))
