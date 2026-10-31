"""C emission for analysed functions.

Each function becomes `void f_XXXXXXXX(R5900 *c)`. Guest registers are C
locals that cache the context:

* at entry, the registers the function reads are loaded;
* before a call, dirty registers the callee may read or write are stored;
* after a call, registers the callee may have written are reloaded;
* before returning, every dirty register is stored.

Which registers a callee reads (live-in) and writes (may-write) comes from a
whole-program fixpoint over per-function summaries, so a call to a small leaf
function only synchronises the handful of registers it touches. Resource sets
are Python ints used as bitsets.

Callee-saved registers (abi.py): a function that preserves one does not count
as writing it, and unless it uses the caller's value, not as reading it, so
its callers neither store nor reload it; where it preserves it by a save of
its own and all it calls do the same, the save and the restores are left out
and the register is a C local that never reaches the context. The same for
the callee-saved FPU registers; and sp, which a callee leaves as it found it,
is not read back after a call.

Return values (return_values below): v0, v1 and f0 are written back at a
return only where some caller may read them after the call. Before, every
function that had used v0 or v1 as a scratch register stored them (four
words) at each return, and one that wrote them on some paths only also
loaded them at its entry, to have something to store on the others.
"""
import os

import abi
from lift import LiftError, res_ctx, res_name, res_type
from lift_vu0 import FullLift
from r5900 import COND_BRANCHES, LIKELY, format_insn, is_unconditional_branch

LINK_BRANCHES = {"bltzal", "bgezal", "bltzall", "bgezall"}
LIKELY_BASE = {"beql": "beq", "bnel": "bne", "blezl": "blez", "bgtzl": "bgtz", "bltzl": "bltz",
               "bgezl": "bgez", "bltzall": "bltzal", "bgezall": "bgezal"}

# -- resource universe ---------------------------------------------------------------

RESOURCES = []
for _n in range(1, 32):
    for _k in range(4):
        RESOURCES.append(("r", _n, _k))
RESOURCES += [("hi", k) for k in range(4)] + [("lo", k) for k in range(4)] + [("sa",)]
RESOURCES += [("f", n) for n in range(32)] + [("facc",), ("fcc",)]
RESOURCES += [("vf", n, c) for n in range(1, 32) for c in "xyzw"]
RESOURCES += [("vacc", c) for c in "xyzw"] + [("vq",), ("vI",), ("vp",), ("vr",)]
RESOURCES += [("vi", n) for n in range(1, 16)]
RESOURCES += [("c0", n) for n in range(32)]
BIT = {r: 1 << k for k, r in enumerate(RESOURCES)}
ALL = (1 << len(RESOURCES)) - 1


def bits(resources):
    v = 0
    for r in resources:
        v |= BIT[r]
    return v


def unbits(v):
    out = []
    k = 0
    while v:
        if v & 1:
            out.append(RESOURCES[k])
        v >>= 1
        k += 1
    return out


def parse_res(spec):
    """'vf2.xyz vf23 vi1 vq' (a title's VCALLMS_INLINE) as resources: vfN.lanes (all four lanes without), viN, vq"""
    out = []
    for tok in spec.split():
        if tok == "vq":
            out.append(("vq",))
        elif tok.startswith("vf"):
            n, _, lanes = tok[2:].partition(".")
            out += [("vf", int(n), c) for c in (lanes or "xyzw")]
        elif tok.startswith("vi"):
            out.append(("vi", int(tok[2:])))
        else:
            raise ValueError(f"VCALLMS_INLINE: unknown register {tok}")
    return out


VU0_ALL = bits(r for r in RESOURCES if r[0] in ("vf", "vacc", "vq", "vI", "vp", "vr", "vi"))
GPR_LANES01 = lambda regs: bits(("r", n, k) for n in regs for k in (0, 1))   # noqa: E731
SYSCALL_LIVE_IN = GPR_LANES01([3, 4, 5, 6, 7, 8, 9, 10, 11, 29])
SYSCALL_MAY_WRITE = GPR_LANES01([2, 3])

# Caller-saved state under the EE ABI. Compiled code never relies on these
# surviving a call, so a callee need not publish them at return and a caller
# need not store them before a call unless it reads them afterwards. v0/v1/f0
# carry return values and are handled separately.
_TEMP_GPRS = [1, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27]
TEMPS = bits(("r", n, k) for n in _TEMP_GPRS for k in range(4)) |     bits([("hi", k) for k in range(4)] + [("lo", k) for k in range(4)] + [("sa",), ("fcc",), ("facc",)]) |     bits(("f", n) for n in range(1, 20))
RETVALS = bits(("r", n, k) for n in (2, 3) for k in range(4)) | BIT[("f", 0)]
CALL_CLOBBER = TEMPS | RETVALS
CALLEE_SAVED_GPRS = {16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30}
# registers whose upper 64 bits may not be the sign extension of the lower 32
# (so compares on them need all 64 bits): any value a caller passed in, and the
# results of a call
ENTRY_WIDE = frozenset(range(1, 32))
RETURN_WIDE = frozenset({2, 3})
# what a native override may read: arguments, stack, gp, ra
OVERRIDE_LIVE_IN = bits(("r", n, k) for n in (4, 5, 6, 7, 8, 9, 10, 11, 28, 29, 31) for k in range(4)) | \
    bits(("f", n) for n in range(12, 20))
OVERRIDE_SUMMARY = (OVERRIDE_LIVE_IN, CALL_CLOBBER)
# a native in the title's OVERRIDE_VU0 reads no VU0 state but may leave any of it changed (it does what a VU0
# routine called inside the original does): its call sites store what they hold in the VU0 registers and read back
# the ones used after the call from the context, as after a vcallms
OVERRIDE_VU0_SUMMARY = (OVERRIDE_LIVE_IN, CALL_CLOBBER | VU0_ALL)


def override_summary(prog, target):
    return OVERRIDE_VU0_SUMMARY if target in prog.override_vu0 else OVERRIDE_SUMMARY


