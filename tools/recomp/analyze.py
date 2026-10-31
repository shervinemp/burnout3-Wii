"""Function discovery and control-flow analysis for the PS2 executable.

The program is split into functions; each function is a set of reachable
instructions with labels for every branch target. Calls between functions
become C calls, jumps that leave a function become tail calls, and `jr` through
a recognised jump table becomes a C switch.

Any split is semantically safe: every function boundary synchronises the
register file through the context, so a wrong boundary costs speed, not
correctness.
"""
import bisect
import re

from r5900 import (COND_BRANCHES, HAS_DELAY_SLOT, LIKELY, decode,
                   is_unconditional_branch)

STORES = ("sw", "sb", "sh", "sd", "sq", "swc1", "sqc2", "swl", "swr", "sdl", "sdr")


class Config:
    """Per-title layout."""

    def __init__(self, text_ranges, data_ranges, extra_funcs=(), not_funcs=()):
        self.text_ranges = sorted(text_ranges)  # [(start, end)] of executable code
        self.data_ranges = data_ranges          # [(start, end)] scanned for code pointers
        self.extra_funcs = set(extra_funcs)
        self.not_funcs = set(not_funcs)


class Function:
    prefix = "f_"

    def __init__(self, start):
        self.start = start
        self.end = start                # one past the last instruction that belongs to it
        self.region_end = start
        self.insns = {}                 # pc -> Insn, reachable instructions only
        self.labels = set()             # pcs that are branch targets inside the function
        self.jump_tables = {}           # pc of jr -> list of target pcs
        self.calls = set()              # direct call targets
        self.tail_calls = set()         # jumps / fallthrough to other functions
        self.delay_slots = set()        # pcs that are delay slots
        self.new_starts = set()

    @property
    def name(self):
        return f"{Function.prefix}{self.start:08X}"


