"""Burnout 3: Takedown (PS2, NTSC-U, SLUS-210.50) layout for the recompiler.

Section boundaries are from the PS2 decomp's map (siddharthakumar-98/Burnout3,
Burnout3_decomp/docs/layout.md); the ELF has no section headers.
"""

# SPDX-License-Identifier: GPL-3.0-or-later
#
# The widescreen patch data below is by Aero_, from PCSX2/pcsx2_patches
# (SLUS-21050_BEBF8793.pnach). The pcsx2_patches repository has no explicit
# licence; the writes are factual address/value data and are included with
# attribution. See THIRD_PARTY_NOTICES.md.

ELF_SHA1_ROM = "817cfb412af2d81d4b5fda7b57837b4e28442dfc"   # PT_LOAD contents

TEXT_RANGES = [
    (0x00100000, 0x00469E00),   # .text
    (0x004D3E00, 0x004DD820),   # .init: C++ static initializers, reached via .ctor
]

VU_CHAINS = (0x00469E00, 0x00483F00)  # VIF DMA chains carrying the VU microprograms
VU0_CHAINS = [0x004787D0, 0x00479610, 0x0047F6D0, 0x004839E0]  # of those, the VU0 microprograms
# What the game reads after a call of each VU0 routine (work/vu0live.py > config/burnout3_vu0live.txt): the
# routines are compiled a second time to store only that (tools/vu/vurecomp.py, runtime/vu.c rec_run).
VU0_LIVE = "burnout3_vu0live.txt"

DATA_RANGES = [
    (0x00483F00, 0x004B1500),   # .data
    (0x004B1500, 0x004D3E00),   # .rodata
    (0x004DD820, 0x004DDAA0),   # .ctor
    (0x004DDAA0, 0x004E0680),   # .vtables
    (0x004E1400, 0x004E2680),   # .sdata
]

GP = 0x004E8670
BSS = (0x004E2680, 0x01ECEA00)

# Function starts the analysis cannot find on its own (with the reason).
EXTRA_FUNCS = [
]

# Addresses that look like function starts but are not.
# Not called through pointers by the game (tools/recomp/abi.py): the crash handler's register dump and
# restore, its dispatcher, and data that decodes as code.
ABI_EXEMPT = [0x00100000, 0x00114B00, 0x00114C00, 0x00114E3C, 0x0041BCF0]   # (and the entry point)
# Functions that save and restore every callee-saved register they use, in a way the analysis does not
# read (looked at one by one): frames of more than 32 KB, saved through a copy of the old sp (0x1E8E78,
# 0x2073A0); frames with a frame pointer, sp restored from it (0x100EA0, 0x1003A0).
ABI_FOLLOWS = [0x001E8E78, 0x002073A0, 0x00100EA0, 0x001003A0,
               0x0010E87C]     # a lone "jr ra" whose delay slot is the next function's first instruction
# Never come back: _exit, and the C++ unwinder's throw and its jump into a handler (never run here).
NORETURN = [0x00100218, 0x001009F0, 0x00101FD0]
# Two further steps of tools/recomp/emit.py, written and off: return registers no caller reads not
# written back (return_values), and calls through pointers reading only what the functions whose
# address is taken read (compute_summaries). Both are swamped by registers that only look read (a
# register written on some paths counts as read at entry, so that there is something to store on
# the others): the generated C shrinks by under 1%. See docs/HANDOFF.md.
RET_LIVE = False
IND_SUMMARY = False
# Functions the runtime calls by name besides the overridden ones' originals (their return registers are
# all written back; tools/recomp/emit.py, return_values).
NATIVE_CALLS = [0x001F3080]
# Jump tables the analysis does not find by itself: jr pc -> (table address, entries).
JUMP_TABLES = {0x00353DB8: (0x004C71B0, 8)}

NOT_FUNCS = [
]

# VU microprogram subroutines done in C when the C function accepts (returns
# 1), else the microcode runs: chain -> {BAL target: function}.
VU_NATIVE_SUBS = {
    0x0046A590: {0x0888: "vu1_sky_strip"},      # sky: transform, clip and kick one strip
}