class Item:
    """One emitted step: an instruction, or a control transfer with its delay slot."""
    __slots__ = ("pc", "kind", "insn", "succ", "fall", "pre", "cond", "reads", "writes",
                 "kills", "effects", "sync", "target", "table", "vu0", "seq",
                 "dirty_in", "live_out", "llive_out", "live_in", "llive_in")

    def __init__(self, pc, kind, insn):
        self.pc = pc
        self.kind = kind
        self.insn = insn
        self.succ = []          # local successor pcs
        self.fall = None        # fallthrough pc (also in succ)
        self.pre = []           # statements before the transfer (delay slot etc.)
        self.cond = None
        self.reads = 0
        self.writes = 0
        self.kills = 0
        self.effects = []       # wide effects in order
        self.sync = None        # None | "call" | "exit" | "exit_taken"
        self.target = None
        self.table = None
        self.vu0 = False
        self.seq = []           # (insn, written resources) in execution order


def _branch_cond(lifter, i, wide):
    """C condition for a conditional branch, using 64-bit compares when needed."""
    op = LIKELY_BASE.get(i.op, i.op)
    g = lifter.g
    if op in ("beq", "bne"):
        rel = "==" if op == "beq" else "!="
        if any(r in wide for r in (i.rs, i.rt) if r):
            return f"{lifter.j64(i.rs)} {rel} {lifter.j64(i.rt)}"
        return f"{g(i.rs)} {rel} {g(i.rt)}"
    rel = {"blez": "<=", "bgtz": ">", "bltz": "<", "bgez": ">=", "bltzal": "<", "bgezal": ">="}.get(op)
    if rel:
        if i.rs in wide:
            return f"(int64_t){lifter.j64(i.rs)} {rel} 0"
        return f"(int32_t){g(i.rs)} {rel} 0"
    if op in ("bc1t", "bc1tl"):
        return lifter.res(("fcc",))
    if op in ("bc1f", "bc1fl"):
        return f"!{lifter.res(('fcc',))}"
    if op in ("bc0t", "bc0tl"):
        return "ps2_cpcond0(c)"
    if op in ("bc0f", "bc0fl"):
        return "!ps2_cpcond0(c)"
    if op in ("bc2t", "bc2tl"):
        return "0"          # VU0 never busy: micro calls run synchronously
    if op in ("bc2f", "bc2fl"):
        return "1"
    raise LiftError(f"branch {i.op}")


def _apply_effects(wide, effects):
    w = set(wide)
    for eff in effects:
        for reg, kind in eff.items():
            if kind == "sext":
                w.discard(reg)
            elif kind == "wide":
                w.add(reg)
            elif kind[0] == "copy":
                (w.add if kind[1] in w else w.discard)(reg)
            elif kind[0] == "join":
                (w.add if (kind[1] in w or kind[2] in w) else w.discard)(reg)
    return frozenset(w)


def _update_consts(consts, insn, writes):
    op = insn.op
    new = dict(consts)
    if op == "lui":
        new[insn.rt] = (insn.imm << 16) & 0xFFFFFFFF
        return new
    if op == "addiu" and insn.rs in consts:
        new[insn.rt] = (consts[insn.rs] + insn.simm) & 0xFFFFFFFF
        return new
    if op == "ori" and insn.rs in consts:
        new[insn.rt] = consts[insn.rs] | insn.imm
        return new
    if op == "daddu" and insn.rt == 0 and insn.rs in consts:
        new[insn.rd] = consts[insn.rs]
        return new
    for r in writes:
        if r[0] == "r":
            new.pop(r[1], None)
    return new