class Program:
    def __init__(self, img, cfg, log=print):
        self.img = img
        self.cfg = cfg
        self.log = log
        self._cache = {}
        self.starts = []
        self.funcs = {}
        self.jump_tables = {}
        self.jump_table_ranges = []
        self.jump_table_targets = set()
        self.overrides = {}             # address -> native C function name
        self.override_vu0 = set()       # overrides that may change the VU0 registers (title OVERRIDE_VU0)
        self.override_vu0_reload = {}   # address -> {call site: VU0 state read after it} (a report)
        self.prefix = "f_"              # C name prefix of recompiled functions
        self.table = "ps2_func_table"   # name of the address -> function table
        self.defines = []               # lines emitted before the includes of each file
        self.manual_jump_tables = {}    # jr pc -> (table address, entries), from config
        self.debug_wrap = set()         # functions whose calls are logged (debugging)
        self.wide64 = True              # 64-bit GPRs (EE); False for the 32-bit IOP
        self.io_funcs = set()           # functions that reach hardware through pointers
        self.entry_hooks = {}           # address -> native function called on entry (title fixes)
        self.vcall_sites = {}           # VCALLMS address -> native function called in place of ps2_vu0_callms
        self.vcall_inline = {}          # VCALLMS address -> (function on a Vu0I, registers read, registers written)
        self.inline_variants = set()    # functions also emitted with those sites inline, as n_XXXXXXXX
        self.hot_funcs = set()          # functions compiled for speed, not size (PS2_HOT)
        self.code_patches = {}          # pc -> patches.Site: instructions swapped at run time (g_code_patch)

    # -- instruction access ----------------------------------------------------

    def insn(self, pc):
        i = self._cache.get(pc)
        if i is None:
            i = decode(self.img.u32(pc), pc)
            self._cache[pc] = i
        return i

    def text_range(self, addr):
        for lo, hi in self.cfg.text_ranges:
            if lo <= addr < hi:
                return lo, hi
        return None

    def in_text(self, addr):
        return not addr & 3 and self.text_range(addr) is not None

    def text_pcs(self):
        for lo, hi in self.cfg.text_ranges:
            yield from range(lo, hi, 4)

    # -- jump tables -----------------------------------------------------------

    CALLEE_SAVED = frozenset(range(16, 24)) | {30}
    NO_GPR_WRITE = frozenset(STORES) | HAS_DELAY_SLOT | {"nop", "sync", "lwc1", "ldc1", "lqc2", "mtc1", "ctc1",
                                                         "qmtc2", "ctc2", "cache", "pref", "syscall", "break"}

    COP_GPR_WRITERS = frozenset({"mfc0", "mfc1", "cfc1", "qmfc2", "cfc2"})

    @classmethod
    def _cop_no_gpr(cls, i):
        """Coprocessor instruction that writes no GPR (its register fields name FPRs/VFs)."""
        return (i.word >> 26) in (0x10, 0x11, 0x12) and i.op not in cls.COP_GPR_WRITERS

    def _reg_const_before(self, reg, pc, lo_text, depth=0):
        """Constant held by `reg` just before pc, when the nearest earlier write
        to it (scanning straight back, within the function) is `lui` or `lui` +
        `addiu`/`ori`. Typical for table bases set up before a loop. Calls
        end the scan for caller-saved registers."""
        if reg == 0 or depth > 2:
            return None
        p = pc - 4
        for _ in range(512):
            if p < lo_text:
                return None
            i = self.insn(p)
            if i.op == "jr" and i.rs == 31:
                return None                     # previous function
            if i.op in self.NO_GPR_WRITE or self._cop_no_gpr(i):
                if i.op == "jalr" and i.rd == reg:
                    return None
                if i.op in ("jal", "jalr") and reg not in self.CALLEE_SAVED:
                    return None
                p -= 4
                continue
            if reg in (i.rt, i.rd):
                if i.op == "lui" and i.rt == reg:
                    return i.imm << 16
                if i.op in ("addiu", "ori") and i.rt == reg and i.rs != reg:
                    v = self._reg_const_before(i.rs, p, lo_text, depth + 1)
                    if v is None:
                        return None
                    return (v + i.simm) & 0xFFFFFFFF if i.op == "addiu" else v | i.imm
                if i.op in ("addiu", "ori") and i.rt == reg:
                    v = self._reg_const_before(reg, p, lo_text, depth + 1)
                    if v is None:
                        return None
                    return (v + i.simm) & 0xFFFFFFFF if i.op == "addiu" else v | i.imm
                return None
            p -= 4
        return None

    def _find_jump_table(self, jr_pc):
        """Recognise `lw rX, lo(base + idx*4)` feeding `jr rX`. Returns target list or None."""
        lo_text, hi_text = self.text_range(jr_pc)
        reg = self.insn(jr_pc).rs
        pc = jr_pc - 4
        load = None
        for _ in range(12):
            if pc < lo_text:
                break
            i = self.insn(pc)
            if i.op in HAS_DELAY_SLOT:
                break
            if i.op == "lw" and i.rt == reg:
                load = i
                break
            if reg in (i.rt, i.rd) and i.op not in STORES and i.op != "nop":
                break
            pc -= 4
        if load is None:
            return None
        chain = []
        p = load.pc - 4
        for _ in range(16):
            if p < lo_text:
                break
            i = self.insn(p)
            if i.op in HAS_DELAY_SLOT:
                break
            chain.append(i)
            p -= 4
        chain.reverse()
        sym = {}            # reg -> ("const", v) | ("sym", v)  (sym = const + index)
        chain_pc = chain[0].pc if chain else load.pc
        for r in {i.rs for i in chain if i.op in ("addiu", "ori", "addu", "daddu")} | \
                 {i.rt for i in chain if i.op in ("addu", "daddu")} | {load.rs}:
            v = self._reg_const_before(r, chain_pc, lo_text)
            if v is not None:
                sym[r] = ("const", v)
        for i in chain:
            if i.op == "lui":
                sym[i.rt] = ("const", i.imm << 16)
            elif i.op == "addiu" and i.rs in sym:
                kind, v = sym[i.rs]
                sym[i.rt] = (kind, (v + i.simm) & 0xFFFFFFFF)
            elif i.op == "ori" and i.rs in sym and sym[i.rs][0] == "const":
                sym[i.rt] = ("const", sym[i.rs][1] | i.imm)
            elif i.op in ("addu", "daddu"):
                a, b = sym.get(i.rs), sym.get(i.rt)
                if a and not b and i.rt != 0:
                    sym[i.rd] = ("sym", a[1])
                elif b and not a and i.rs != 0:
                    sym[i.rd] = ("sym", b[1])
                elif a and b:
                    sym[i.rd] = ("const", (a[1] + b[1]) & 0xFFFFFFFF)
                else:
                    sym.pop(i.rd, None)
            elif i.op in STORES or i.op in ("nop", "sync") or self._cop_no_gpr(i):
                pass
            elif (i.word >> 26) == 0 or (i.word >> 26) == 0x1C:
                sym.pop(i.rd, None)        # R-type writes rd
            else:
                sym.pop(i.rt, None)        # I-type writes rt
        b = sym.get(load.rs)
        if b is None or b[0] != "sym":
            return None
        table = (b[1] + load.simm) & 0xFFFFFFFF
        if not self.img.contains(table) or self.in_text(table):
            return None
        count = None
        p = load.pc
        for _ in range(24):
            p -= 4
            if p < lo_text:
                break
            i = self.insn(p)
            if i.op == "sltiu":
                count = i.imm
                break
        targets = []
        limit = count if count is not None else 1024
        while len(targets) < limit:
            addr = table + 4 * len(targets)
            if not self.img.contains(addr):
                break
            t = self.img.u32(addr)
            if t & 3 or not (lo_text <= t < hi_text) or abs(t - jr_pc) > 0x10000:
                if count is None:
                    break
                return None
            targets.append(t)
        if not targets:
            return None
        self.jump_table_ranges.append((table, table + 4 * len(targets)))
        return targets

    def find_jump_tables(self):
        for pc, (table, count) in self.manual_jump_tables.items():
            t = [self.img.u32(table + 4 * k) for k in range(count)]
            self.jump_tables[pc] = t
            self.jump_table_targets.update(t)
            self.jump_table_ranges.append((table, table + 4 * count))
        for pc in self.text_pcs():
            i = self.insn(pc)
            if i.op == "jr" and i.rs != 31 and pc not in self.manual_jump_tables:
                t = self._find_jump_table(pc)
                if t:
                    self.jump_tables[pc] = t
                    self.jump_table_targets.update(t)
        self.jump_table_ranges.sort()
        self.log(f"jump tables: {len(self.jump_tables)}")

    def _in_table(self, addr):
        k = bisect.bisect_right(self.jump_table_ranges, (addr, 0xFFFFFFFF)) - 1
        return k >= 0 and self.jump_table_ranges[k][0] <= addr < self.jump_table_ranges[k][1]

    # -- seeds -----------------------------------------------------------------

    def _epilogue_before(self, addr, lo):
        """True when the code just before addr ends a function: `jr ra` + delay
        slot, optionally followed by alignment padding."""
        p = addr - 4
        while p >= lo and self.insn(p).word == 0:
            p -= 4
        if p < lo:
            return True
        i = self.insn(p)
        if i.op == "eret":                      # no delay slot
            return True
        if i.op == "jr" and i.rs == 31:
            return p + 4 < addr                 # its delay slot (a nop) must come before addr
        i = self.insn(p - 4) if p - 4 >= lo else None
        return i is not None and ((i.op == "jr" and i.rs == 31) or i.op == "eret")

    def _looks_like_start(self, addr):
        """Strict test for code pointers found in data or built in code."""
        if not self.in_text(addr) or addr in self.jump_table_targets:
            return False
        lo, _ = self.text_range(addr)
        return addr == lo or self._epilogue_before(addr, lo)

    def collect_seeds(self, symbol_funcs=()):
        cfg = self.cfg
        self.find_jump_tables()
        seeds = {self.img.entry}
        seeds |= set(cfg.extra_funcs)
        seeds |= {a for a in symbol_funcs if self.in_text(a)}
        for pc in self.text_pcs():
            i = self.insn(pc)
            if i.op == "jal" and self.in_text(i.target):
                seeds.add(i.target)
        # code pointers built with lui/addiu or lui/ori inside the code
        self.code_ptr_refs = set()
        for pc in self.text_pcs():
            i = self.insn(pc)
            if i.op != "lui":
                continue
            hi = i.imm << 16
            _, range_end = self.text_range(pc)
            for k in range(1, 8):
                if pc + 4 * k >= range_end:
                    break
                j = self.insn(pc + 4 * k)
                if j.op in ("addiu", "ori") and j.rs == i.rt:
                    addr = (hi + j.simm) & 0xFFFFFFFF if j.op == "addiu" else hi | j.imm
                    if self.in_text(addr):
                        self.code_ptr_refs.add(addr)
                        if self._looks_like_start(addr):
                            seeds.add(addr)
                    break
                if i.rt in (j.rt, j.rd) and j.op not in STORES:
                    break
        # code pointers stored in data (vtables, callback tables, ctor lists)
        self.data_code_ptrs = set()
        for lo, hi in cfg.data_ranges:
            for addr in range(lo, hi, 4):
                if self._in_table(addr):
                    continue
                v = self.img.u32(addr)
                if self.in_text(v):
                    self.data_code_ptrs.add((addr, v))
                    if self._looks_like_start(v):
                        seeds.add(v)
        seeds -= cfg.not_funcs
        self.starts = sorted(seeds)
        self.log(f"seeds: {len(self.starts)}")

    # -- per-function traversal -----------------------------------------------

    def _region_end(self, start):
        k = bisect.bisect_right(self.starts, start)
        _, range_end = self.text_range(start)
        nxt = self.starts[k] if k < len(self.starts) else range_end
        return min(nxt, range_end)

    def _is_start(self, addr):
        k = bisect.bisect_left(self.starts, addr)
        return k < len(self.starts) and self.starts[k] == addr

    def trace(self, start):
        """Discover the reachable instructions of the function at start."""
        f = Function(start)
        region_end = self._region_end(start)
        f.region_end = region_end
        work = [start]
        seen = set()

        def local(t):
            return start <= t < region_end

        while work:
            pc = work.pop()
            while True:
                if pc in seen:
                    break
                if pc >= region_end:
                    f.tail_calls.add(pc)
                    f.new_starts.add(pc)
                    break
                seen.add(pc)
                i = self.insn(pc)
                f.insns[pc] = i
                op = i.op
                if op in HAS_DELAY_SLOT:
                    # the delay slot belongs to the branch even when another
                    # function happens to start there
                    ds = pc + 4
                    f.insns[ds] = self.insn(ds)
                    f.delay_slots.add(ds)
                    seen.add(ds)
                    if op in COND_BRANCHES:
                        t = i.branch_target
                        if local(t):
                            f.labels.add(t)
                            work.append(t)
                        else:
                            f.tail_calls.add(t)
                            f.new_starts.add(t)
                        if op in ("bltzal", "bgezal", "bltzall", "bgezall"):
                            f.calls.add(t)
                        if is_unconditional_branch(i):
                            break
                        pc += 8
                        f.labels.add(pc)
                        continue
                    if op == "j":
                        t = i.target
                        if local(t) and t != start:
                            f.labels.add(t)
                            work.append(t)
                        else:
                            f.tail_calls.add(t)
                            f.new_starts.add(t)
                        break
                    if op == "jal":
                        f.calls.add(i.target)
                        f.new_starts.add(i.target)
                        pc += 8
                        f.labels.add(pc)
                        continue
                    if op == "jalr":
                        pc += 8
                        f.labels.add(pc)
                        continue
                    if op == "jr":
                        tbl = self.jump_tables.get(pc)
                        if tbl and all(local(t) for t in tbl):
                            f.jump_tables[pc] = tbl
                            for t in tbl:
                                f.labels.add(t)
                                work.append(t)
                        break
                if op == "eret":
                    break
                pc += 4
        f.end = max(f.insns) + 4 if f.insns else start
        f.new_starts = {t for t in f.new_starts if self.in_text(t)}
        return f

    def analyze(self, max_rounds=200):
        funcs = {}
        dirty = set(self.starts)
        for rnd in range(max_rounds):
            for s in dirty:
                funcs[s] = self.trace(s)
            added = set()
            for s in dirty:
                f = funcs[s]
                for t in f.new_starts:
                    if not self._is_start(t) and t not in self.cfg.not_funcs:
                        added.add(t)
                # orphan code after the reachable end, inside the region
                p = f.end
                while p < f.region_end and self.insn(p).word == 0:
                    p += 4
                if p < f.region_end and p not in self.cfg.not_funcs:
                    added.add(p)
            if not added:
                self.funcs = funcs
                self.log(f"analysis converged after {rnd + 1} rounds: {len(funcs)} functions")
                return
            self.starts = sorted(set(self.starts) | added)
            # retrace the new functions and whichever function's region shrank
            dirty = set(added)
            for t in added:
                k = bisect.bisect_left(self.starts, t)
                if k > 0:
                    dirty.add(self.starts[k - 1])
            self.log(f"round {rnd + 1}: {len(added)} new function starts")
        raise RuntimeError("function discovery did not converge")


def read_symbol_funcs(path):
    """Function addresses from a splat symbol_addrs.txt."""
    out = {}
    pat = re.compile(r"^\s*([A-Za-z0-9_$.]+)\s*=\s*0x([0-9A-Fa-f]+);(.*)$")
    for line in open(path, encoding="utf-8"):
        m = pat.match(line)
        if m and "type:func" in m.group(3):
            out[int(m.group(2), 16)] = m.group(1)
    return out
