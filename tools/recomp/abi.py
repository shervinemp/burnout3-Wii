"""Callee-saved registers that need not go through the context.

A recompiled function keeps the guest registers it uses in C locals and
synchronises them with the context around calls. A callee-saved register
(s0..s7, fp, ra) costs the most there: the function loads it from the
context only to store it on the guest stack (`sq`: four words), and at its
end loads it back and stores it to the context, sixteen memory operations a
register a call, for a value none of its callees and none of its callers
ever reads from the context.

For every function X and such register N this follows the code with a small
state: is N saved in a stack slot, may it still hold the caller's value, may
it hold one of X's own.

  preserves(X, N): N holds the caller's value again wherever X leaves (it was
      restored from the slot it was saved in, or never written), also across
      calls made while it was not saved, whose callees must preserve it too.
  consumes(X, N): X may read N while it still holds the caller's value (a
      variable used before it is set, or code that is not a function of its
      own), or calls something that does while it does.

A caller need not store N before calling a function that does not consume it,
nor reload N after calling one that preserves it. A function that preserves N
through a save of its own, and calls only functions that preserve and do not
consume it, is "neutral" for N: the save and the restores are left out, N is
a C local that is never written to the context (and the lanes above bit 63,
which only the `sq` read, are gone with them).

Calls through pointers reach the functions whose address is taken (minus the
title's ABI_EXEMPT: exception entries and the like); N is safe across them
when all of those preserve and none consumes it.
"""
from r5900 import COND_BRANCHES, LIKELY, is_unconditional_branch

REGS = (16, 17, 18, 19, 20, 21, 22, 23, 30, 31)
FREGS = tuple(("f", n) for n in range(20, 32))      # the callee-saved FPU registers, as keys
SAVE_OPS = {"sq": 16, "sd": 8, "sw": 4}
LOAD_OPS = {"lq": 16, "ld": 8, "lw": 4}
PAIR = {"lq": "sq", "ld": "sd", "lw": "sw", "lwc1": "swc1"}
FSAVE_OPS = {"swc1": 4}
FLOAD_OPS = {"lwc1": 4}
SLOT_SIZE = {"sq": 16, "sd": 8, "sw": 4, "swc1": 4}
STORE_SIZE = {"sq": 16, "sd": 8, "sw": 4, "sh": 2, "sb": 1, "swc1": 4, "sqc2": 16, "swl": 4, "swr": 4, "sdl": 8,
              "sdr": 8}
LOAD_SIZE = {"lq": 16, "ld": 8, "lw": 4, "lwu": 4, "lh": 2, "lhu": 2, "lb": 1, "lbu": 1, "lwc1": 4, "lqc2": 16,
             "lwl": 4, "lwr": 4, "ldl": 8, "ldr": 8}


class Node:
    __slots__ = ("pc", "rd", "wr", "frd", "fwr", "call", "succ", "insn", "ret")

    def __init__(self, pc):
        self.pc = pc
        self.rd = self.wr = 0       # GPR bitmasks (bit n)
        self.frd = self.fwr = 0     # FPU registers
        self.call = None            # ('f', address) | ('ind',) | ('sys',)
        self.succ = []
        self.insn = None
        self.ret = False            # the function is left for the callee (a tail jump)