class FuncEmitter:
    def __init__(self, prog, func, names):
        self.prog = prog
        self.f = func
        self.names = names
        self.lifter = FullLift()
        self.lifter.vcall_sites = getattr(prog, "vcall_sites", {})
        self.fprefix = None     # the C name's prefix when not the program's (an inline variant, see emit_program)
        self.fid = None         # index in ps2_func_table, for PS2_PROF counters
        self.items = None
        self.order = None
        # callee-saved registers (abi.py): the saves and restores left out, the registers that stay C
        # locals for it, and what the function's summary does not show its callers
        self.skip_pcs = frozenset()
        self.private = 0
        self.drop_li = 0
        self.drop_mw = 0
        self.ret_live = RETVALS     # the return registers some caller may read after a call of this

    # -- lifting ------------------------------------------------------------------------

    def lift(self, insn, consts, wide):
        if insn.pc in self.skip_pcs:
            return [], set(), set(), {}, False      # the save or restore of a register kept in a C local
        try:
            stmts, reads, writes, eff = self.lifter.lift(insn, consts, wide)
            return list(stmts), set(reads), set(writes), dict(eff), self.lifter.vu0_call
        except LiftError:
            return [f"ps2_bad_insn(c, 0x{insn.pc:08X}u);"], set(), set(), {}, False

    def lift_patched(self, i, site, consts, wide):
        """A code patch's site (patches.py): both variants under the runtime's bit, as one step that reads what
        either reads, may write what either writes and kills what both write. (statements, reads, writes,
        kills as bits, wide effects, every resource written) or None when it cannot be done here."""
        if site.cave and i.pc in self.f.delay_slots:
            print(f"code patch {site.name}: {i.pc:08X} left out in {self.f.name}: a jump into a cave in a delay slot")
            return None
        sa, ra, wa, _, va = self.lift(i, consts, wide)
        sb = []
        rb = kb = 0
        wall = set(wa)
        cs, wd = consts, wide
        vb = False
        for j in site.alt:
            s, r, w, e, v = self.lift(j, cs, wd)
            rb |= bits(r) & ~kb
            kb |= bits(w)
            wall |= w
            sb += [f"/* {j.pc:08X}: {format_insn(j)} */"] + s
            cs = _update_consts(cs, j, w)
            wd = _apply_effects(wd, [e])
            vb = vb or v
        if va or vb:
            print(f"code patch {site.name}: {i.pc:08X} left out in {self.f.name}: a VU0 call")
            return None
        reads = bits(ra) | rb
        kills = bits(wa) & kb
        # a register written by either: its upper lanes taken as anything (64-bit compares after it)
        eff = {r[1]: "wide" for r in wall if r[0] == "r"}
        stmts = [f"if (g_code_patch & 0x{site.mask:X}u) {{     /* code patch {site.name} */"] + \
            ["    " + x for x in sb] + ["} else {"] + ["    " + x for x in sa] + ["}"]
        return stmts, set(unbits(reads)), wall, kills, eff

    def build_items(self, wide_at, consts_at=None):
        f = self.f
        items = {}
        order = []
        standalone_delay = {pc for pc in f.delay_slots if pc in f.labels}
        for pc in sorted(f.insns):
            if pc in f.delay_slots and pc not in standalone_delay:
                continue
            i = f.insns[pc]
            consts = (consts_at.get(pc) or {}) if consts_at else {}
            it = self.make_item(i, consts, wide_at.get(pc, frozenset()))
            items[pc] = it
            order.append(pc)
        return items, order

    def const_dataflow(self, items, order):
        """Must-constants per item (register -> value on every path), for I/O routing."""
        preds = {pc: [] for pc in order}
        for pc in order:
            for s_ in items[pc].succ:
                if s_ in preds:
                    preds[s_].append(pc)
        out = {}
        inn = {}
        start = self.f.start
        changed = True
        while changed:
            changed = False
            for pc in order:
                it = items[pc]
                c = None
                if pc == start:
                    c = {}
                for p in preds[pc]:
                    o = out.get(p)
                    if o is None:
                        continue            # not computed yet: optimistic
                    if c is None:
                        c = dict(o)
                    else:
                        c = {r: v for r, v in c.items() if o.get(r) == v}
                if c is None:
                    continue
                inn[pc] = c
                cur = c
                for insn, w in it.seq:
                    if insn is None:
                        cur = {r: v for r, v in cur.items() if not any(x[0] == "r" and x[1] == r for x in w)}
                    else:
                        cur = _update_consts(cur, insn, w)
                if it.sync == "call" or it.kind in ("syscall", "vu0call"):
                    cur = {r: v for r, v in cur.items() if r in CALLEE_SAVED_GPRS}
                if it.kind in ("branch", "likely"):
                    cur = {r: v for r, v in cur.items() if c.get(r) == v}
                if out.get(pc) != cur:
                    out[pc] = cur
                    changed = True
        return inn

    def make_item(self, i, consts, wide):
        f = self.f
        op = i.op
        pc = i.pc
        it = Item(pc, "normal", i)
        lf = self.lifter
        lf.reads = set()
        lf.writes = set()

        def add(stmts, reads, writes, eff, vu0=False, kill=True, kills=None):
            b = bits(writes)
            it.reads |= bits(reads) & ~it.kills     # reads after an unconditional write are internal
            it.writes |= b
            if kill:
                it.kills |= b if kills is None else kills
            it.effects.append(eff)
            it.vu0 = it.vu0 or vu0
            return stmts

        sites = self.prog.code_patches

        def take_reads():
            it.reads |= bits(lf.reads)
            lf.reads = set()

        def delay_code(kill=True):
            ds = pc + 4
            if ds not in f.insns:
                return []
            d = f.insns[ds]
            dconsts = consts
            for insn_, w_ in it.seq:
                if insn_ is not None:
                    dconsts = _update_consts(dconsts, insn_, w_)
            dwide = _apply_effects(wide, it.effects)
            pat = self.lift_patched(d, sites[ds], dconsts, dwide) if ds in sites else None
            if pat is not None:
                stmts, r, w, kl, e = pat
                it.seq.append((None, w))
                return [f"/* {ds:08X}: {format_insn(d)} */"] + add(stmts, r, w, e, False, kill, kl)
            stmts, r, w, e, v = self.lift(d, dconsts, dwide)
            it.seq.append((d, w))
            return [f"/* {ds:08X}: {format_insn(d)} */"] + add(stmts, r, w, e, v, kill)

        def link(reg):
            it.pre.append(f"r{reg} = 0x{pc + 8:08X}u; r{reg}_1 = 0u;")
            it.seq.append((None, {("r", reg, 0), ("r", reg, 1)}))
            b = BIT[("r", reg, 0)] | BIT[("r", reg, 1)]
            it.writes |= b
            it.kills |= b
            it.effects.append({reg: "sext"})

        if op in COND_BRANCHES:
            t = i.branch_target
            local = t in f.labels
            likely = op in LIKELY
            it.cond = _branch_cond(lf, i, wide)
            take_reads()
            if op in LINK_BRANCHES:
                it.kind = "linkbranch"
                link(31)
                it.pre += delay_code(kill=not likely)
                it.target = t
                it.sync = "call"
                it.succ = [pc + 8]
                it.fall = pc + 8
                return it
            if is_unconditional_branch(i):
                it.kind = "jump" if local else "tail"
                it.pre = delay_code()
                it.target = t
                if local:
                    it.succ = [t]
                else:
                    it.sync = "exit"
                return it
            it.kind = "likely" if likely else "branch"
            it.pre = delay_code(kill=not likely)
            it.target = t
            it.fall = pc + 8
            it.succ = [pc + 8]
            if local:
                it.succ.append(t)
            else:
                it.sync = "exit_taken"
            return it

        if op == "j":
            t = i.target
            local = t in f.labels
            it.kind = "jump" if local else "tail"
            it.pre = delay_code()
            it.target = t
            if local:
                it.succ = [t]
            else:
                it.sync = "exit"
            return it

        if op == "jal":
            it.kind = "call"
            link(31)
            it.pre += delay_code()
            it.target = i.target
            it.sync = "call"
            it.succ = [pc + 8]
            it.fall = pc + 8
            return it

        if op == "jalr":
            it.kind = "icall"
            tgt = lf.g(i.rs)
            take_reads()
            it.pre.append(f"uint32_t _tgt = {tgt};")
            if i.rd:
                link(i.rd)
            it.pre += delay_code()
            it.sync = "call"
            it.succ = [pc + 8]
            it.fall = pc + 8
            return it

        if op == "jr":
            if i.rs == 31:
                it.kind = "ret"
                it.pre = delay_code()
                it.sync = "exit"
                return it
            tgt = lf.g(i.rs)
            take_reads()
            it.pre.append(f"uint32_t _tgt = {tgt};")
            it.pre += delay_code()
            if pc in f.jump_tables:
                it.kind = "switch"
                it.table = f.jump_tables[pc]
                it.succ = sorted(set(it.table))
            else:
                it.kind = "itail"
                it.sync = "exit"
            return it

        if op == "syscall":
            it.kind = "syscall"
            it.sync = "call"
            it.succ = [pc + 4]
            it.fall = pc + 4
            return it

        if op == "eret":
            it.kind = "ret"
            it.sync = "exit"
            return it

        pat = self.lift_patched(i, sites[pc], consts, wide) if pc in sites else None
        if pat is not None:
            stmts, r, w, kl, e = pat
            it.seq.append((None, w))
            it.pre = add(stmts, r, w, e, False, True, kl)
            it.succ = [pc + 4]
            it.fall = pc + 4
            return it
        stmts, r, w, e, v = self.lift(i, consts, wide)
        it.seq.append((i, w))
        it.pre = add(stmts, r, w, e, v)
        if v:
            it.kind = "vu0call"
            it.sync = "call"
        it.succ = [pc + 4]
        it.fall = pc + 4
        return it

    # -- per-function dataflow -------------------------------------------------------------

    def prepare(self):
        """Lift twice: once to learn 64-bit effects, then with the wide state known."""
        items, order = self.build_items({})
        wide_at = self.wide_dataflow(items, order)
        consts_at = self.const_dataflow(items, order)
        self.items, self.order = self.build_items(wide_at, consts_at)
        self.preds = {pc: [] for pc in self.order}
        for pc in self.order:
            for s in self.items[pc].succ:
                if s in self.preds:
                    self.preds[s].append(pc)

    def wide_dataflow(self, items, order):
        preds = {pc: [] for pc in order}
        for pc in order:
            for s in items[pc].succ:
                if s in preds:
                    preds[s].append(pc)
        out = {}
        inn = {}
        changed = True
        while changed:
            changed = False
            for pc in order:
                it = items[pc]
                w = set(ENTRY_WIDE) if pc == self.f.start and self.prog.wide64 else set()
                for p in preds[pc]:
                    w |= out.get(p, frozenset())
                w = frozenset(w)
                inn[pc] = w
                o = _apply_effects(w, it.effects)
                if it.sync == "call":
                    # callee-saved registers keep their state, v0/v1 may hold a 64-bit result
                    o = (o & CALLEE_SAVED_GPRS) | (RETURN_WIDE if self.prog.wide64 else frozenset())
                elif it.kind == "likely":
                    o = o | w           # the delay slot only runs when the branch is taken
                if out.get(pc) != o:
                    out[pc] = o
                    changed = True
        return inn

    def callee_summary(self, it, summaries):
        """(live_in, may_write) of whatever a call-like item invokes."""
        k = it.kind
        if k in ("call", "linkbranch", "tail") or (k in ("branch", "likely") and it.sync == "exit_taken"):
            if it.target in self.prog.overrides:
                return override_summary(self.prog, it.target)
            s = summaries.get(it.target)
            if s is None:
                return ALL, ALL
            return s
        if k == "syscall":
            return SYSCALL_LIVE_IN, SYSCALL_MAY_WRITE
        if k == "vu0call":
            return VU0_ALL, VU0_ALL
        # Through a pointer: what the functions whose address is taken read and write between them
        # (prog.ind_summary, part of the summaries' fixpoint; None: anything), less the registers all
        # of them preserve.
        li, mw = self.prog.ind_summary or (ALL, ALL)
        return li & ~self.prog.ind_safe, mw & ~self.prog.ind_safe & ~self.prog.ind_safe_mw

    def dataflow(self, summaries):
        items, order, preds = self.items, self.order, self.preds
        # forward: may-dirty
        dirty_out = {pc: 0 for pc in order}
        for pc in order:
            items[pc].dirty_in = 0
        changed = True
        while changed:
            changed = False
            for pc in order:
                it = items[pc]
                d = 0
                for p in preds[pc]:
                    d |= dirty_out[p]
                it.dirty_in = d
                if it.sync == "call":
                    li, mw = self.callee_summary(it, summaries)
                    o = (d | it.writes) & ~(li | mw)
                else:
                    o = d | it.writes
                if o != dirty_out[pc]:
                    dirty_out[pc] = o
                    changed = True
        self.dirty_out = dirty_out
        # backward: liveness. `live` counts ctx reads by callees too (for the summary);
        # `llive` only counts reads of the C locals (decides loads).
        for pc in order:
            it = items[pc]
            it.live_in = it.llive_in = 0
            it.live_out = it.llive_out = 0
        rev = list(reversed(order))
        changed = True
        while changed:
            changed = False
            for pc in rev:
                it = items[pc]
                lo = llo = 0
                for s in it.succ:
                    if s in items:
                        lo |= items[s].live_in
                        llo |= items[s].llive_in
                if it.fall is not None and it.fall not in items:
                    # running off the end tail-calls the next function after storing dirty state
                    fst = self.fall_stores(it, summaries)
                    nxt = summaries.get(it.fall)
                    lo |= fst | (nxt[0] if nxt else ALL)
                    llo |= fst
                it.live_out, it.llive_out = lo, llo
                stored = self.stored(it, summaries)
                if it.sync == "call":
                    li_c, mw_c = self.callee_summary(it, summaries)
                    li = it.reads | ((stored | li_c | (lo & ~mw_c)) & ~it.kills)
                    lli = it.reads | ((stored | (llo & ~mw_c)) & ~it.kills)
                elif it.sync == "exit":
                    li_c = self.callee_summary(it, summaries)[0] if it.kind in ("tail", "itail") else 0
                    li = it.reads | ((stored | li_c) & ~it.kills)
                    lli = it.reads | (stored & ~it.kills)
                elif it.sync == "exit_taken":
                    li_c = self.callee_summary(it, summaries)[0]
                    li = it.reads | ((stored | li_c | lo) & ~it.kills)
                    lli = it.reads | ((stored | llo) & ~it.kills)
                else:
                    li = it.reads | (lo & ~it.kills)
                    lli = it.reads | (llo & ~it.kills)
                if li != it.live_in or lli != it.llive_in:
                    it.live_in, it.llive_in = li, lli
                    changed = True

    def stored(self, it, summaries):
        """Resources written back to the context at this item's sync point."""
        if it.sync is None:
            return 0
        d = (it.dirty_in | it.writes) & ~self.private
        if it.sync == "call":
            li, mw = self.callee_summary(it, summaries)
            if li == ALL:
                return d
            # a clobbered scratch register only needs storing if we read it afterwards
            return d & (li | (mw & (it.live_out | ~CALL_CLOBBER)))
        if it.kind == "ret":
            return d & ~TEMPS & ~(RETVALS & ~self.ret_live)
        if it.kind == "itail":
            return d
        li = self.callee_summary(it, summaries)[0]
        return d & (li | (~TEMPS & ~self.ret_dead(it.target)))

    def fall_stores(self, it, summaries):
        """Stores before tail-calling the next function when execution runs off the end."""
        nxt = override_summary(self.prog, it.fall) if it.fall in self.prog.overrides else summaries.get(it.fall)
        li = nxt[0] if nxt else ALL
        return self.dirty_out[it.pc] & (li | (~TEMPS & ~self.ret_dead(it.fall))) & ~self.private

    def summary(self, summaries):
        """(live_in at entry, may_write) of this function."""
        entry = self.items.get(self.f.start)
        live_in = entry.live_in if entry else 0
        mw = 0
        for it in self.items.values():
            mw |= it.writes
            if it.sync in ("call", "exit", "exit_taken") and it.kind not in ("ret",):
                mw |= self.callee_summary(it, summaries)[1]
            if it.fall is not None and it.fall not in self.items:
                nxt = summaries.get(it.fall)
                mw |= nxt[1] if nxt else ALL
        return live_in & ~self.drop_li, mw & ~self.drop_mw

    # -- return values --------------------------------------------------------------------

    def ret_dead(self, target):
        """Return registers that need no store before leaving for function `target` (a jump to it, or
        running on into it): no caller reads them after this function, or the target sets them anyway."""
        must = 0 if target in self.prog.overrides else self.prog.must_ret.get(target, 0)
        return RETVALS & ~(self.ret_live & ~must)

    def direct_target(self, it):
        """The function a call-like item calls by its address (None: through a pointer, a native, none)."""
        if it.kind in ("call", "linkbranch", "tail") or (it.kind in ("branch", "likely") and it.sync == "exit_taken"):
            return None if it.target in self.prog.overrides else it.target
        return None

    def must_ret(self, must, summaries):
        """The return registers that hold a value made by this function (or by a function it called
        since) at every return: forwards, those written on every path. A call takes away what the
        callee may write and adds what the callee always leaves."""
        items, order, preds = self.items, self.order, self.preds
        out = {pc: RETVALS for pc in order}
        res = RETVALS
        changed = True
        while changed:
            changed = False
            res = RETVALS
            for pc in order:
                it = items[pc]
                d = 0 if pc == self.f.start else RETVALS
                for p in preds[pc]:
                    d &= out[p]
                if pc == self.f.start:
                    d = 0
                d |= it.kills & RETVALS
                exits = []
                if it.sync == "call":
                    mw = self.callee_summary(it, summaries)[1]
                    m = must.get(self.direct_target(it), 0)
                    d = d & ~mw if it.kind == "linkbranch" else (d & ~mw) | m
                elif it.kind == "ret":
                    exits.append(d)
                elif it.kind == "itail":
                    exits.append(0)
                elif it.kind == "tail" or it.sync == "exit_taken":
                    mw = self.callee_summary(it, summaries)[1]
                    exits.append((d & ~mw) | must.get(self.direct_target(it), 0))
                if it.fall is not None and it.fall not in items and it.kind not in ("jump", "tail", "ret", "itail",
                                                                                     "switch"):
                    nxt = override_summary(self.prog, it.fall) if it.fall in self.prog.overrides else summaries.get(it.fall)
                    mw = nxt[1] if nxt else ALL
                    exits.append((d & ~mw) | (0 if it.fall in self.prog.overrides else must.get(it.fall, 0)))
                for x in exits:
                    res &= x
                if out[pc] != d:
                    out[pc] = d
                    changed = True
        return res

    def ret_liveness(self, must, summaries):
        """[(function or None for a pointer, return registers)]: for each call, the return registers that
        may be read after it comes back (in this function, or after it returns: self.ret_live); for each
        jump to another function, what this function's callers read. Backwards; a call ends the life of
        what the callee always leaves."""
        items, order = self.items, self.order
        rin = {pc: 0 for pc in order}
        sites = {}
        changed = True
        while changed:
            changed = False
            for pc in reversed(order):
                it = items[pc]
                lo = 0
                for s in it.succ:
                    if s in items:
                        lo |= rin[s]
                if it.fall is not None and it.fall not in items and it.kind not in ("jump", "tail", "ret", "itail",
                                                                                     "switch"):
                    # running on into the next function: a jump to it
                    nxt = override_summary(self.prog, it.fall) if it.fall in self.prog.overrides else summaries.get(it.fall)
                    t = None if it.fall in self.prog.overrides else it.fall
                    sites[(pc, "fall")] = (t, self.ret_live)
                    lo |= ((nxt[0] if nxt else ALL) & RETVALS) | (self.ret_live & ~must.get(t, 0))
                if it.sync == "call":
                    # (through a pointer: what the functions whose address is taken read of them, none as a rule)
                    li = self.prog.ind_li_ret if it.kind == "icall" else self.callee_summary(it, summaries)[0] & RETVALS
                    t = self.direct_target(it)
                    if it.kind in ("call", "linkbranch", "icall"):
                        sites[(pc, "call")] = (t, lo)
                    before = li | (lo if it.kind == "linkbranch" else lo & ~must.get(t, 0))
                elif it.kind == "ret":
                    before = self.ret_live
                elif it.kind == "itail":
                    sites[(pc, "tail")] = (None, self.ret_live)
                    before = self.prog.ind_li_ret | self.ret_live
                elif it.kind == "tail" or it.sync == "exit_taken":
                    li = self.callee_summary(it, summaries)[0] & RETVALS
                    t = self.direct_target(it)
                    sites[(pc, "tail")] = (t, self.ret_live)
                    taken = li | (self.ret_live & ~must.get(t, 0))
                    before = taken if it.kind == "tail" else taken | lo
                else:
                    before = lo
                x = (it.reads & RETVALS) | (before & ~it.kills)
                if x != rin[pc]:
                    rin[pc] = x
                    changed = True
        return list(sites.values())

    # -- emission -------------------------------------------------------------------------

    @staticmethod
    def stores(v):
        return [f"{res_ctx(r)} = {res_name(r)};" for r in unbits(v)]

    @staticmethod
    def loads(v):
        return [f"{res_name(r)} = {res_ctx(r)};" for r in unbits(v)]

    def call_expr(self, target):
        if target in self.prog.overrides:
            return f"{self.prog.overrides[target]}(c);"
        if target in self.prog.funcs:
            return f"{self.prog.prefix}{target:08X}(c);"
        return f"ps2_call(c, 0x{target:08X}u);"

    def emit(self, summaries):
        f = self.f
        items, order = self.items, self.order
        used = 0
        for it in items.values():
            used |= it.reads | it.writes | self.stored(it, summaries) | it.llive_in
            if it.fall is not None and it.fall not in items:
                used |= self.fall_stores(it, summaries)
        out = []
        nm = self.names.get(f.start)
        out.append(f"/* {nm} */" if nm else f"/* function at 0x{f.start:08X} */")
        wrap = f.start in self.prog.debug_wrap
        fname = f"{self.fprefix or self.prog.prefix}{f.start:08X}"
        io_fn = f.start in self.prog.io_funcs
        if io_fn:   # reaches hardware through pointers: keep the I/O check here
            out.append("#if !defined(PS2_DYNAMIC_IO)\n#undef PS2_FN_IO\n#define PS2_FN_IO 1\n#endif")
        hot = "PS2_HOT " if f.start in self.prog.hot_funcs else ""
        out.append(f"{'static ' if wrap else ''}{hot}void {fname}{'_body' if wrap else ''}(R5900 *c)")
        out.append("{")
        decls = {}
        for r in unbits(used):
            decls.setdefault(res_type(r), []).append(res_name(r))
        for t, names in decls.items():
            for k in range(0, len(names), 12):
                out.append(f"    {t} {', '.join(names[k:k + 12])};")
        out.append(f"    PS2_TRACE_ENTRY(0x{f.start:08X}u);")
        hook = self.prog.entry_hooks.get(f.start)
        if hook:    # title fix that runs before the function body (see ENTRY_HOOKS)
            out.append(f"    {{ void {hook}(R5900 *); {hook}(c); }}")
        entry = items.get(f.start)
        if entry:
            out += ["    " + s for s in self.loads(entry.llive_in)]
        labels_needed = set()
        for it in items.values():
            if it.kind in ("jump", "branch", "likely") and it.target in items:
                labels_needed.add(it.target)
            if it.kind == "switch":
                labels_needed |= set(it.table)
        for k, pc in enumerate(order):
            it = items[pc]
            nxt = order[k + 1] if k + 1 < len(order) else None
            if it.fall is not None and it.fall != nxt and it.fall in items and \
                    it.kind not in ("jump", "tail", "ret", "itail", "switch"):
                labels_needed.add(it.fall)
        prof = self.block_sizes(order, labels_needed) if self.fid is not None else {}
        for k, pc in enumerate(order):
            it = items[pc]
            nxt = order[k + 1] if k + 1 < len(order) else None
            if pc in labels_needed:
                out.append(f"L_{pc:08X}:;")
            if pc in prof:
                out.append(f"    PS2_PROF({self.fid}, {prof[pc]});")
            out.append(f"    /* {pc:08X}: {format_insn(it.insn)} */")
            out += ["    " + s for s in self.emit_item(it, summaries)]
            if it.fall is not None and it.kind not in ("jump", "tail", "ret", "itail", "switch"):
                if it.fall != nxt:
                    if it.fall in items:
                        out.append(f"    goto L_{it.fall:08X};")
                    else:
                        st = self.fall_stores(it, summaries)
                        out += ["    " + s for s in self.stores(st) + [self.call_expr(it.fall), "return;"]]
        out.append("}")
        if wrap:
            # debug: log arguments and result (see DEBUG_WRAP in the title config)
            out.append(f"void {fname}(R5900 *c) {{ ps2_dbg_enter(c, 0x{f.start:08X}u); {fname}_body(c); "
                       f"ps2_dbg_leave(c, 0x{f.start:08X}u); }}")
        if io_fn:
            out.append("#if !defined(PS2_DYNAMIC_IO)\n#undef PS2_FN_IO\n#define PS2_FN_IO 0\n#endif")
        return "\n".join(out)

    ENDS_BLOCK = frozenset({"branch", "likely", "linkbranch", "jump", "tail", "ret", "itail", "switch"})

    def block_sizes(self, order, labels):
        """Leader pc -> guest instructions up to the next leader, for PS2_PROF.
        Leaders: the entry, branch targets, and fall-throughs of branches. A
        branch, jump or call item also covers its delay slot."""
        sizes = {}
        lead = None
        prev_ends = True
        for pc in order:
            it = self.items[pc]
            if prev_ends or pc in labels:
                lead = pc
                sizes[pc] = 0
            sizes[lead] += 1 if it.kind in ("normal", "syscall", "vu0call") else 2
            prev_ends = it.kind in self.ENDS_BLOCK
        return sizes

    def emit_item(self, it, summaries):
        k = it.kind
        out = []
        st = self.stored(it, summaries)
        if k == "normal":
            return list(it.pre)
        li_c, mw_c = self.callee_summary(it, summaries) if it.sync == "call" else (0, 0)
        reload = it.llive_out & mw_c
        if it.sync == "call" and self.direct_target(it) is None and it.target in self.prog.override_vu0:
            # (the report of emit_program: the VU0 state a native has to leave right for this call site)
            self.prog.override_vu0_reload.setdefault(it.target, {})[it.pc] = reload & VU0_ALL
        if k == "vu0call":
            body = list(it.pre)
            idx = next(n for n, s in enumerate(body) if "ps2_vu0_call" in s)
            return body[:idx] + self.stores(st) + [body[idx]] + self.loads(reload) + body[idx + 1:]
        if k == "branch":
            out.append(f"{{ int _c = {it.cond};")
            out += it.pre
            if it.sync == "exit_taken":
                out.append("if (_c) {")
                out += self.stores(st)
                out += [self.call_expr(it.target), "return; } }"]
            elif it.target <= it.pc:
                out.append(f"if (_c) {{ PS2_POLL(); goto L_{it.target:08X}; }} }}")
            else:
                out.append(f"if (_c) goto L_{it.target:08X}; }}")
            return out
        if k == "likely":
            out.append(f"if ({it.cond}) {{")
            out += it.pre
            if it.sync == "exit_taken":
                out += self.stores(st)
                out += [self.call_expr(it.target), "return;"]
            else:
                if it.target <= it.pc:
                    out.append("PS2_POLL();")
                out.append(f"goto L_{it.target:08X};")
            out.append("}")
            return out
        if k == "jump":
            return it.pre + (["PS2_POLL();"] if it.target <= it.pc else []) + [f"goto L_{it.target:08X};"]
        if k == "tail":
            return it.pre + self.stores(st) + [self.call_expr(it.target), "return;"]
        if k == "ret":
            return it.pre + self.stores(st) + ["return;"]
        if k == "call":
            return it.pre + self.stores(st) + [self.call_expr(it.target)] + self.loads(reload)
        if k == "linkbranch":
            out.append(f"{{ int _c = {it.cond};")
            out += it.pre + self.stores(st)
            out.append(f"if (_c) {self.call_expr(it.target)} }}")
            return out + self.loads(reload)
        if k == "icall":
            return ["{"] + it.pre + self.stores(st) + ["ps2_call(c, _tgt); }"] + self.loads(reload)
        if k == "itail":
            return ["{"] + it.pre + self.stores(st) + ["ps2_call(c, _tgt); return; }"]
        if k == "switch":
            out.append("{")
            out += it.pre
            out.append("switch (_tgt) {")
            for t in sorted(set(it.table)):
                out.append(f"case 0x{t:08X}u: goto L_{t:08X};")
            out.append(f"default: ps2_bad_jump(c, 0x{it.pc:08X}u, _tgt); return; }} }}")
            return out
        if k == "syscall":
            return self.stores(st) + [f"ps2_syscall(c, 0x{it.pc:08X}u);"] + self.loads(reload)
        raise RuntimeError(k)


