"""Test of the code patches (patches.py) without the game: a small program written here, recompiled with its
patches, compiled with the host's cc and run with the bit clear and set.

    python -I tools/recomp/test_patches.py

The function (at 0x100000) has the three kinds of site: an immediate changed (a "short" write over a lui), a
`j` into a cave in the data whose delay slot and return run the instruction after the site twice, and an
immediate changed in a branch's delay slot. The cave's own delay slot writes a register the original code never
writes, and the function stores it after: with the bit clear that register must keep the caller's value.
"""
import os
import struct
import subprocess
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import patches                                  # noqa: E402
from analyze import Config, Program             # noqa: E402
from emit import emit_program                   # noqa: E402

ZERO, V0, V1, A0, T0, T1, T2, S0, T9, SP, RA = 0, 2, 3, 4, 8, 9, 10, 16, 25, 29, 31


def itype(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def lui(rt, imm):
    return itype(0x0F, 0, rt, imm)


def addiu(rt, rs, imm):
    return itype(0x09, rs, rt, imm)


def ori(rt, rs, imm):
    return itype(0x0D, rs, rt, imm)


def sw(rt, off, base):
    return itype(0x2B, base, rt, off)


def lw(rt, off, base):
    return itype(0x23, base, rt, off)


def beq(rs, rt, frm, to):
    return itype(0x04, rs, rt, (to - frm - 4) >> 2)


def j(target):
    return (0x02 << 26) | ((target >> 2) & 0x03FFFFFF)


JR_RA = (RA << 21) | 0x08
NOP = 0

TEXT, DATA, CAVE = 0x100000, 0x200000, 0x200000

CODE = [
    addiu(SP, SP, -32),         # 100000
    lui(V0, 0x4420),            # 100004  site 1: lui v0, 0x4456
    sw(V0, 0, A0),              # 100008
    lui(T0, 0x3F80),            # 10000C  site 2: j cave
    sw(T0, 4, A0),              # 100010  the jump's delay slot, run again after the cave
    lw(V1, 8, A0),              # 100014
    addiu(V1, V1, 1),           # 100018
    sw(V1, 8, A0),              # 10001C
    beq(ZERO, ZERO, 0x100020, 0x100030),    # 100020
    ori(T1, ZERO, 5),           # 100024  site 3 (a delay slot): ori t1, zero, 7
    NOP,                        # 100028
    NOP,                        # 10002C
    sw(T1, 12, A0),             # 100030
    sw(T2, 20, A0),             # 100034  t2: the caller's, or the cave's
    JR_RA,                      # 100038
    addiu(SP, SP, 32),          # 10003C
    # a second function: a callee-saved register saved, set by a site, stored, restored
    addiu(SP, SP, -16),         # 100040
    sw(S0, 0, SP),              # 100044
    lui(S0, 0x1234),            # 100048  site 4: lui s0, 0x5678
    sw(S0, 24, A0),             # 10004C
    lw(S0, 0, SP),              # 100050
    JR_RA,                      # 100054
    addiu(SP, SP, 16),          # 100058
]
CAVE_CODE = [
    lui(T9, 0x4000),            # 200000
    sw(T9, 16, A0),             # 200004
    j(0x100010),                # 200008  back to the site's delay slot
    addiu(T2, ZERO, 3),         # 20000C  its delay slot
]
PNACH = f"""
patch=1,EE,20100004,short,4456 // lui v0, 0x4456
patch=1,EE,2010000C,extended,{j(CAVE):08X} // j cave
patch=1,EE,20200000,extended,{CAVE_CODE[0]:08X}
patch=1,EE,20200004,extended,{CAVE_CODE[1]:08X}
patch=1,EE,20200008,extended,{CAVE_CODE[2]:08X}
patch=1,EE,2020000C,extended,{CAVE_CODE[3]:08X}
patch=1,EE,00100024,short,0007 // ori t1, zero, 7
patch=1,EE,00100048,short,5678 // lui s0, 0x5678
"""


class Image:
    def __init__(self):
        self.base = TEXT
        size = DATA + 0x100 - TEXT
        buf = bytearray(size)
        for k, w in enumerate(CODE):
            struct.pack_into("<I", buf, k * 4, w)
        # the cave's place in the data holds something else until PCSX2 writes the patch
        for k in range(4):
            struct.pack_into("<I", buf, CAVE - TEXT + 4 * k, 0xDEAD0000 + k)
        self.data = bytes(buf)
        self.end = TEXT + size
        self.memsz = size
        self.entry = TEXT

    def contains(self, a):
        return self.base <= a < self.end

    def u32(self, a):
        return struct.unpack_from("<I", self.data, a - self.base)[0]


MAIN = r"""
#include <stdio.h>
#include <stdlib.h>
#include "funcs.h"
uint8_t *ps2_ram;
uint32_t g_code_patch;
void ps2_call(R5900 *c, uint32_t a) { (void)c; printf("call %08X\n", a); exit(2); }
void ps2_bad_insn(R5900 *c, uint32_t pc) { (void)c; printf("bad insn %08X\n", pc); exit(2); }
void ps2_bad_jump(R5900 *c, uint32_t pc, uint32_t t) { (void)c; printf("bad jump %08X %08X\n", pc, t); exit(2); }
static uint32_t W(uint32_t a) { return *(uint32_t *)(ps2_ram + a); }
int main(int argc, char **argv)
{
    static R5900 ctx;
    R5900 *c = &ctx;
    ps2_ram = calloc(1, 0x02000000);
    g_code_patch = (uint32_t)atoi(argv[1]);
    c->r[4][0] = 0x1000;            /* a0 */
    c->r[29][0] = 0x8000;           /* sp */
    c->r[31][0] = 0x12345678u;      /* ra */
    c->r[8][0] = 0x11111111u;       /* t0: the caller's */
    c->r[10][0] = 0x22222222u;      /* t2: the caller's */
    c->r[16][0] = 0x33333333u;      /* s0: the caller's */
    *(uint32_t *)(ps2_ram + 0x1008) = 41;
    f_00100000(c);
    f_00100040(c);
    printf("%08X %08X %u %u %08X %08X %08X sp %X s0 %08X\n", W(0x1000), W(0x1004), W(0x1008), W(0x100C), W(0x1010),
           W(0x1014), W(0x1018), c->r[29][0], c->r[16][0]);
    return 0;
}
"""

EXPECT = {
    # bit clear: the program as it was (t2 the caller's)
    0: "44200000 3F800000 42 5 00000000 22222222 12340000 sp 8000 s0 33333333",
    # bit set: v0 0x4456...; the site's lui t0 gone (the delay slot stores the caller's t0, twice); the cave stores
    # t9 and leaves t2 = 3; the delay slot's ori gives 7. (Temporaries are not written back to the context at a
    # return, patched or not: memory shows what they held.)
    1: "44560000 11111111 42 7 40000000 00000003 56780000 sp 8000 s0 33333333",
}


def main():
    img = Image()
    cfg = Config([(TEXT, TEXT + 4 * len(CODE))], [(DATA, DATA + 0x100)], extra_funcs=[0x100040])
    prog = Program(img, cfg, log=lambda *a: None)
    prog.code_patches = patches.build(img, cfg.text_ranges, {"test": (0, PNACH)})
    assert sorted(prog.code_patches) == [0x100004, 0x10000C, 0x100024, 0x100048], sorted(prog.code_patches)
    prog.collect_seeds()
    prog.analyze()
    title = types.SimpleNamespace(IND_SUMMARY=False, RET_LIVE=False)
    root = os.path.normpath(os.path.join(HERE, "..", ".."))
    with tempfile.TemporaryDirectory() as out:
        emit_program(prog, title, out, 400, log=lambda *a: None)
        with open(os.path.join(out, "main.c"), "w") as f:
            f.write(MAIN)
        src = [os.path.join(out, n) for n in sorted(os.listdir(out)) if n.startswith("recomp_")]
        src.append(os.path.join(out, "main.c"))
        exe = os.path.join(out, "t")
        subprocess.check_call(["cc", "-O1", "-w", "-I", out, "-I", os.path.join(root, "runtime"), "-o", exe] + src)
        ok = True
        for bit, want in EXPECT.items():
            got = subprocess.check_output([exe, str(bit)]).decode().strip()
            print(f"bit {bit}: {got}" + ("" if got == want else f"\n  expected {want}"))
            ok = ok and got == want
        if not ok or "-v" in sys.argv:
            with open(src[0]) as f:
                print(f.read())
        if not ok:
            sys.exit(1)
    print("code patches: ok")


if __name__ == "__main__":
    main()