def build(prog, f, lifter, noreturn=()):
    """The function as one node an instruction (delay slots where they run) and one a call; node 0 is its
    return. noreturn: functions that never come back (the code behind a call of one is not run on into)."""
    nodes = [Node(0)]
    index = {}

    def get(key):
        k = index.get(key)
        if k is None:
            k = index[key] = len(nodes)
            nodes.append(Node(key[1]))
        return k

    def tail(pc):
        k = get(("tail", pc))
        nodes[k].call = ("f", pc)
        nodes[k].ret = True
        nodes[k].succ = [0]
        return k

    def plain(pc):
        return get(("i", pc)) if pc in f.insns else tail(pc)

    sites = prog.code_patches

    def fork(k, pc, after):
        """A code patch's site (patches.py) at node k: k does nothing and goes on to the original instruction
        and to the instructions run instead (a node each, with their own pcs), both of which go on to after."""
        site = sites[pc]
        o = get(("po", pc))
        effects(nodes[o], f.insns[pc])
        nodes[o].succ = list(after)
        nxt = list(after)
        for m, i in reversed(list(enumerate(site.alt))):
            a = get(("pa", i.pc, pc, m))
            effects(nodes[a], i)
            nodes[a].succ = nxt
            nxt = [a]
        nodes[k].insn = None
        nodes[k].rd = nodes[k].wr = nodes[k].frd = nodes[k].fwr = 0
        nodes[k].succ = [o] + nxt

    def effects(n, i):
        op = i.op
        n.insn = i
        if op in COND_BRANCHES:
            n.rd |= 1 << i.rs
            if op in ("beq", "bne", "beql", "bnel"):
                n.rd |= 1 << i.rt
            return
        if op in ("jr", "jalr"):
            if not (op == "jr" and i.rs == 31):     # (the return is no use of ra's value)
                n.rd |= 1 << i.rs
            return
        if op in ("j", "jal", "syscall", "eret", "break"):
            return
        try:
            _, reads, writes, _ = lifter.lift(i, {}, frozenset())
        except Exception:               # noqa: BLE001
            return
        for r in reads:
            if r[0] == "r":
                n.rd |= 1 << r[1]
            elif r[0] == "f":
                n.frd |= 1 << r[1]
        for r in writes:
            if r[0] == "r":
                n.wr |= 1 << r[1]
            elif r[0] == "f":
                n.fwr |= 1 << r[1]

    for pc in sorted(f.insns):
        i = f.insns[pc]
        op = i.op
        if pc in f.delay_slots and pc not in f.labels:
            continue
        n = nodes[get(("i", pc))]
        effects(n, i)
        if op in COND_BRANCHES or op in ("j", "jal", "jr", "jalr"):
            d = get(("d", pc + 4))
            effects(nodes[d], f.insns[pc + 4])
            after = []
            if op in COND_BRANCHES:
                t = i.branch_target
                after = [plain(t) if t in f.labels else tail(t)]
                if op in ("bltzal", "bgezal", "bltzall", "bgezall"):
                    n.wr |= 1 << 31
                    c = get(("call", pc))
                    nodes[c].call = ("f", t)
                    nodes[c].succ = [plain(pc + 8)]
                    after = [c]
                if op in LIKELY:
                    n.succ = [d, plain(pc + 8)]
                    nodes[d].succ = after
                    if pc + 4 in sites:
                        fork(d, pc + 4, after)
                    continue
                if not is_unconditional_branch(i):
                    after.append(plain(pc + 8))
            elif op == "j":
                t = i.target
                after = [plain(t)] if t in f.labels else [tail(t)]
            elif op in ("jal", "jalr"):
                # the link is written before the delay slot runs, the call is after it
                n.wr |= 1 << (31 if op == "jal" else i.rd)
                c = get(("call", pc))
                nodes[c].call = ("f", i.target) if op == "jal" else ("ind",)
                nodes[c].succ = [] if op == "jal" and i.target in noreturn else [plain(pc + 8)]
                after = [c]
            else:                               # jr
                tbl = f.jump_tables.get(pc)
                if tbl:
                    after = [plain(t) for t in tbl]
                elif i.rs == 31:
                    after = [0]
                else:
                    c = get(("itail", pc))
                    nodes[c].call = ("ind",)
                    nodes[c].ret = True
                    nodes[c].succ = [0]
                    after = [c]
            n.succ = [d]
            nodes[d].succ = after
            if pc + 4 in sites:
                fork(d, pc + 4, after)
        elif op == "eret":
            n.succ = [0]
        elif op in ("syscall", "vcallms", "vcallmsr"):
            n.call = ("sys",)
            n.succ = [plain(pc + 4)]
        else:
            n.succ = [plain(pc + 4)]
            if pc in sites:
                fork(get(("i", pc)), pc, n.succ)
    return nodes


def reg_name(n):
    return "f%d" % n[1] if isinstance(n, tuple) else "r%d" % n


BAD = "BAD"
ENTRY = (None, True, False)         # (slot, may hold the caller's value, may hold the function's own)


def _merge(a, b):
    if a is None:
        return b
    if b is None or a == b:
        return a
    if a == BAD or b == BAD:
        return BAD
    slot = a[0]
    if a[0] != b[0]:
        if a[2] or b[2]:
            return BAD
        slot = None
    return (slot, a[1] or b[1], a[2] or b[2])


