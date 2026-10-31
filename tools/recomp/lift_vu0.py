"""COP2 (VU0 macro mode) translation, mixed into lift.Lift.

VF0 is the constant (0, 0, 0, 1) and is never written. The MAC/status/clip
flags are not modelled: Burnout 3's macro code never reads them (it reads
VI registers and VPU-STAT only).
"""
from lift import COMP, Lift, res_name

CTRL_I, CTRL_R, CTRL_Q = 21, 20, 22


def _lanes(dest):
    return [c for k, c in enumerate(COMP) if dest & (8 >> k)]


class VU0Mixin:
    # -- transfers ---------------------------------------------------------------------

    def op_lqc2(self, i):
        a = self.addr(i)
        stmts = [s for s in (self.setvf(i.ft, c, f"LWF(_a + {4 * k}u)") for k, c in enumerate(COMP)) if s]
        if stmts:
            self.out.append(f"{{ uint32_t _a = {a} & ~15u; {' '.join(stmts)} }}")

    def op_sqc2(self, i):
        a = self.addr(i)
        vals = [self.vf(i.ft, c) for c in COMP]
        self.out.append(f"{{ uint32_t _a = {a} & ~15u; " +
                        " ".join(f"SWF(_a + {4 * k}u, {v});" for k, v in enumerate(vals)) + " }")

    def op_qmtc2(self, i):
        if i.fs == 0:
            return
        lanes = self.g128(i.rt) if i.rt else ["0u"] * 4
        tmp = " ".join(f"uint32_t _t{k} = {lanes[k]};" for k in range(4))
        sets = " ".join(self.setvf(i.fs, c, f"BITS2F(_t{k})") for k, c in enumerate(COMP))
        self.out.append(f"{{ {tmp} {sets} }}")

    def op_qmfc2(self, i):
        if i.rt == 0:
            return
        vals = [self.vf(i.fs, c) for c in COMP]
        tmp = " ".join(f"uint32_t _t{k} = F2BITS({vals[k]});" for k in range(4))
        self.out.append(f"{{ {tmp}")
        self.w128(i.rt, [f"_t{k}" for k in range(4)])
        self.out.append("}")

    def op_ctc2(self, i):
        v = self.g(i.rt)
        n = i.rd
        if 1 <= n <= 15:
            self.setvi(n, v)
        elif n == CTRL_I:
            self.out.append(f"{self.res(('vI',), True)} = BITS2F({v});")
        elif n == CTRL_R:
            self.out.append(f"{self.res(('vr',), True)} = {v};")
        elif n == CTRL_Q:
            self.out.append(f"{self.res(('vq',), True)} = BITS2F({v});")
        else:
            self.out.append(f"ps2_vu0_ctc2(c, {n}, {v});")

    def op_cfc2(self, i):
        n = i.rd
        if n == 0:
            self.w32(i.rt, "0u")
        elif 1 <= n <= 15:
            self.w32(i.rt, self.vi(n))
        elif n == CTRL_I:
            self.w32(i.rt, f"F2BITS({self.res(('vI',))})")
        elif n == CTRL_R:
            self.w32(i.rt, self.res(("vr",)))
        elif n == CTRL_Q:
            self.w32(i.rt, f"F2BITS({self.res(('vq',))})")
        else:
            self.w32(i.rt, f"ps2_vu0_cfc2(c, {n})")

    # -- FMAC ----------------------------------------------------------------------------

    def _src(self, i, kind, comp):
        """Second operand for a lane: ft lane, ft broadcast, Q or I."""
        if kind == "vec":
            return self.vf(i.ft, comp)
        if kind == "bc":
            return self.vf(i.ft, COMP[i.bc])
        if kind == "q":
            return self.res(("vq",))
        if kind == "i":
            return self.res(("vI",))
        raise ValueError(kind)

    def _assign_lanes(self, dst_kind, i, exprs):
        """exprs: comp -> C expression. Evaluates all lanes before writing any."""
        if not exprs:
            return
        tmps = " ".join(f"float _{c} = {self.fchk(e)};" for c, e in exprs.items())
        sets = []
        for c in exprs:
            if dst_kind == "acc":
                sets.append(f"{self.res(('vacc', c), True)} = _{c};")
            else:
                s = self.setvf(i.fd, c, f"_{c}")
                if s:
                    sets.append(s)
        self.out.append(f"{{ {tmps} {' '.join(sets)} }}")

    def fmac(self, i, op, kind, to_acc=False):
        exprs = {}
        for c in _lanes(i.dest):
            a = self.vf(i.fs, c)
            b = self._src(i, kind, c)
            if op == "add":
                e = f"{a} + {b}"
            elif op == "sub":
                e = f"{a} - {b}"
            elif op == "mul":
                e = f"{a} * {b}"
            elif op == "madd":
                e = f"{self.res(('vacc', c))} + {a} * {b}"
            elif op == "msub":
                e = f"{self.res(('vacc', c))} - {a} * {b}"
            elif op == "max":
                e = f"ps2_fmax({a}, {b})"
            elif op == "min":
                e = f"ps2_fmin({a}, {b})"
            else:
                raise ValueError(op)
            exprs[c] = e
        self._assign_lanes("acc" if to_acc else "vf", i, exprs)

    def op_vopmula(self, i):
        # ACC.xyz = fs.yzx * ft.zxy
        fs = {c: self.vf(i.fs, c) for c in COMP}
        ft = {c: self.vf(i.ft, c) for c in COMP}
        exprs = {"x": f"{fs['y']} * {ft['z']}", "y": f"{fs['z']} * {ft['x']}", "z": f"{fs['x']} * {ft['y']}"}
        self._assign_lanes("acc", i, exprs)

    def op_vopmsub(self, i):
        fs = {c: self.vf(i.fs, c) for c in COMP}
        ft = {c: self.vf(i.ft, c) for c in COMP}
        acc = {c: self.res(("vacc", c)) for c in "xyz"}
        exprs = {"x": f"{acc['x']} - {fs['y']} * {ft['z']}",
                 "y": f"{acc['y']} - {fs['z']} * {ft['x']}",
                 "z": f"{acc['z']} - {fs['x']} * {ft['y']}"}
        self._assign_lanes("vf", i, exprs)

    def op_vabs(self, i):
        exprs = {c: self.fchk(f"fabsf({self.vf(i.fs, c)})") for c in _lanes(i.dest)}
        self._assign_ft(i, exprs)

    def _assign_ft(self, i, exprs):
        if not exprs:
            return
        tmps = " ".join(f"float _{c} = {e};" for c, e in exprs.items())
        sets = " ".join(s for s in (self.setvf(i.ft, c, f"_{c}") for c in exprs) if s)
        self.out.append(f"{{ {tmps} {sets} }}")

    def op_vmove(self, i):
        self._assign_ft(i, {c: self.vf(i.fs, c) for c in _lanes(i.dest)})

    def op_vmr32(self, i):
        rot = {"x": "y", "y": "z", "z": "w", "w": "x"}
        self._assign_ft(i, {c: self.vf(i.fs, rot[c]) for c in _lanes(i.dest)})

    def _conv(self, i, fn):
        self._assign_ft(i, {c: self.fchk(f"{fn}({self.vf(i.fs, c)})") if fn.startswith("ps2_itof")
                            else f"{fn}({self.vf(i.fs, c)})" for c in _lanes(i.dest)})

    def op_vitof0(self, i):
        self._conv(i, "ps2_itof0")

    def op_vitof4(self, i):
        self._conv(i, "ps2_itof4")

    def op_vitof12(self, i):
        self._conv(i, "ps2_itof12")

    def op_vitof15(self, i):
        self._conv(i, "ps2_itof15")

    def op_vftoi0(self, i):
        self._conv(i, "ps2_ftoi0")

    def op_vftoi4(self, i):
        self._conv(i, "ps2_ftoi4")

    def op_vftoi12(self, i):
        self._conv(i, "ps2_ftoi12")

    def op_vftoi15(self, i):
        self._conv(i, "ps2_ftoi15")

    def op_vclipw(self, i):
        self.out.append(f"ps2_vu0_clip(c, {self.vf(i.fs, 'x')}, {self.vf(i.fs, 'y')}, "
                        f"{self.vf(i.fs, 'z')}, {self.vf(i.ft, 'w')});")

    # -- divider --------------------------------------------------------------------------

    def op_vdiv(self, i):
        self.out.append(f"{self.res(('vq',), True)} = ps2_fdiv({self.vf(i.fs, COMP[i.fsf])}, "
                        f"{self.vf(i.ft, COMP[i.ftf])});")

    def op_vsqrt(self, i):
        self.out.append(f"{self.res(('vq',), True)} = ps2_fsqrt({self.vf(i.ft, COMP[i.ftf])});")

    def op_vrsqrt(self, i):
        self.out.append(f"{self.res(('vq',), True)} = ps2_frsqrt({self.vf(i.fs, COMP[i.fsf])}, "
                        f"{self.vf(i.ft, COMP[i.ftf])});")

    def op_vwaitq(self, i):
        pass

    op_vnop = op_vwaitq

    # -- integer unit ----------------------------------------------------------------------

    def op_viadd(self, i):
        self.setvi(i.fd, f"{self.vi(i.fs)} + {self.vi(i.ft)}")

    def op_visub(self, i):
        self.setvi(i.fd, f"{self.vi(i.fs)} - {self.vi(i.ft)}")

    def op_viand(self, i):
        self.setvi(i.fd, f"{self.vi(i.fs)} & {self.vi(i.ft)}")

    def op_vior(self, i):
        self.setvi(i.fd, f"{self.vi(i.fs)} | {self.vi(i.ft)}")

    def op_viaddi(self, i):
        imm = i.sa if i.sa < 16 else i.sa - 32
        self.setvi(i.ft, f"{self.vi(i.fs)} + ({imm})")

    def op_vmtir(self, i):
        self.setvi(i.ft, f"F2BITS({self.vf(i.fs, COMP[i.fsf])})")

    def op_vmfir(self, i):
        v = self.vi(i.fs)
        self._assign_ft(i, {c: f"BITS2F((uint32_t)(int32_t)(int16_t){v})" for c in _lanes(i.dest)})

    def _vumem(self, i, what):
        self.out.append(f"ps2_vu0_unhandled(c, 0x{i.pc:08X}u, \"{what}\");")

    def op_vilwr(self, i):
        self._vumem(i, "vilwr")

    def op_viswr(self, i):
        self._vumem(i, "viswr")

    def op_vlqi(self, i):
        self._vumem(i, "vlqi")

    def op_vsqi(self, i):
        self._vumem(i, "vsqi")

    def op_vlqd(self, i):
        self._vumem(i, "vlqd")

    def op_vsqd(self, i):
        self._vumem(i, "vsqd")

    # -- random ----------------------------------------------------------------------------

    def op_vrinit(self, i):
        self.out.append(f"{self.res(('vr',), True)} = 0x3F800000u | (F2BITS({self.vf(i.fs, COMP[i.fsf])}) & 0x7FFFFFu);")

    def op_vrxor(self, i):
        r = self.res(("vr",))
        self.out.append(f"{self.res(('vr',), True)} = 0x3F800000u | (({r} ^ F2BITS({self.vf(i.fs, COMP[i.fsf])})) & 0x7FFFFFu);")

    def op_vrnext(self, i):
        r = self.res(("vr",))
        self.out.append(f"{self.res(('vr',), True)} = ps2_vu_rnext({r});")
        self.op_vrget(i)

    def op_vrget(self, i):
        r = self.res(("vr",))
        self._assign_ft(i, {c: f"BITS2F({r})" for c in _lanes(i.dest)})

    # -- micro subroutines ------------------------------------------------------------------

    @staticmethod
    def _inl_field(r):
        """a resource as a field of the runtime's Vu0I (vu0_inline.h)"""
        if r[0] == "vf":
            return f"vf[{r[1]}][{COMP.index(r[2])}]"
        if r[0] == "vi":
            return f"vi[{r[1]}]"
        return "q"

    def _inline_site(self, entry, spec):
        """A VCALLMS the title runs inline (VCALLMS_INLINE: the function that does what the entry does on a Vu0I,
        the registers it reads, the registers it writes): an ordinary step on the locals, reading and writing
        what the title says. When the runtime does not take it (vu0_inl_enter: the entry is not native and
        checked yet, a debugging switch, something in flight in VU0) the call is made as ever, through the
        context, but only these registers go there and come back."""
        fn, reads, writes = spec
        rd = [(r, self.res(r)) for r in reads]
        wr = [(r, self.res(r, True)) for r in writes]
        from lift import res_ctx
        out = ["{ Vu0I s_;"]
        out += [f"s_.{self._inl_field(r)} = {n};" for r, n in rd]
        out.append(f"if (vu0_inl_enter(c, {entry}u)) {{")
        out.append(f"{fn}(&s_);")
        out += [f"{n} = s_.{self._inl_field(r)};" for r, n in wr]
        out.append("} else {")
        out += [f"{res_ctx(r)} = {n};" for r, n in rd]
        out.append(f"ps2_vu0_callms(c, {entry}u);")
        out += [f"{n} = {res_ctx(r)};" for r, n in wr]
        out.append("} }")
        self.out.append("\n".join(out))

    def op_vcallms(self, i):
        # the function emitter syncs all VU0 state around this call; a site the title names (VCALLMS_SITES)
        # calls its own native, which takes the same arguments
        fn = getattr(self, "vcall_sites", {}).get(i.pc)
        spec = getattr(self, "vcall_inline", {}).get(i.pc)
        if spec:
            self._inline_site(((i.word >> 6) & 0x7FFF) * 8, spec)
            return
        if fn:
            self.out.append(f"{{ extern void {fn}(R5900 *, uint32_t); {fn}(c, {((i.word >> 6) & 0x7FFF) * 8}u); }}"
                            "  /* VCALLMS_SITES: in place of ps2_vu0_callms (the emitter finds the call by that) */")
        else:
            self.out.append(f"ps2_vu0_callms(c, {((i.word >> 6) & 0x7FFF) * 8}u);")
        self.vu0_call = True

    def op_vcallmsr(self, i):
        self.out.append("ps2_vu0_callmsr(c);")
        self.vu0_call = True

    # COP2 branches are handled by the function emitter (bc2t/f read VU0 busy: never busy)


