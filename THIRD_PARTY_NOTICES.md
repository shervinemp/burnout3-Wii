# Third-party notices

This project is licensed under **GPL-3.0-or-later** (see `LICENSE`). The
components below keep their own copyright notices and licences. A Wii binary
built from this source is a combined work under GPL-3.0-or-later because it
links GPL-covered code. The original source in this repository is the
corresponding source for the project code; a binary also contains the
generated `gen/` and `gen_iop/` trees, which are derived from the game and are
not distributed here (see `NOTICE`).

No game assets, game executables, or generated recompiled game code are
included in this repository. Human-readable credits are in `CREDITS.md`.

## libntfs / NTFS-3G

- **Where:** `external/libntfs/`
- **Used by:** the Wii build (`Makefile.wii`), linked into `burnout3.dol`
- **Source:** <https://github.com/extremscorner/libntfs>, commit
  `f73a4d1ff9e2d6bd8fdc7c8caa3a74df74803103` (NTFS-3G 2026.9.28). This is
  **extrems**' Wii port of **NTFS-3G**
  (<https://github.com/tuxera/ntfs-3g>).
- **Licence:** GPL-2.0-or-later. The full GPL v2 text is in
  `external/libntfs/LICENSE` and `LICENSES/GPL-2.0-or-later.txt`.
- **Changes:** documented in `external/libntfs/PATCHES.md` (libogc 3.1
  compatibility and GPT support). No NTFS logic was changed otherwise.
- **Why it matters:** GPL-2.0-or-later is compatible with GPL-3.0-or-later.
  Distributing a binary that links libntfs requires the corresponding source
  and the GPL notices. This repository provides both.

## PCSX2 GS swizzle tables

- **Where:** `runtime/gs.c`, `runtime/gs.h`
- **Used by:** both the Wii and PC builds
- **Source:** <https://github.com/PCSX2/pcsx2>, `pcsx2/GS/GSTables.cpp`
- **Copyright:** 2002-2026 PCSX2 Dev Team
- **Licence:** GPL-3.0-or-later (the file carries `SPDX-License-Identifier:
  GPL-3.0+`)
- **What was used:** the block/column swizzle lookup tables for PS2 GS texture
  addressing. The rest of the GS emulation in this project is original code.
- **Why it matters:** GPL-3.0-or-later is compatible with this project's
  licence. The tables are marked in the source files.

## PCSX2 behavioural references

Several runtime files cite PCSX2 for hardware behaviour while implementing it
independently for this project: `runtime/vif.c` (VIF UNPACK), `runtime/vu.c`
and `runtime/vu_rec.h` (VU pipeline stalls and EFU forms), `runtime/iop_sio2.c`
(SIO2 / DualShock 2 / memory-card byte formats), and `runtime/iop_rev.h`
(SPU2 reverb). PCSX2 is GPL-3.0-or-later. These files are distributed under
this project's GPL-3.0-or-later licence; if any line is later found to be a
close adaptation rather than an independent implementation, it is already
covered by that licence and the attribution above.

## PS2SDK / ps2dev (reference only)

- **Where:** the IOP runtime and tools cite PS2SDK for API/protocol details:
  `runtime/iop_libs.c`, `runtime/iop_kernel.c`, `runtime/iop_sd.c`,
  `runtime/iop_rev.h`, `runtime/sif.c`, `tools/iop/irxdis.py`,
  `tools/iop/irxload.py`.
- **Source:** <https://github.com/ps2dev/ps2sdk>
- **Licence:** Academic Free License 2.0 (AFL-2.0) upstream; see the PS2SDK
  `LICENSE`.
- **What was used:** libsd/libkernel/libmc/libpad/SIFCMD/SIFRPC API and
  protocol references, and the SPU2 reverb presets/coefficients.
- **What was not used:** no PS2SDK source is vendored in this repository. The
  runtime is written for this project from the API/protocol/preset references.
  This entry is an attribution for the reference material, not a claim that
  PS2SDK code is included. If a file is later found to be a close adaptation,
  its licence will be handled separately.
- **Licence note:** AFL-2.0 is not compatible with the GPL. Do not copy PS2SDK
  source into this GPL-3.0-or-later work without checking the terms. The
  reverb preset/coefficient values are treated as functional SPU2 register
  data; if you want zero ambiguity, re-derive them or drop the presets.

## PCSX2 widescreen patch (Aero_)

- **Where:** `config/burnout3.py` (`WIDESCREEN_PATCH`, re-expressed as
  address/size/value tuples), and the same values re-expressed as C tables in
  `runtime/patches.c`
- **Source:** <https://github.com/PCSX2/pcsx2_patches>,
  `patches/SLUS-21050_BEBF8793.pnach`, "Widescreen 16:9" by **Aero_**
- **Licence:** the `pcsx2_patches` repository does not declare a licence. The
  original `.pnach` text is not included; only the factual address/size/value
  writes are, with attribution to the author. If the author or PCSX2 asks, the
  writes will be removed or relicensed.
- **What was used:** the widescreen data writes, and the code-cave addresses
  needed to compile the patched instructions into the recompiled code. The
  data half is written by `runtime/patches.c`; the code half is selected by
  `g_code_patch` bit 0.

## libchdr / MAME CHD (optional, not vendored)

- **Where:** `tools/chd2iso/` builds against a caller-provided libchdr checkout
- **Source:** <https://github.com/rtissera/libchdr>; the CHD format and the
  original code come from **MAME** (<https://www.mamedev.org>)
- **Licence:** BSD-3-Clause; see `LICENSES/BSD-3-Clause.txt`
- **Why it matters:** `chd2iso` is an optional helper for converting CHD images.
  libchdr is not included in this repository or in the Wii binary.

## Burnout 3 decompilation layout (reference only)

- **Source:** <https://github.com/siddharthakumar-98/Burnout3>
- **Licence:** GPL-3.0 (the repository's `LICENSE` is GPL v3)
- **What was used:** section boundaries and a few layout facts referenced in
  `config/burnout3.py`. No source code from that project is included.

## devkitPro / libogc, AESND and Wii libraries (build dependencies)

- **Where:** not vendored; installed with devkitPro
- **Source:** <https://devkitpro.org>, <https://github.com/devkitPro/libogc>
- **Licence:** libogc uses its own permissive licence (see
  <https://github.com/devkitPro/libogc/blob/master/LICENSE>). **AESND** is
  part of libogc and is credited for the DSP task boot protocol used by
  `platform/wii/dsp_mix.c`. The other devkitPro libraries used by the Wii link
  (`libfat`, `libwiiuse`, `libbte`) keep their respective licences; see the
  devkitPro packages.
- **Why it matters:** they are linked into the Wii binary. Their source is
  available from devkitPro. This repository's source is the corresponding
  source for the project code.

## Trademarks

Burnout 3: Takedown is a trademark of Electronic Arts Inc. Criterion Games is a
studio of Electronic Arts. Nintendo, Wii, GameCube, PlayStation and PS2 are
trademarks of their respective owners. This is an unofficial fan project and is
not affiliated with or endorsed by any of them.

## Credits

See `CREDITS.md` for the human-readable list of projects, authors and what
each one contributed. It is the “thank you” page; this file is the licence
record.

## Reporting a licensing problem

If you believe a file in this repository infringes your rights, please open an
issue or contact the maintainer. The file will be removed or the licence
corrected promptly.