class Result:
    __slots__ = ("ok", "consumes", "raw", "saves", "restores", "why", "calls_ok", "reads")

    def __init__(self):
        self.ok = True              # preserves
        self.consumes = False
        self.raw = False            # ... without ever having saved it (no function of its own, or assembly)
        self.saves = set()          # pcs of the instructions that save it
        self.restores = set()
        self.why = None             # (pc, reason) where it stops preserving
        self.calls_ok = True        # every callee preserves and does not consume it
        self.reads = None           # pc of a read of the caller's value by the function itself


def follow(nodes, n, callee, start=ENTRY, cont=None):
    """Register n through a function. callee(call) -> (preserves, consumes, consumes it raw). start: the state it is entered
    in; cont(address, state) -> Result of the code a jump leaves for while the register is still saved (the
    analysis split a function in two there: the other part restores it)."""
    res = Result()
    if len(nodes) < 2:
        if start[2]:
            res.ok = False
        return res
    fpr = isinstance(n, tuple)          # ("f", number): an FPU register, saved with swc1
    if fpr:
        n = n[1]
    bit = 1 << n
    save_ops, load_ops = (FSAVE_OPS, FLOAD_OPS) if fpr else (SAVE_OPS, LOAD_OPS)
    state = [None] * len(nodes)
    state[1] = start
    work = [1]

    def bad(pc, reason):
        if res.why is None:
            res.why = (pc, reason)
        res.ok = False
        return BAD

    while work:
        k = work.pop()
        nd = nodes[k]
        s = state[k]
        i = nd.insn
        if s == BAD:
            out = BAD
        else:
            slot, mc, mm = s
            out = None
            sp_rel = i is not None and not nd.call and i.rs == 29
            if sp_rel and i.rt == n and i.op in save_ops and not mm and (slot is None or
                                                                         slot == (i.simm, i.op)):
                # the save (or the same value saved again)
                out = ((i.simm, i.op), mc, mm)
            elif sp_rel and i.rt == n and i.op in load_ops and slot is not None and slot == (i.simm, PAIR[i.op]):
                out = (slot, True, False)
            else:
                if nd.call and nd.ret and (mm or slot is not None) and nd.call[0] == "f" and cont is not None:
                    # the rest of this function, as the analysis has it: followed from here
                    r2 = cont(nd.call[1], s)
                    res.calls_ok = False            # (the pair is left as it is: the register goes through the context)
                    if r2 is None or not r2.ok:
                        out = bad(nd.pc, "left for another function before the restore")
                    else:
                        res.consumes = res.consumes or r2.consumes
                        res.raw = res.raw or r2.raw
                        out = (None, True, False)
                elif nd.call:
                    pres, cons, craw = callee(nd.call)
                    if not pres or cons:
                        res.calls_ok = False
                    if cons and mc:
                        res.consumes = True
                        if slot is None and craw:
                            res.raw = True
                    if nd.ret and mm:
                        out = bad(nd.pc, "left for another function before the restore")
                    elif not pres:
                        if slot is None:
                            out = bad(nd.pc, f"a callee that does not preserve it, and no save {nd.call}")
                        else:
                            mc, mm = False, True
                if out is None and (nd.frd if fpr else nd.rd) & bit and mc:
                    res.consumes = True
                    if slot is None:
                        res.raw = True
                    if res.reads is None:
                        res.reads = nd.pc
                if out is None and (nd.fwr if fpr else nd.wr) & bit:
                    if slot is None:
                        out = bad(nd.pc, "written without a save")
                    else:
                        mc, mm = False, True
                if out is None and i is not None and not nd.call and nd.wr & (1 << 29):
                    if mm:
                        out = bad(nd.pc, "sp moved before the restore")
                    else:
                        slot = None
                if out is None and slot is not None and sp_rel and (i.op in STORE_SIZE or i.op in LOAD_SIZE):
                    size = STORE_SIZE.get(i.op) or LOAD_SIZE[i.op]
                    if i.simm < slot[0] + SLOT_SIZE[slot[1]] and slot[0] < i.simm + size:
                        out = bad(nd.pc, "its stack slot used for something else")
                if out is None:
                    out = (slot, mc, mm)
        for t in nd.succ:
            if t == 0:
                if out != BAD and out[2]:
                    bad(nd.pc, "returns before the restore")
                continue
            m = _merge(state[t], out)
            if m == BAD and state[t] != BAD and out != BAD:
                bad(nodes[t].pc, "paths meet with it saved differently")
            if m != state[t]:
                state[t] = m
                work.append(t)
    if res.ok:
        # which instructions are the save and the restores, by the states as they came out; one that is that
        # in one place and an ordinary store or load in another (a delay slot that is also jumped to) cannot
        # be left out
        other = set()
        for k in range(1, len(nodes)):
            nd, s = nodes[k], state[k]
            i = nd.insn
            if s is None or i is None or nd.call or i.rs != 29 or i.rt != n:
                continue
            slot, mc, mm = s
            if i.op in save_ops and not mm and (slot is None or slot == (i.simm, i.op)):
                res.saves.add(nd.pc)
            elif i.op in load_ops and slot is not None and slot == (i.simm, PAIR[i.op]):
                res.restores.add(nd.pc)
            elif i.op in save_ops or i.op in load_ops:
                other.add(nd.pc)
        if other & (res.saves | res.restores):
            res.calls_ok = False
    return res