# Functions replaced by native code (runtime/hle.c): address -> C function.
OVERRIDES = {
    0x0010C3E8: "hle_tty_write",      # libkernl TTY write (printf to DECI2): goes to the log
    0x0026AF50: "hle_world_batch",    # world batch: native draw command (runtime/hle_render.c)
    0x002FBE20: "hle_audio_filter",   # audio DSP: filter, folder, drive (runtime/hle_audio.c)
    0x002FC820: "hle_audio_mix",      # audio DSP: stereo average times gains (runtime/hle_audio.c)
    0x002FC070: "hle_audio_channel",  # the engine sound, one channel: done where the chip is mixed (runtime/hle_audio.c)
    0x001CC900: "hle_car_forces",     # a stopped car gets no torque about its up axis (runtime/hle.c)
    0x00303A40: "hle_shadow_alpha",   # the car shadow keeps a floor of strength in shade (runtime/hle.c)
    0x00221E80: "hle_tex_send",       # textures the game places: a note, not an upload (runtime/hle.c)
    0x001D3EE0: "hle_frame_rate",     # frame rate mode (30/60 fps): can be forced (runtime/hle.c)
    0x0026A710: "hle_inst_view",      # reflection views drawn in turn (runtime/hle.c)
    0x002BC140: "hle_env_sky",
    0x002BC070: "hle_env_horizon",
    0x00125338: "hle_qsort",          # C library qsort: sorts by float key run natively (runtime/hle.c)
    0x00273070: "hle_coll_sphere",    # collision sphere tests skipped where they cannot hit (runtime/vu.c)
    0x0026B1C0: "hle_stage_copy",     # the walker's staged batches copied into the chain (runtime/hle_render.c)
    0x0026BAA0: "hle_view_packet",    # the walker's view packet queued without the recompiled code (runtime/hle_render.c)
    0x0019A950: "hle_race_load",      # a race's load step: taken again within the frame while it has time (runtime/hle.c)
    0x001322B0: "hle_fe_load",        # the front end's load step, the same
    0x00274C90: "hle_coll_c90",       # collision: a segment against a triangle, the 0x03D8 test done in the caller (runtime/vu.c)
    0x00273BF0: "hle_coll_bf0",       # collision: a segment against a strip of triangles, the nearest hit (runtime/vu.c)
    0x00273400: "hle_coll_400",       # collision: a segment against a strip of triangles, a callback for each hit (runtime/vu.c)
}

# Overrides that run VU0 work (a native of a VU0 routine's caller): they read no VU0 register and may leave any of
# them changed. The generator prints the VU0 state each call site reads afterwards, which the native has to leave
# right (tools/recomp/emit.py, OVERRIDE_VU0_SUMMARY).
OVERRIDE_VU0 = {0x00274C90, 0x00273BF0, 0x00273400}

# VCALLMS instructions run inline in a function's variant (the title's INLINE_VARIANTS: the function is also emitted as
# n_XXXXXXXX with these sites changed): site -> (the function on a Vu0I in runtime/vu0_inline.h that does what the
# entry does, the VU0 registers it reads, the VU0 registers it writes). The lists are what runtime/vu.c's natives for
# the entries read and leave (vu0_seg_*: SEG_DEAD and the rest are not written), which work/vu0live.py found to be all
# anything reads after them; a register the function holds in a local goes in and out of the Vu0I, no store to the
# context, no dispatch, no reload. vf24.xyz is read as well as written: the entries write it only when the segment
# passes the determinant test.
_SEG_R = "vf2.xyz vf4.xyz vf5.xyz vf6.xyz vf7.xyz vf23 vf24.xyz"
VCALLMS_INLINE = {
    0x00273638: ("vu0i_388", _SEG_R, "vf4.xyz vf6.xyz vf7.xyz vf19.x vf24.xyz vq vi1 vi8"),      # 0x273400: first test of each triangle
    0x00273E14: ("vu0i_388", _SEG_R, "vf4.xyz vf6.xyz vf7.xyz vf19.x vf24.xyz vq vi1 vi8"),      # 0x273BF0: the first triangle
    0x00273EEC: ("vu0i_550", "vf2.xyz vf4.xyz vf8.xyz vf9.xyz vf10.xyz vf23", "vf8.xyz vf9.xyz vf19.x vq vi2 vi8"),
    0x00274024: ("vu0i_390", _SEG_R, "vf6.xyz vf7.xyz vf19.x vf24.xyz vq vi1 vi8"),
}
INLINE_VARIANTS = {0x00273BF0, 0x00273400}
EXTRA_INCLUDES = ["vu0_inline.h"]