def compute_summaries(emitters, log=print, prog=None, taken=None):
    """Whole-program fixpoint of (live_in, may_write) per function (worklist). With taken (the functions
    whose address is taken), a call through a pointer reads and writes what those do between them, which
    is part of the fixpoint; without, anything."""
    callers = {s: set() for s in emitters}
    ind_users = set()
    if taken is not None:
        prog.ind_summary = OVERRIDE_SUMMARY     # (a native may be called through a pointer too)
        if prog.override_vu0 & taken:
            prog.ind_summary = (prog.ind_summary[0], prog.ind_summary[1] | VU0_ALL)
        for s, e in emitters.items():
            if any(it.kind in ("icall", "itail") for it in e.items.values()):
                ind_users.add(s)
    for s, e in emitters.items():
        for it in e.items.values():
            t = None
            if it.kind in ("call", "linkbranch", "tail") or it.sync == "exit_taken":
                t = it.target
            if t in callers:
                callers[t].add(s)
            if it.fall is not None and it.fall not in e.items and it.fall in callers:
                callers[it.fall].add(s)
    summaries = {s: (0, 0) for s in emitters}
    work = sorted(emitters, reverse=True)
    queued = set(work)
    steps = 0
    while work:
        s = work.pop()
        queued.discard(s)
        e = emitters[s]
        e.dataflow(summaries)
        li, mw = e.summary(summaries)
        old = summaries[s]
        new = (old[0] | li, old[1] | mw)
        steps += 1
        if new != old:
            summaries[s] = new
            for c in callers[s]:
                if c not in queued:
                    queued.add(c)
                    work.append(c)
            if taken is not None and s in taken:
                ind = (prog.ind_summary[0] | new[0], prog.ind_summary[1] | new[1])
                if ind != prog.ind_summary:
                    prog.ind_summary = ind
                    for c in ind_users:
                        if c not in queued:
                            queued.add(c)
                            work.append(c)
    log(f"summaries converged after {steps} function analyses")
    return summaries


