"""R5900 (PS2 Emotion Engine) instruction decoder.

decode(word, pc) returns an Insn with the mnemonic in lower case and the raw
fields. Unknown encodings decode to mnemonic "unknown" so analysis can report
them instead of crashing.
"""

GPR_NAMES = [
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
]


class Insn:
    __slots__ = ("pc", "word", "op", "rs", "rt", "rd", "sa", "funct", "imm",
                 "simm", "target", "fmt", "ft", "fs", "fd", "dest", "bc",
                 "fsf", "ftf")

    def __init__(self, pc, word, op):
        self.pc = pc
        self.word = word
        self.op = op
        self.rs = (word >> 21) & 31
        self.rt = (word >> 16) & 31
        self.rd = (word >> 11) & 31
        self.sa = (word >> 6) & 31
        self.funct = word & 63
        self.imm = word & 0xFFFF
        self.simm = self.imm - 0x10000 if self.imm & 0x8000 else self.imm
        self.target = ((pc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
        # coprocessor views
        self.fmt = self.rs
        self.ft = self.rt
        self.fs = self.rd
        self.fd = self.sa
        self.dest = (word >> 21) & 15          # VU xyzw mask: x=8 y=4 z=2 w=1
        self.bc = word & 3                      # VU broadcast field
        self.fsf = (word >> 21) & 3
        self.ftf = (word >> 23) & 3

    @property
    def branch_target(self):
        return (self.pc + 4 + (self.simm << 2)) & 0xFFFFFFFF

    def __repr__(self):
        return f"<{self.pc:08X} {self.word:08X} {self.op}>"


PRIMARY = {
    0x02: "j", 0x03: "jal", 0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz",
    0x08: "addi", 0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi",
    0x0D: "ori", 0x0E: "xori", 0x0F: "lui",
    0x14: "beql", 0x15: "bnel", 0x16: "blezl", 0x17: "bgtzl",
    0x18: "daddi", 0x19: "daddiu", 0x1A: "ldl", 0x1B: "ldr",
    0x1E: "lq", 0x1F: "sq",
    0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu",
    0x26: "lwr", 0x27: "lwu", 0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw",
    0x2C: "sdl", 0x2D: "sdr", 0x2E: "swr", 0x2F: "cache",
    0x31: "lwc1", 0x33: "pref", 0x36: "lqc2", 0x37: "ld",
    0x39: "swc1", 0x3E: "sqc2", 0x3F: "sd",
}

SPECIAL = {
    0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv", 0x07: "srav",
    0x08: "jr", 0x09: "jalr", 0x0A: "movz", 0x0B: "movn", 0x0C: "syscall",
    0x0D: "break", 0x0F: "sync",
    0x10: "mfhi", 0x11: "mthi", 0x12: "mflo", 0x13: "mtlo",
    0x14: "dsllv", 0x16: "dsrlv", 0x17: "dsrav",
    0x18: "mult", 0x19: "multu", 0x1A: "div", 0x1B: "divu",
    0x20: "add", 0x21: "addu", 0x22: "sub", 0x23: "subu",
    0x24: "and", 0x25: "or", 0x26: "xor", 0x27: "nor",
    0x28: "mfsa", 0x29: "mtsa", 0x2A: "slt", 0x2B: "sltu",
    0x2C: "dadd", 0x2D: "daddu", 0x2E: "dsub", 0x2F: "dsubu",
    0x30: "tge", 0x31: "tgeu", 0x32: "tlt", 0x33: "tltu", 0x34: "teq", 0x36: "tne",
    0x38: "dsll", 0x3A: "dsrl", 0x3B: "dsra", 0x3C: "dsll32", 0x3E: "dsrl32", 0x3F: "dsra32",
}

REGIMM = {
    0x00: "bltz", 0x01: "bgez", 0x02: "bltzl", 0x03: "bgezl",
    0x08: "tgei", 0x09: "tgeiu", 0x0A: "tlti", 0x0B: "tltiu", 0x0C: "teqi", 0x0E: "tnei",
    0x10: "bltzal", 0x11: "bgezal", 0x12: "bltzall", 0x13: "bgezall",
    0x18: "mtsab", 0x19: "mtsah",
}

MMI = {
    0x00: "madd", 0x01: "maddu", 0x04: "plzcw",
    0x10: "mfhi1", 0x11: "mthi1", 0x12: "mflo1", 0x13: "mtlo1",
    0x18: "mult1", 0x19: "multu1", 0x1A: "div1", 0x1B: "divu1",
    0x20: "madd1", 0x21: "maddu1",
    0x30: "pmfhl", 0x31: "pmthl",
    0x34: "psllh", 0x36: "psrlh", 0x37: "psrah",
    0x3C: "psllw", 0x3E: "psrlw", 0x3F: "psraw",
}

MMI0 = {
    0x00: "paddw", 0x01: "psubw", 0x02: "pcgtw", 0x03: "pmaxw",
    0x04: "paddh", 0x05: "psubh", 0x06: "pcgth", 0x07: "pmaxh",
    0x08: "paddb", 0x09: "psubb", 0x0A: "pcgtb",
    0x10: "paddsw", 0x11: "psubsw", 0x12: "pextlw", 0x13: "ppacw",
    0x14: "paddsh", 0x15: "psubsh", 0x16: "pextlh", 0x17: "ppach",
    0x18: "paddsb", 0x19: "psubsb", 0x1A: "pextlb", 0x1B: "ppacb",
    0x1E: "pext5", 0x1F: "ppac5",
}

MMI1 = {
    0x01: "pabsw", 0x02: "pceqw", 0x03: "pminw", 0x04: "padsbh",
    0x05: "pabsh", 0x06: "pceqh", 0x07: "pminh", 0x0A: "pceqb",
    0x10: "padduw", 0x11: "psubuw", 0x12: "pextuw",
    0x14: "padduh", 0x15: "psubuh", 0x16: "pextuh",
    0x18: "paddub", 0x19: "psubub", 0x1A: "pextub", 0x1B: "qfsrv",
}

MMI2 = {
    0x00: "pmaddw", 0x02: "psllvw", 0x03: "psrlvw", 0x04: "pmsubw",
    0x08: "pmfhi", 0x09: "pmflo", 0x0A: "pinth", 0x0C: "pmultw", 0x0D: "pdivw",
    0x0E: "pcpyld", 0x10: "pmaddh", 0x11: "phmadh", 0x12: "pand", 0x13: "pxor",
    0x14: "pmsubh", 0x15: "phmsbh", 0x1A: "pexeh", 0x1B: "prevh",
    0x1C: "pmulth", 0x1D: "pdivbw", 0x1E: "pexew", 0x1F: "prot3w",
}

MMI3 = {
    0x00: "pmadduw", 0x03: "psravw", 0x08: "pmthi", 0x09: "pmtlo", 0x0A: "pinteh",
    0x0C: "pmultuw", 0x0D: "pdivuw", 0x0E: "pcpyud", 0x12: "por", 0x13: "pnor",
    0x1A: "pexch", 0x1B: "pcpyh", 0x1E: "pexcw",
}

COP1_S = {
    0x00: "add.s", 0x01: "sub.s", 0x02: "mul.s", 0x03: "div.s", 0x04: "sqrt.s",
    0x05: "abs.s", 0x06: "mov.s", 0x07: "neg.s", 0x16: "rsqrt.s",
    0x18: "adda.s", 0x19: "suba.s", 0x1A: "mula.s", 0x1C: "madd.s",
    0x1D: "msub.s", 0x1E: "madda.s", 0x1F: "msuba.s", 0x24: "cvt.w.s",
    0x28: "max.s", 0x29: "min.s", 0x30: "c.f.s", 0x32: "c.eq.s",
    0x34: "c.lt.s", 0x36: "c.le.s",
}

_BC = "xyzw"
VU_S1 = {}
for _i, _n in enumerate(("vadd", "vsub", "vmadd", "vmsub", "vmax", "vmini", "vmul")):
    for _b in range(4):
        VU_S1[_i * 4 + _b] = _n + _BC[_b]
VU_S1.update({
    0x1C: "vmulq", 0x1D: "vmaxi", 0x1E: "vmuli", 0x1F: "vminii",
    0x20: "vaddq", 0x21: "vmaddq", 0x22: "vaddi", 0x23: "vmaddi",
    0x24: "vsubq", 0x25: "vmsubq", 0x26: "vsubi", 0x27: "vmsubi",
    0x28: "vadd", 0x29: "vmadd", 0x2A: "vmul", 0x2B: "vmax", 0x2C: "vsub",
    0x2D: "vmsub", 0x2E: "vopmsub", 0x2F: "vmini",
    0x30: "viadd", 0x31: "visub", 0x32: "viaddi", 0x34: "viand", 0x35: "vior",
    0x38: "vcallms", 0x39: "vcallmsr",
})

VU_S2 = {}
for _i, _n in enumerate(("vadda", "vsuba", "vmadda", "vmsuba")):
    for _b in range(4):
        VU_S2[_i * 4 + _b] = _n + _BC[_b]
for _b in range(4):
    VU_S2[0x18 + _b] = "vmula" + _BC[_b]
VU_S2.update({
    0x10: "vitof0", 0x11: "vitof4", 0x12: "vitof12", 0x13: "vitof15",
    0x14: "vftoi0", 0x15: "vftoi4", 0x16: "vftoi12", 0x17: "vftoi15",
    0x1C: "vmulaq", 0x1D: "vabs", 0x1E: "vmulai", 0x1F: "vclipw",
    0x20: "vaddaq", 0x21: "vmaddaq", 0x22: "vaddai", 0x23: "vmaddai",
    0x24: "vsubaq", 0x25: "vmsubaq", 0x26: "vsubai", 0x27: "vmsubai",
    0x28: "vadda", 0x29: "vmadda", 0x2A: "vmula", 0x2C: "vsuba", 0x2D: "vmsuba",
    0x2E: "vopmula", 0x2F: "vnop", 0x30: "vmove", 0x31: "vmr32",
    0x34: "vlqi", 0x35: "vsqi", 0x36: "vlqd", 0x37: "vsqd",
    0x38: "vdiv", 0x39: "vsqrt", 0x3A: "vrsqrt", 0x3B: "vwaitq",
    0x3C: "vmtir", 0x3D: "vmfir", 0x3E: "vilwr", 0x3F: "viswr",
    0x40: "vrnext", 0x41: "vrget", 0x42: "vrinit", 0x43: "vrxor",
})


def decode(word, pc):
    opc = word >> 26
    rs = (word >> 21) & 31
    rt = (word >> 16) & 31
    funct = word & 63
    name = None
    if opc == 0x00:
        name = SPECIAL.get(funct)
        if name == "sll" and word == 0:
            name = "nop"
    elif opc == 0x01:
        name = REGIMM.get(rt)
    elif opc == 0x1C:
        sub = (word >> 6) & 31
        if funct == 0x08:
            name = MMI0.get(sub)
        elif funct == 0x28:
            name = MMI1.get(sub)
        elif funct == 0x09:
            name = MMI2.get(sub)
        elif funct == 0x29:
            name = MMI3.get(sub)
        else:
            name = MMI.get(funct)
    elif opc == 0x10:
        if rs == 0x00:
            name = "mfc0"
        elif rs == 0x04:
            name = "mtc0"
        elif rs == 0x08:
            name = ("bc0f", "bc0t", "bc0fl", "bc0tl")[rt] if rt < 4 else None
        elif rs == 0x10:
            name = {0x01: "tlbr", 0x02: "tlbwi", 0x06: "tlbwr", 0x08: "tlbp",
                    0x18: "eret", 0x38: "ei", 0x39: "di"}.get(funct)
    elif opc == 0x11:
        if rs == 0x00:
            name = "mfc1"
        elif rs == 0x02:
            name = "cfc1"
        elif rs == 0x04:
            name = "mtc1"
        elif rs == 0x06:
            name = "ctc1"
        elif rs == 0x08:
            name = ("bc1f", "bc1t", "bc1fl", "bc1tl")[rt] if rt < 4 else None
        elif rs == 0x10:
            name = COP1_S.get(funct)
        elif rs == 0x14 and funct == 0x20:
            name = "cvt.s.w"
    elif opc == 0x12:
        if word & (1 << 25):
            if funct >= 0x3C:
                name = VU_S2.get((((word >> 6) & 31) << 2) | (funct & 3))
            else:
                name = VU_S1.get(funct)
        elif rs == 0x01:
            name = "qmfc2"
        elif rs == 0x02:
            name = "cfc2"
        elif rs == 0x05:
            name = "qmtc2"
        elif rs == 0x06:
            name = "ctc2"
        elif rs == 0x08:
            name = ("bc2f", "bc2t", "bc2fl", "bc2tl")[rt] if rt < 4 else None
    else:
        name = PRIMARY.get(opc)
    return Insn(pc, word, name or "unknown")


# --- instruction classes used by analysis -------------------------------------

COND_BRANCHES = {
    "beq", "bne", "blez", "bgtz", "beql", "bnel", "blezl", "bgtzl",
    "bltz", "bgez", "bltzl", "bgezl", "bltzal", "bgezal", "bltzall", "bgezall",
    "bc0f", "bc0t", "bc0fl", "bc0tl", "bc1f", "bc1t", "bc1fl", "bc1tl",
    "bc2f", "bc2t", "bc2fl", "bc2tl",
}
LIKELY = {
    "beql", "bnel", "blezl", "bgtzl", "bltzl", "bgezl", "bltzall", "bgezall",
    "bc0fl", "bc0tl", "bc1fl", "bc1tl", "bc2fl", "bc2tl",
}
LINKING_BRANCHES = {"bltzal", "bgezal", "bltzall", "bgezall"}
HAS_DELAY_SLOT = COND_BRANCHES | {"j", "jal", "jr", "jalr"}


def is_unconditional_branch(insn):
    """beq $zero,$zero / bgez $zero: always taken, compilers emit them as 'b'."""
    if insn.op in ("beq", "beql") and insn.rs == 0 and insn.rt == 0:
        return True
    if insn.op in ("bgez", "bgezl") and insn.rs == 0:
        return True
    return False


def format_insn(i):
    """Short human-readable rendering for comments and reports."""
    g = GPR_NAMES
    op = i.op
    if op in ("j", "jal"):
        return f"{op} 0x{i.target:08X}"
    if op in COND_BRANCHES:
        return f"{op} {g[i.rs]},{g[i.rt]},0x{i.branch_target:08X}"
    if op in ("lui",):
        return f"lui {g[i.rt]},0x{i.imm:04X}"
    if op in PRIMARY.values() and i.word >> 26 >= 0x20:
        return f"{op} {g[i.rt]},{i.simm}({g[i.rs]})"
    if op in ("addiu", "addi", "slti", "sltiu", "daddiu", "daddi"):
        return f"{op} {g[i.rt]},{g[i.rs]},{i.simm}"
    if op in ("andi", "ori", "xori"):
        return f"{op} {g[i.rt]},{g[i.rs]},0x{i.imm:X}"
    if op in ("jr",):
        return f"jr {g[i.rs]}"
    if op in ("jalr",):
        return f"jalr {g[i.rd]},{g[i.rs]}"
    if op in ("sll", "srl", "sra", "dsll", "dsrl", "dsra", "dsll32", "dsrl32", "dsra32"):
        return "nop" if i.word == 0 else f"{op} {g[i.rd]},{g[i.rt]},{i.sa}"
    if op in ("sllv", "srlv", "srav", "dsllv", "dsrlv", "dsrav"):
        return f"{op} {g[i.rd]},{g[i.rt]},{g[i.rs]}"
    if op in ("mult", "multu", "div", "divu", "mult1", "multu1", "div1", "divu1") and i.rd == 0:
        return f"{op} {g[i.rs]},{g[i.rt]}"
    if op in ("mfhi", "mflo", "mfhi1", "mflo1"):
        return f"{op} {g[i.rd]}"
    if op in ("mthi", "mtlo", "mthi1", "mtlo1"):
        return f"{op} {g[i.rs]}"
    if op in ("lq", "sq", "ld", "sd", "lwc1", "swc1", "lqc2", "sqc2"):
        reg = f"$f{i.rt}" if op in ("lwc1", "swc1") else (f"vf{i.rt}" if op in ("lqc2", "sqc2") else g[i.rt])
        return f"{op} {reg},{i.simm}({g[i.rs]})"
    if op in SPECIAL.values() or op in MMI.values() or op in MMI0.values() \
            or op in MMI1.values() or op in MMI2.values() or op in MMI3.values():
        return f"{op} {g[i.rd]},{g[i.rs]},{g[i.rt]} sa={i.sa}"
    return f"{op} rs={i.rs} rt={i.rt} rd={i.rd} sa={i.sa}"