# Functions whose calls are logged with arguments and result (debugging only).
DEBUG_WRAP = [
]

# Functions that reach hardware registers through pointers (addresses computed
# at run time). The Wii build keeps the I/O check only in these; the PC debug
# build checks everywhere and logs "dynamic I/O ... in function X" for new ones.
IO_FUNCS = [
    0x00103BC8, 0x00103D48, 0x00104050, 0x001040B8, 0x001042F0,   # libkernel DMA/timer setup
    0x00107988, 0x00109BF8, 0x0010A930,                           # libipu / libmpeg
    0x0010C2B0, 0x0010E120,                                       # libkernel, libdma
    0x001F1778, 0x001F1CE8, 0x001F1F50, 0x001F2620, 0x001F3080,   # RenderWare DMA queue
    0x001F3A10,
]

# Hot functions (from the Wii profile, work/wprof.py) compiled for speed
# (PS2_HOT: -O2) where the rest of the recompiled code is optimized for size.
HOT_FUNCS = [
    0x002FBE20, 0x002FC820,                                       # audio DSP (fixed point, MMI)
    0x001F3080, 0x0021C780, 0x0021FBB0, 0x0026AF50, 0x0026B1C0,   # render list walkers, DMA packets
    0x0026BAA0, 0x00221E80, 0x00125338, 0x0021E6F0, 0x00221A70,
    0x0026B2F0, 0x0026A610, 0x0026B2B0, 0x00221650, 0x00273BF0,
]
# ... and every function with 0.02% or more of a race's samples (burnout3_hot.txt:
# python -I work/wprof.py work/prof.bin -n 2000, the f_ lines)
import os as _os
with open(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "burnout3_hot.txt")) as _f:
    HOT_FUNCS += [int(_l, 16) for _l in _f.read().split()]
# ... except CB3TrafficVehicle::Update (0x186850): the PS2 compiler inlined Remove into itself ~256 times (77 KB of MIPS, 1964
# bytes once folded). At -O2 that is 176 KB of PowerPC, at -Os 12 KB (GCC folds the copies); it is 0.1% of a race's samples.
HOT_FUNCS = [_a for _a in HOT_FUNCS if _a != 0x00186850]

# B3_WTIME=1 (a build of its own, runtime/wtime.h): the world walker and its callees through clock-read wrappers
if _os.environ.get("B3_WTIME"):
    for _a in (0x0021C780, 0x0021D080, 0x0026B8E0, 0x0026B7B0, 0x0026AE30, 0x0026B2B0, 0x0026B2F0,    # (0x26B1C0 and 0x26BAA0 time themselves)
               0x00221A70, 0x001F3080, 0x0026A7E0, 0x0026A610, 0x0026A1C0, 0x00332D00, 0x0021D770, 0x0021D550, 0x0026A240,
               0x001B8F00, 0x00273BF0, 0x00273400, 0x00274C90, 0x00274AF0, 0x003BE9B0, 0x003BF720, 0x003BEF60):
        OVERRIDES[_a] = "hle_wt_%08X" % _a