def return_values(prog, title, emitters, summaries, log=print):
    """Sets each function's ret_live and prog.must_ret (see the top of the file)."""
    callers = {s: set() for s in emitters}
    for s, e in emitters.items():
        for it in e.items.values():
            t = e.direct_target(it)
            if t in callers:
                callers[t].add(s)
            if it.fall is not None and it.fall not in e.items and it.fall in callers:
                callers[it.fall].add(s)
    # what a function always leaves: from everything, taking away
    must = {s: RETVALS for s in emitters}
    work = set(emitters)
    while work:
        nxt = set()
        for s in work:
            m = emitters[s].must_ret(must, summaries)
            if m != must[s]:
                must[s] = m
                nxt |= callers[s]
        work = nxt
    prog.must_ret = must
    # what callers read: from nothing, adding. A function whose address is taken may be called by the
    # runtime (callbacks, handlers, thread entries: v0 read there) and through any pointer in the game;
    # one a native calls by name (the overridden ones' originals, NATIVE_CALLS) keeps them all.
    taken = ({v for _, v in prog.data_code_ptrs if v in emitters} | {a for a in prog.code_ptr_refs if a in emitters})
    taken -= set(getattr(title, "ABI_EXEMPT", ()))
    prog.ind_li_ret = (prog.ind_summary[0] if prog.ind_summary else ALL) & RETVALS
    v0 = bits(("r", 2, k) for k in (0, 1))
    live = {s: 0 for s in emitters}
    for s in taken:
        live[s] = v0
    for s in set(prog.overrides) | set(getattr(title, "NATIVE_CALLS", ())):
        if s in live:
            live[s] = RETVALS
    ind = 0
    work = set(emitters)
    while work:
        nxt = set()
        for s in sorted(work):
            e = emitters[s]
            e.ret_live = live[s]
            for t, b in e.ret_liveness(must, summaries):
                if t is None:
                    if b & ~ind:
                        ind |= b
                        for x in taken:
                            if ind & ~live[x]:
                                live[x] |= ind
                                nxt.add(x)
                elif t in live and b & ~live[t]:
                    live[t] |= b
                    nxt.add(t)
        work = nxt
    for s, e in emitters.items():
        e.ret_live = live[s]
    v0_0, v1_0, f0 = BIT[("r", 2, 0)], BIT[("r", 3, 0)], BIT[("f", 0)]
    log(f"return values: of {len(emitters)} functions {sum(1 for v in live.values() if v & v0_0)} have v0 read by a "
        f"caller, {sum(1 for v in live.values() if v & v1_0)} v1, {sum(1 for v in live.values() if v & f0)} f0; "
        f"through pointers: {' '.join(res_name(r) for r in unbits(ind)) or 'none'}; read by a function called "
        f"through one: {' '.join(res_name(r) for r in unbits(prog.ind_li_ret)) or 'none'}")