class Abi:
    """The verdicts for a program: per register n, preserves[n] and consumes[n] (sets of function addresses),
    neutral[n] = {address: pcs of the save and restores to leave out}, ind_safe (registers that are safe
    across calls through pointers), why[n] = {address: (pc, reason)}."""

    def __init__(self, prog, lifter, overrides=(), exempt=(), follows=(), noreturn=(), log=print):
        noreturn = set(noreturn)
        self.graphs = {a: build(prog, f, lifter, noreturn) for a, f in prog.funcs.items()}
        self.follows = set(follows)     # preserve them all by a prologue this does not read (the title says)
        self.taken = ({v for _, v in prog.data_code_ptrs if v in prog.funcs} |
                      {a for a in prog.code_ptr_refs if a in prog.funcs}) - set(exempt)
        self.overrides = set(overrides)
        self.preserves = {}
        self.consumes = {}
        self.neutral = {}
        self.why = {}
        self.saved = {}                 # functions with a save of it at all (statistics)
        self.reads = {}                 # reads[n] = {address: pc of its own read of the caller's value}
        self.raw = {}                   # raw[n]: functions that use it as it came without ever saving it
        self.ind = {}                   # through pointers: (all preserve it, some consume it)
        self.ind_safe = set()
        callers = {a: set() for a in self.graphs}
        ind_callers = set()
        for a, nodes in self.graphs.items():
            for nd in nodes:
                if nd.call and nd.call[0] == "f" and nd.call[1] in callers:
                    callers[nd.call[1]].add(a)
                elif nd.call and nd.call[0] == "ind":
                    ind_callers.add(a)
        for n in REGS + FREGS:
            self._register(n, callers, ind_callers)
        self._sp(callers, ind_callers)
        log(f"callee-saved registers: {sum(len(v) for v in self.neutral.values())} saves left out in "
            f"{len({a for v in self.neutral.values() for a in v})} functions; safe through pointers: "
            f"{' '.join(reg_name(n) for n in REGS + FREGS if n in self.ind_safe) or 'none'}; sp left as it was "
            f"by {len(self.sp_preserves)} of {len(self.graphs)}{'' if self.sp_ind else ' (NOT through pointers)'}")

    def _sp(self, callers, ind_callers):
        """Which functions leave sp as they found it (then it need not be read back after a call): its
        distance from the entry value through the code, moved by addiu sp,sp,K only, zero at every exit."""
        graphs = self.graphs
        pres = set(graphs)
        ind = [True]
        self.sp_why = {}

        def ok(call):
            if call[0] == "sys":
                return True
            if call[0] == "ind":
                return ind[0]
            t = call[1]
            return t in self.overrides or t in self.follows or t in pres

        def walk(nodes, start, depth):
            """the function from a distance of start: (pc, reason) when sp is not back at 0 at an exit"""
            if len(nodes) < 2:
                return (0, "runs on into nothing") if start else None
            state = [None] * len(nodes)
            state[1] = start
            work = [1]
            while work:
                k = work.pop()
                nd, d = nodes[k], state[k]
                i = nd.insn
                if nd.call:
                    if nd.ret and d != 0 and nd.call[0] == "f" and nd.call[1] in graphs and depth < 6:
                        # the rest of a function the analysis split: on from here
                        bad = walk(graphs[nd.call[1]], d, depth + 1)
                        if bad:
                            return bad
                        d = 0
                    elif not ok(nd.call):
                        return (nd.pc, f"a callee that does not {nd.call}")
                    elif nd.ret and d != 0:
                        return (nd.pc, "left for another function with the frame still there")
                elif i is not None and nd.wr & (1 << 29):
                    if i.op in ("addiu", "daddiu") and i.rs == 29 and i.rt == 29:
                        d += i.simm
                    else:
                        return (nd.pc, "sp set in another way")
                for t in nd.succ:
                    if t == 0:
                        if d != 0:
                            return (nd.pc, "returns with the frame still there")
                    elif state[t] is None:
                        state[t] = d
                        work.append(t)
                    elif state[t] != d:
                        return (nodes[t].pc, "paths meet with different frames")
            return None

        work = set(graphs)
        while work:
            nxt = set()
            for a in work:
                if a not in pres or a in self.follows:
                    continue
                bad = walk(graphs[a], 0, 0)
                if bad:
                    pres.discard(a)
                    self.sp_why[a] = bad
                    nxt |= callers[a]
                    if a in self.taken and ind[0]:
                        ind[0] = False
                        nxt |= ind_callers
            work = nxt
        self.sp_preserves = pres
        self.sp_ind = ind[0]

    def _register(self, n, callers, ind_callers):
        graphs = self.graphs
        pres = set(graphs)
        cons = set()
        ind = [True, False]             # through pointers: all preserve, some consume
        why = {}

        raw = set()

        def callee(call):
            if call[0] == "sys":
                return True, False, False
            if call[0] == "ind":
                return ind[0], ind[1], ind[1]
            t = call[1]
            if t in self.overrides:
                return True, n == 31, n == 31       # a native may ask for the return address
            if t in self.follows:
                return True, False, False
            if t not in graphs:
                return False, True, True
            return t in pres, t in cons, t in raw

        depth = [0]

        def cont(t, state):
            if t not in graphs or depth[0] > 6:
                return None
            depth[0] += 1
            r = follow(graphs[t], n, callee, state, cont)
            depth[0] -= 1
            return r

        # what preserves it: from everything, taking out what does not, and then those who relied on it
        work = set(graphs)
        while work:
            nxt = set()
            for a in work:
                if a not in pres or a in self.follows:
                    continue
                r = follow(graphs[a], n, callee, ENTRY, cont)
                if not r.ok:
                    pres.discard(a)
                    why[a] = r.why
                    nxt |= callers[a]
                    if a in self.taken and ind[0]:
                        ind[0] = False
                        nxt |= ind_callers
            work = nxt
        # what consumes it: from nothing, adding
        work = set(graphs)
        results = {}
        while work:
            nxt = set()
            for a in work:
                r = follow(graphs[a], n, callee, ENTRY, cont)
                results[a] = r
                if r.consumes and a not in cons:
                    cons.add(a)
                    nxt |= callers[a]
                # Through a pointer only a use of the register as it came counts (by code that never saved
                # it, or handed on to such): a function that saved it and then reads it before setting it
                # reads a variable it never set (there are a handful, in debug output), and what it gets
                # there is as arbitrary either way.
                if r.raw and a not in raw:
                    raw.add(a)
                    nxt |= callers[a]
                    if a in self.taken and a not in self.follows and not ind[1]:
                        ind[1] = True
                        nxt |= ind_callers
            work = nxt
        self.preserves[n] = pres
        self.consumes[n] = cons
        self.why[n] = why
        if ind[0] and not ind[1]:
            self.ind_safe.add(n)
        cons -= self.follows
        self.neutral[n] = {a: r.saves | r.restores for a, r in results.items()
                           if r.ok and r.saves and r.calls_ok and a in pres and a not in self.follows}
        self.saved[n] = {a for a, r in results.items() if r.saves}
        self.reads[n] = {a: r.reads for a, r in results.items() if r.reads is not None}
        self.raw[n] = raw
        self.ind[n] = tuple(ind)
