# Credits and thanks

This port exists because of a lot of prior work. The list below says what each
project/person contributed and where to find them. Licence details are in
`THIRD_PARTY_NOTICES.md`; this file is the human-readable “thank you”.

## The original game

- **Burnout 3: Takedown** — developed by Criterion Games, published by
  Electronic Arts. The game, its executable, assets, audio, textures and
  trademarks belong to EA and its licensors. This project does not include
  them and is not affiliated with or endorsed by EA or Criterion.
- **RenderWare** — the middleware the original game uses. No RenderWare SDK
  code is included here; its behaviour is part of the game being emulated.

## Emulation and hardware references

- **PCSX2** — <https://github.com/PCSX2/pcsx2>
  - GS block/column swizzle tables used in `runtime/gs.c`/`runtime/gs.h`.
  - Behavioural references for VIF UNPACK, VU pipeline stalls/EFU, SIO2 /
    DualShock 2 / memory-card protocols, and SPU2 reverb.
  - PCSX2 is GPL-3.0-or-later.
- **Aero_** and the **pcsx2_patches** project —
  <https://github.com/PCSX2/pcsx2_patches>
  - The “Widescreen 16:9” patch for `SLUS-21050_BEBF8793.pnach`. The current
    source keeps only the factual address/size/value writes, with attribution.
- **Dolphin** — <https://github.com/dolphin-emu/dolphin>
  - GX and DSP behaviour reference while building the Wii backend and audio
    path.
- **PS2SDK / ps2dev** — <https://github.com/ps2dev/ps2sdk>
  - libsd, libkernel, libmc, libpad, SIFCMD/SIFRPC API and protocol
    references, plus the SPU2 reverb presets/coefficients.
  - No PS2SDK source is vendored here; the runtime is written for this project
    from those references. Credited because the API/protocol/preset references
    come from PS2SDK. PS2SDK is AFL-2.0, which is not GPL-compatible, so its
    source must not be copied into this GPL work without checking the terms.
- **libogc / AESND** — <https://github.com/devkitPro/libogc>
  - GX API, DSP task boot protocol (AESND), threads, timing and the Wii
    platform layer. libogc is a build dependency, not vendored.

## Libraries

- **libntfs** (Wii port by **extrems**) —
  <https://github.com/extremscorner/libntfs>
  - Vendored in `external/libntfs` for NTFS support. GPL-2.0-or-later.
- **NTFS-3G** — <https://github.com/tuxera/ntfs-3g>
  - The upstream NTFS driver that libntfs is based on. GPL-2.0-or-later.
- **libchdr / MAME** — <https://github.com/rtissera/libchdr>
  - Optional CHD-to-ISO helper (`tools/chd2iso`). BSD-3-Clause. Not vendored.
- **devkitPro** — <https://devkitpro.org>
  - devkitPPC, libogc, libfat, libwiiuse, libbte, `elf2dol`, `gcdsptool` and
    the Homebrew Channel packaging conventions.

## Game-specific references

- **Burnout3 decompilation** by **siddharthakumar-98** —
  <https://github.com/siddharthakumar-98/Burnout3>
  - Section boundaries and layout facts referenced in `config/burnout3.py`.
    No decompilation source is included. GPL-3.0.
- **The author's reVC-Wii project** — an earlier Wii port, used as a reference
  for GX backend patterns and Homebrew Channel packaging.

## This project

- **Shervin Naseri (`sherv`)** — original recompiler, runtime, Wii backend,
  tools and documentation.
- **Contributors** — everyone who has sent fixes, measurements, test logs or
  feedback. See the git history for the full list.

## A note on attribution

If you believe a source is missing from this file, or that a piece of code is
closer to another project than the notes say, please open an issue. The goal is
to give credit accurately and to fix any licensing mistake promptly.

The licence for this project's original code is GPL-3.0-or-later; see
`LICENSE`. Third-party components keep their own licences; see
`THIRD_PARTY_NOTICES.md` and `LICENSES/`.