def emit_program(prog, title, out_dir, per_file, names=None, log=print):
    names = names or {}
    os.makedirs(out_dir, exist_ok=True)
    funcs = [prog.funcs[s] for s in sorted(prog.funcs)]
    emitters = {}
    for f in funcs:
        e = FuncEmitter(prog, f, names)
        e.fid = len(emitters) if prog.table == "ps2_func_table" else None   # profiling id (EE only)
        e.prepare()
        emitters[f.start] = e
    log("lifted all functions")
    prog.ind_safe = prog.ind_safe_mw = 0
    prog.must_ret = {}
    prog.ind_li_ret = RETVALS
    prog.ind_summary = None
    taken = None
    if title is not None and getattr(title, "IND_SUMMARY", True):
        taken = ({v for _, v in prog.data_code_ptrs if v in emitters} | {a for a in prog.code_ptr_refs if a in emitters})
        taken -= set(getattr(title, "ABI_EXEMPT", ()))
    if title is not None and getattr(title, "ABI_SAVES", True):
        # callee-saved registers that need not go through the context (abi.py)
        lanes = lambda n: BIT[n] if isinstance(n, tuple) else bits(("r", n, k) for k in range(4))    # noqa: E731
        ab = abi.Abi(prog, FullLift(), prog.overrides, getattr(title, "ABI_EXEMPT", ()),
                     getattr(title, "ABI_FOLLOWS", ()), getattr(title, "NORETURN", ()), log)
        for n in ab.ind_safe:
            prog.ind_safe |= lanes(n)
        if ab.sp_ind:
            prog.ind_safe_mw = lanes(29)    # sp is read by every callee, and left as it was
        for a, e in emitters.items():
            skip = set()
            if a in ab.sp_preserves:
                e.drop_mw |= lanes(29)
            for n in abi.REGS + abi.FREGS:
                if a in ab.preserves[n]:
                    e.drop_mw |= lanes(n)
                    if a not in ab.consumes[n]:
                        e.drop_li |= lanes(n)
                pcs = ab.neutral[n].get(a)
                if pcs:
                    skip |= pcs
                    e.private |= lanes(n)
            if skip:
                e.skip_pcs = frozenset(skip)
                e.prepare()
    summaries = compute_summaries(emitters, log, prog, taken)
    if title is not None and getattr(title, "RET_LIVE", True):
        # the return registers nobody reads are not written back; then the summaries again, with the
        # entry loads that only fed those stores gone
        return_values(prog, title, emitters, summaries, log)
        summaries = compute_summaries(emitters, log, prog, taken)
    if prog.ind_summary:
        log("a call through a pointer reads " + " ".join(sorted({res_name(r)[:3].rstrip("_") for r in unbits(prog.ind_summary[0])})))
    for e in emitters.values():
        e.dataflow(summaries)
    with open(os.path.join(out_dir, "funcs.h"), "w", newline="\n") as h:
        h.write("/* generated: prototypes of recompiled functions */\n#pragma once\n")
        for line in prog.defines:
            h.write(line + "\n")
        h.write("#include \"ps2_recomp.h\"\n")
        for inc in getattr(title, "EXTRA_INCLUDES", ()):
            h.write(f"#include \"{inc}\"\n")
        for f in funcs:
            h.write(f"void {prog.prefix}{f.start:08X}(R5900 *c);\n")
        for a in sorted(prog.inline_variants):
            h.write(f"void n_{a:08X}(R5900 *c);   /* {prog.prefix}{a:08X} with its VCALLMS_INLINE sites run inline */\n")
        for name in sorted(set(prog.overrides.values())):
            h.write(f"void {name}(R5900 *c);   /* native override */\n")
    with open(os.path.join(out_dir, "functable.c"), "w", newline="\n") as t:
        t.write("/* generated: address -> function table, sorted */\n#include \"funcs.h\"\n")
        t.write(f"const PS2FuncEntry {prog.table}[] = {{\n")
        for f in funcs:
            fn = prog.overrides.get(f.start, f"{prog.prefix}{f.start:08X}")
            t.write(f"    {{0x{f.start:08X}u, {fn}}},\n")
        t.write("};\n")
        count = "ps2_func_count" if prog.table == "ps2_func_table" else prog.table + "_count"
        t.write(f"const unsigned {count} = {len(funcs)};\n")
        if prog.table == "ps2_func_table":   # guest instructions executed per function
            t.write(f"#if defined(PS2_PROFILE)\nuint64_t g_prof[{len(funcs)}];\n#endif\n")
    files = []
    for n in range(0, len(funcs), per_file):
        chunk = funcs[n:n + per_file]
        path = os.path.join(out_dir, f"recomp_{n // per_file:03d}.c")
        files.append(path)
        with open(path, "w", newline="\n") as o:
            o.write("/* generated by tools/recomp: do not edit */\n#include \"funcs.h\"\n\n")
            for f in chunk:
                o.write(emitters[f.start].emit(summaries))
                o.write("\n\n")
    for a in sorted(prog.override_vu0_reload):
        sites = prog.override_vu0_reload[a]
        rel = 0
        for v in sites.values():
            rel |= v
        log(f"OVERRIDE_VU0 0x{a:08X} ({prog.overrides[a]}): {len(sites)} call sites; VU0 state read after the call: "
            + (" ".join(res_name(r) for r in unbits(rel)) or "none"))
        for pc in sorted(sites):
            log(f"    call at 0x{pc:08X}: " + (" ".join(res_name(r) for r in unbits(sites[pc])) or "none"))
    if prog.inline_variants:
        # the same function again with its VCALLMS_INLINE sites inline, under the name n_XXXXXXXX: the recompiled
        # one stays as the reference (the runtime's same-state checks) and shares its summary and ABI decisions
        path = os.path.join(out_dir, "recomp_v00.c")
        files.append(path)
        with open(path, "w", newline="\n") as o:
            o.write("/* generated by tools/recomp: do not edit */\n#include \"funcs.h\"\n\n")
            for a in sorted(prog.inline_variants):
                e0 = emitters[a]
                v = FuncEmitter(prog, e0.f, names)
                v.lifter.vcall_inline = prog.vcall_inline
                v.fprefix = "n_"
                v.skip_pcs, v.private, v.drop_li, v.drop_mw, v.ret_live = \
                    e0.skip_pcs, e0.private, e0.drop_li, e0.drop_mw, e0.ret_live
                v.prepare()
                v.dataflow(summaries)
                o.write(v.emit(summaries))
                o.write("\n\n")
    log(f"wrote {len(files)} source files to {out_dir}")