# broadcast, Q, I and vector variants of the FMAC ops
for _op, _base in (("add", "vadd"), ("sub", "vsub"), ("madd", "vmadd"), ("msub", "vmsub"),
                   ("max", "vmax"), ("min", "vmini"), ("mul", "vmul")):
    for _b, _c in enumerate(COMP):
        setattr(VU0Mixin, f"op_{_base}{_c}",
                (lambda op: lambda self, i: self.fmac(i, op, "bc"))(_op))
    setattr(VU0Mixin, f"op_{_base}", (lambda op: lambda self, i: self.fmac(i, op, "vec"))(_op))
for _op, _base in (("add", "vadd"), ("sub", "vsub"), ("madd", "vmadd"), ("msub", "vmsub"), ("mul", "vmul")):
    setattr(VU0Mixin, f"op_{_base}q", (lambda op: lambda self, i: self.fmac(i, op, "q"))(_op))
    setattr(VU0Mixin, f"op_{_base}i", (lambda op: lambda self, i: self.fmac(i, op, "i"))(_op))
setattr(VU0Mixin, "op_vmaxi", lambda self, i: self.fmac(i, "max", "i"))
setattr(VU0Mixin, "op_vminii", lambda self, i: self.fmac(i, "min", "i"))
# accumulator forms
for _op, _base in (("add", "vadda"), ("sub", "vsuba"), ("madd", "vmadda"), ("msub", "vmsuba"), ("mul", "vmula")):
    for _b, _c in enumerate(COMP):
        setattr(VU0Mixin, f"op_{_base}{_c}",
                (lambda op: lambda self, i: self.fmac(i, op, "bc", to_acc=True))(_op))
    setattr(VU0Mixin, f"op_{_base}", (lambda op: lambda self, i: self.fmac(i, op, "vec", to_acc=True))(_op))
    setattr(VU0Mixin, f"op_{_base}q", (lambda op: lambda self, i: self.fmac(i, op, "q", to_acc=True))(_op))
    setattr(VU0Mixin, f"op_{_base}i", (lambda op: lambda self, i: self.fmac(i, op, "i", to_acc=True))(_op))


class FullLift(VU0Mixin, Lift):
    def lift(self, i, consts, wide_regs=frozenset()):
        self.wide_regs = wide_regs
        self.vu0_call = False
        return Lift.lift(self, i, consts)


_ = res_name