# VCALLMS instructions that call a native of their own instead of ps2_vu0_callms (same arguments): the call
# site, which a native cannot tell from the registers (the recompiled code keeps them in C locals).
VCALLMS_SITES = {
    0x002732D4: "hle_coll_sphere_call",   # 0x273070's sphere tests, a strip's first triangle (runtime/vu.c)
    0x00273344: "hle_coll_sphere_call",   # ... and each next one
}

# Instructions swapped at run time (tools/recomp/patches.py): a set's sites run the patched code while its bit of
# g_code_patch is set (runtime/patches.c). name -> (bit, [(address, size, value), ...] or pnach text).
# Widescreen 16:9: Aero_'s PCSX2 patch for this disc (pcsx2_patches, SLUS-21050_BEBF8793.pnach), re-expressed as
# address/size/value writes rather than the .pnach text. The recompiler takes its code half from these writes: the
# immediates changed in place and the jumps into caves in .data (0x4858C0..0x4859E8), run where they are, delay
# slots and all. The data half (FOV, HUD, borders) is written into RAM by runtime/patches.c; writes here that do
# not land in code are ignored.
# Bit 0 is set with args.txt "--widescreen" / autopilot "widescreen".
WIDESCREEN_PATCH = [
    (0x00665ECD, 1, 0x01),
    (0x004E0A38, 4, 0x3FAAAAAB),
    (0x004E0C70, 4, 0x3FE38E39),
    (0x004E0C7C, 4, 0x3FE38E39),
    (0x004E0C80, 4, 0x40638E39),
    (0x004E105C, 4, 0x3A99999A),
    (0x006682B0, 4, 0x42A00000),
    (0x00669B30, 4, 0x42A00000),
    (0x004B7688, 4, 0x3F9947AE),
    (0x004B7678, 4, 0x3F9947AE),
    (0x004B7658, 4, 0xBE4A3D71),
    (0x004B7668, 4, 0xBE4A3D71),
    (0x004CA660, 4, 0x44555555),
    (0x004CA640, 4, 0x44555555),
    (0x004CA650, 4, 0x44555555),
    (0x004CA638, 4, 0xC2D70000),
    (0x004CA658, 4, 0xC2D70000),
    (0x003D7238, 4, 0x3C024456),
    (0x003D70BC, 4, 0x08121630),
    (0x004858C0, 4, 0x3C08C2D7),
    (0x004858C4, 4, 0x44880800),
    (0x004858C8, 4, 0x080F5C30),
    (0x003D72F4, 4, 0x08121633),
    (0x004858CC, 4, 0x3C19C2D7),
    (0x004858D0, 4, 0x44991000),
    (0x004858D4, 4, 0x080F5CBE),
    (0x001A176C, 2, 0x4456),
    (0x001A17D0, 2, 0x4456),
    (0x001A1774, 4, 0x08121672),
    (0x004859C8, 4, 0xAFA200A0),
    (0x004859CC, 4, 0x3C02C2D7),
    (0x004859D0, 4, 0xAFA200A8),
    (0x004859D4, 4, 0x080685DE),
    (0x001A17D8, 4, 0x08121676),
    (0x004859D8, 4, 0xAFA20090),
    (0x004859DC, 4, 0x3C02C2D7),
    (0x004859E0, 4, 0xAFA20098),
    (0x004859E4, 4, 0x080685F7),
    (0x00134F2C, 2, 0x4456),
    (0x00134F74, 4, 0x0812166E),
    (0x004859B8, 4, 0xAFA20174),
    (0x004859BC, 4, 0x3C023EC0),
    (0x004859C0, 4, 0xAFA20168),
    (0x004859C4, 4, 0x0804D3DE),
    (0x0030D7E4, 2, 0x43F0),
    (0x0030D834, 4, 0x08121636),
    (0x004858D8, 4, 0xE4830008),
    (0x004858DC, 4, 0x3C1942A0),
    (0x004858E0, 4, 0x44991800),
    (0x004858E4, 4, 0xE4830000),
    (0x004858E8, 4, 0x080C360E),
    (0x004D1570, 4, 0x44555555),
    (0x004D1568, 4, 0xC2D70000),
    (0x0031B180, 2, 0x4456),
    (0x0031B1F4, 4, 0x0812163B),
    (0x004858EC, 4, 0x3C190000),
    (0x004858F0, 4, 0x44993800),
    (0x004858F4, 4, 0xE4870000),
    (0x004858F8, 4, 0xC4850004),
    (0x004858FC, 4, 0x080C6C7E),
    (0x0038AE00, 2, 0x4456),
    (0x0038AE38, 2, 0x4456),
    (0x0038AE4C, 4, 0x08121640),
    (0x00485900, 4, 0x3C19C2D7),
    (0x00485904, 4, 0xAFB90068),
    (0x00485908, 4, 0x080E2B94),
    (0x0038AE04, 4, 0x08121643),
    (0x0048590C, 4, 0x3C193E00),
    (0x00485910, 4, 0xAFB90078),
    (0x00485914, 4, 0x080E2B82),
    (0x0031D6E4, 2, 0x4456),
    (0x0031D73C, 2, 0x4456),
    (0x0031D7E8, 2, 0x4456),
    (0x0031D790, 2, 0x4456),
    (0x0031D840, 2, 0x4456),
    (0x0031D6F8, 4, 0x08121646),
    (0x00485918, 4, 0x3C193E00),
    (0x0048591C, 4, 0xAFB90110),
    (0x00485920, 4, 0x080C75BF),
    (0x0031D750, 4, 0x08121649),
    (0x00485924, 4, 0x3C193E00),
    (0x00485928, 4, 0xAFB900F8),
    (0x0048592C, 4, 0x080C75D5),
    (0x0031D7FC, 4, 0x0812164C),
    (0x00485930, 4, 0x3C193E00),
    (0x00485934, 4, 0xAFB900C8),
    (0x00485938, 4, 0x080C7600),
    (0x0031D7A4, 4, 0x0812164F),
    (0x0048593C, 4, 0x3C193E00),
    (0x00485940, 4, 0xAFB900E0),
    (0x00485944, 4, 0x080C75EA),
    (0x0031D854, 4, 0x08121652),
    (0x00485948, 4, 0x3C193E00),
    (0x0048594C, 4, 0xAFB900B0),
    (0x00485950, 4, 0x080C7616),
    (0x0031DA20, 2, 0x4456),
    (0x0031DA40, 4, 0x08121655),
    (0x00485954, 4, 0x3C19C2D7),
    (0x00485958, 4, 0xAFB901D8),
    (0x0048595C, 4, 0x080C7691),
    (0x003A6984, 2, 0x4456),
    (0x003A6998, 4, 0x08121668),
    (0x004859A0, 4, 0x3C19C2D7),
    (0x004859A4, 4, 0xAFB90078),
    (0x004859A8, 4, 0x080E9A67),
    (0x003A69C8, 2, 0x4456),
    (0x003A69DC, 4, 0x0812166B),
    (0x004859AC, 4, 0x3C19C2D7),
    (0x004859B0, 4, 0xAFB90060),
    (0x004859B4, 4, 0x080E9A78),
    (0x004E0C3C, 4, 0x3FE38E39),
    (0x004E0A94, 4, 0x3FB8E38F),
]
CODE_PATCHES = {"widescreen": (0, WIDESCREEN_PATCH)}

# Native functions run on entry to a guest function, before its body (runtime/hle.c).
ENTRY_HOOKS = {
    # libmc2 write worker: on the first write to an empty file it reads its file
    # position (sp+680 of its 768-byte frame) before setting it, and makes that
    # the file length. On the PS2 the slot holds something harmless; here it
    # held libmc2's work-area pointer, giving icon.sys a 30 MB length and the
    # save a "corrupt data" error on the next boot.
    0x00246788: "hle_mc2_write_entry",
    # callback of the frame kick's wait for the last flip: the wait sleeps (runtime/hle.c)
    0x00228040: "hle_flip_wait_entry",
    # clear of the reflection views (runtime/hle.c)
    0x00222F70: "hle_env_clear_entry",
}
