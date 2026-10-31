# Burnout 3: Takedown — Wii static recompilation

An experimental source-level static recompilation of **Burnout 3: Takedown**
(PlayStation 2, NTSC-U, `SLUS-210.50`) for the **Nintendo Wii**.

The game's PS2 executable is lifted to C, the VU microcode is compiled to C,
and the PS2 hardware (EE, VU, IOP, DMA, VIF, GIF, GS, SPU2) is emulated by a
small runtime. On the Wii, GS drawing is translated to GX; a PC host build
exists for debugging.

> **This is an unofficial, non-commercial fan project.** It is not affiliated
> with, sponsored by, or endorsed by Electronic Arts, Criterion Games,
> Nintendo, Sony, PCSX2, Dolphin, or any other project. Burnout 3: Takedown and its assets are
> copyright Electronic Arts and its licensors.
>
> **No game assets are included.** You must own a legal copy of the game and
> supply your own disc image. Do not download an ISO. Do not publish prebuilt
> binaries or the generated `gen/`/`gen_iop/` trees: they are derived from the
> game's copyrighted executable.

## Status

This is a research/porting project, not a finished port.

- It boots, reaches the menus, loads races, and renders the world, cars, HUD,
  effects, and audio in Dolphin.
- The race start is around 25 ms of work per frame in plain Dolphin; the real
  Wii is slower. 60 fps is the long-term target; 30 fps is the practical target
  today.
- Input (GameCube pad, Classic Controller, Wii Remote + Nunchuk), rumble,
  memory-card saves, widescreen, and a DSP-based audio mixer are implemented.
- There is no public binary release, and there will not be one from this
  repository. Build it yourself from your own disc.

The long technical log is in `docs/HANDOFF.md`; the performance plan is in
`docs/plan-60fps.md`; the renderer notes are in `docs/native-renderer.md`.

## How it works

| Piece | What it does |
|---|---|
| `tools/recomp/` | Decodes the PS2 EE executable, analyses functions, and emits C (`gen/`). |
| `tools/vu/` | Recompiles VU0/VU1 microprograms to C (`gen/vu*.c`). |
| `tools/iop/` | Recompiles the game's IOP modules to C (`gen_iop/`). |
| `runtime/` | EE kernel, DMA/VIF/GIF/GS, SPU2/IOP HLE, VU interpreter, native overrides. |
| `platform/wii/` | libogc platform layer, GX backend, DSP mixer driver, profiler. |
| `platform/pc/` | POSIX host build used for development and reference rendering. |
| `config/` | Game-specific layout, function overrides, patches, hot-function list. |

Generated sources are **not** committed. `gen/`, `gen_iop/`, `build-*/`, and
`work/` are ignored.

## Requirements

- **devkitPro** with the Wii development packages:
  - devkitPPC (GCC for PowerPC)
  - libogc **3.1.0 or newer** (older versions can freeze in the GX FIFO IRQ)
  - `elf2dol`, `gcdsptool`
- **Python 3.10+** (3.13 is what the project is developed with). Only the
  standard library is needed.
- **GNU make**, `git`, and a POSIX shell (devkitPro's MSYS2 on Windows, or any
  normal shell on Linux/macOS).
- A legally owned **Burnout 3: Takedown** disc image:
  - PlayStation 2, NTSC-U, `SLUS-210.50`
  - ISO format (2.9 GB). CHD can be converted with `tools/chd2iso`; see below.
- For the console: a Wii with the Homebrew Channel, an SD card and/or a
  FAT32/NTFS USB drive.
- For testing: Dolphin.

Install the Wii toolchain with devkitPro's pacman if you do not have it:

```sh
pacman -S wii-dev
```

## Quick start (Wii)

### 1. Get the source

```sh
git clone https://github.com/shervinemp/burnout3-Wii.git
cd burnout3-Wii
```

### 2. Extract the build inputs and generate the C

```sh
python -I tools/setup_assets.py --iso /path/to/burnout3.iso
```

The script:

1. Checks that the image is the NTSC-U release (`SYSTEM.CNF` says
   `SLUS_210.50`).
2. Extracts `SYSTEM.CNF`, `SLUS_210.50`, and the IOP modules listed in
   `config/iop_modules.py` to `work/disc/`.
3. Runs the EE recompiler into `gen/`.
4. Runs the IOP recompiler into `gen_iop/`.

It does **not** put the ISO or the generated code in git. The Wii build reads
the ISO directly from the SD card or USB drive at runtime.

Useful options:

```sh
# You already extracted the disc before:
python -I tools/setup_assets.py --disc work/disc

# Also extract the whole disc (needed only for file-based PC runs):
python -I tools/setup_assets.py --iso /path/to/burnout3.iso --full

# Keep the generated files somewhere else:
python -I tools/setup_assets.py --iso game.iso --gen /tmp/gen --gen-iop /tmp/gen_iop

# Use a symbol file for nicer generated names:
python -I tools/setup_assets.py --iso game.iso --symbols work/symbol_addrs.txt
```

If the recompiler rejects the executable with an `ELF load segment SHA-1`
message, the image is the wrong region, a different revision, or damaged. This
port is configured only for NTSC-U `SLUS-210.50`.

### 3. Build

In a devkitPro MSYS2 shell on Windows:

```sh
export DEVKITPRO=/opt/devkitpro
make -f Makefile.wii -j8
```

On Linux/macOS with devkitPro installed under `/opt/devkitpro`:

```sh
make -f Makefile.wii -j"$(nproc)"
```

The output is:

```
build-wii/burnout3.dol
build-wii/burnout3.elf
```

`-j8` is a good starting point. `-j32` can run the compiler out of memory.

### 4. Package for the Wii

```sh
sh tools/wii/package.sh /path/to/sd-root /path/to/burnout3.iso
```

This creates:

```
<SD>/apps/burnout3/boot.dol
<SD>/apps/burnout3/meta.xml
<SD>/apps/burnout3/README.txt
<SD>/burnout3/burnout3.iso
```

Or split the app and data between an SD card and a USB drive:

```sh
sh tools/wii/package.sh --split /path/to/sd-root /path/to/usb-root /path/to/burnout3.iso
```

The app searches, in order:

```
sd:/burnout3, usb:/burnout3, sd:/apps/burnout3, usb:/apps/burnout3
```

and then the NTFS mounts (`ntfs0:/burnout3`, `ntfs1:/burnout3`, ...). A folder
is used only if it contains `burnout3.iso`.

### 5. Run

Start **Burnout 3 (recompiled)** from the Homebrew Channel. The log is
`run.log` in the data folder. The controls and every `args.txt` option are
documented in `platform/wii/hbc/README.txt` (the same file is packaged with the
app).

Common options in `args.txt` (one per line, in the data folder next to the ISO):

```text
--fps 30            force the game's 30 fps mode
--fps 60            force 60 fps
--widescreen 2      follow the Wii's 16:9 setting (default)
--pad 1             Nintendo face-button positions (default)
--dsp 1             use the Wii DSP audio mixer (console default)
--rumble 0          disable controller rumble
--clip 0            disable the CPU clipper
```

## Memory card

The game expects a formatted 8 MB PS2 memory card:

```
sd:/burnout3/saves/Mcd001.ps2
```

or `saves/Mcd001.ps2` in the data folder. The file must be a raw, formatted
card (8,650,752 bytes), not a file full of `0xFF`.

Ways to get one:

- Create an 8 MB raw memory card in Dolphin's Memory Card Manager and copy the
  resulting file to `saves/Mcd001.ps2`.
- Use a card formatted on a PS2 and copy it with a homebrew memory-card manager.
- If `work/saves/Mcd001.ps2` exists, `tools/wii/package.sh` copies it for you.

## Dolphin

Load `build-wii/burnout3.dol` directly in Dolphin, or package it to an
emulated SD card. The app still needs the ISO at `sd:/burnout3/burnout3.iso` (or
one of the other roots above).

`tools/wii/fatcat.py` can read/write a FAT32 SD image while Dolphin is closed:

```sh
python -I tools/wii/fatcat.py /path/to/WiiSD.raw --put burnout3.iso burnout3/burnout3.iso
python -I tools/wii/fatcat.py /path/to/WiiSD.raw burnout3/run.log out.log
```

Dolphin's log (`Logs/dolphin.log`, usually `OSREPORT`) also contains the app's
log lines. The app also writes `run.log` in its data folder.

## PC development host

The PC build is a debugging host, not a playable PC port. It uses the software
GS renderer and the recompiled game code.

```sh
make -f Makefile.pc -j6
./build-pc/burnout3 --iso /path/to/burnout3.iso
```

If you extracted the whole disc with `--full`, you can omit `--iso` and use the
extracted files in `work/disc`:

```sh
./build-pc/burnout3 --data work/disc
```

Useful PC options:

```text
--iso FILE          read the game from an ISO
--data DIR          read files from an extracted disc (default: work/disc)
--log FILE          log file (default: work/run.log)
--input FILE        scripted controller input
--time-scale N      speed up or slow down host time
--fpu-nearest       use the host's FPU rounding instead of PS2 round-to-zero
```

The PC host writes `work/run.log` and `work/saves/`.

## CHD images

`tools/chd2iso` is an optional helper that writes the 2048-byte user-data
sectors of a CHD as an ISO. It needs a libchdr checkout (BSD-3-Clause; not
vendored):

```sh
git clone https://github.com/rtissera/libchdr /path/to/libchdr
cmake -S tools/chd2iso -B build-chd2iso -DLIBCHDR_DIR=/path/to/libchdr
cmake --build build-chd2iso -j
./build-chd2iso/chd2iso game.chd game.iso
```

Then use `game.iso` with the setup script and the Wii build.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `generated sources missing` or undefined `ps2_*` symbols | Run `tools/setup_assets.py` first; `gen/` is not in git. |
| `ELF load segment SHA-1 ... does not match` | Wrong region/revision or damaged ISO. This port needs NTSC-U `SLUS-210.50`. |
| `no burnout3.iso on SD or USB` | Put the ISO at `sd:/burnout3/burnout3.iso` or `usb:/burnout3/burnout3.iso`; check the case and folder. |
| `disc image is not ISO9660` | The file is a CHD, a compressed archive, or a raw CD image. Convert it to ISO first. |
| Game hangs at the end of loading | The ISO copy is incomplete (NTFS `Vdl` shorter than `Size`). Copy it again and verify. |
| No profile / save option | No formatted PS2 memory card. See "Memory card" above. |
| Freeze in the GX FIFO IRQ | libogc is older than 3.1.0. Update devkitPro/libogc. |
| Build runs out of memory | Lower `-j` (for example `-j6` or `-j8`). |
| `elf2dol: command not found` | The devkitPro Wii tools are not installed or `DEVKITPRO` is wrong. |
| App starts but is very slow on console | Expected. The project is not yet at 60 fps on real hardware. |

## Releases

This repository is **source-only**. There are no binary releases and there
should not be any: no ISOs, no extracted assets, no `gen/`/`gen_iop/`, and no
built `.dol`/`.elf`. See [`RELEASE_POLICY.md`](RELEASE_POLICY.md). Build and run
it locally from your own legally owned disc.

## Licensing

This project is licensed under **GNU GPL v3 or later**
(`GPL-3.0-or-later`). See:

- `LICENSE` — the full GPL v3 text
- `NOTICE` — copyright, trademark, and game-asset notice
- `THIRD_PARTY_NOTICES.md` — third-party components and their licences
- `LICENSES/` — GPL-2.0-or-later, GPL-3.0-or-later, and BSD-3-Clause texts

Why GPL-3.0-or-later?

- `external/libntfs` is **GPL-2.0-or-later** and is linked into the Wii binary.
- `runtime/gs.c`/`runtime/gs.h` contain GS swizzle tables derived from
  **PCSX2** (`GPL-3.0-or-later`); several other runtime files follow PCSX2's
  documented hardware semantics and are covered by the same licence.
- `tools/chd2iso` optionally links **libchdr** (BSD-3-Clause), which is
  compatible.

GPL-2.0-or-later can be used under GPL-3.0-or-later, so one project licence
covers the combined work. The third-party files keep their own notices. Files
with a different SPDX identifier or an explicit third-party copyright header
are covered by that notice, not by the project's copyright.

**What you may distribute:** the original source in this repository, under the
GPL. **What you should not distribute:** the game ISO, extracted game files,
`gen/`, `gen_iop/`, or a built `.dol`/`.elf`. Those contain or are derived from
the game's copyrighted executable. If you distribute a binary, GPL requires you
to provide its corresponding source — which includes the generated code. Do not
put yourself in that position without permission from the rights holder.

This is not legal advice. If you plan to distribute binaries or a patched game,
talk to a lawyer.

## Repository layout

```
config/                 game-specific recompiler config and analysis data
docs/                   design notes, handoff log, performance plans
external/libntfs/       vendored NTFS-3G/libntfs (GPL-2.0-or-later)
platform/pc/            POSIX host layer
platform/wii/           Wii platform, GX backend, DSP mixer
runtime/                PS2 hardware emulation and recompiled-code support
tools/recomp/           EE static recompiler
tools/vu/               VU microcode recompiler
tools/iop/              IOP module recompiler
tools/iso/              ISO9660 extractor
tools/wii/              packaging, FAT image, DOL and DSP checks
tools/chd2iso/          optional CHD-to-ISO helper
tools/setup_assets.py   extract your disc and generate the C
tools/check_repo.py     guard against committing game assets or generated code
tools/publish_public.py publish a single source snapshot to the public mirror
CREDITS.md              full credits for the projects and people used
RELEASE_POLICY.md       source-only release and no-binaries policy
```

## Contributing

Contributions are welcome under `GPL-3.0-or-later`. See `CONTRIBUTING.md` for
the asset policy and the maintainer dual-push setup.

Before committing or pushing:

```sh
python -I tools/check_repo.py
```

The check refuses game images, executables, memory cards, `gen/`, `gen_iop/`,
`build-*/`, and very large files in the git index. Never add a game asset, a
generated source tree, or a built binary to a commit.

Please do not include ROMs, ISOs, decrypted files, game data, or links to
piracy in issues or pull requests.

## Credits

The full list, with links and what each project contributed, is in
[`CREDITS.md`](CREDITS.md). In short:

- **PCSX2** — GS swizzle tables, VIF/VU/SIO2/SPU2 behaviour references, and the
  widescreen patch by **Aero_**.
- **Dolphin** — GX/DSP behaviour reference.
- **PS2SDK / ps2dev** — libsd/libkernel/libmc/libpad/SIF protocol references
  and SPU2 reverb presets.
- **extrems / libntfs** and **NTFS-3G** — NTFS support on the Wii.
- **libchdr / MAME** — optional CHD conversion.
- **Burnout3 decompilation** by siddharthakumar-98 — section-layout reference.
- **devkitPro / libogc / AESND** — Wii toolchain, runtime and DSP boot protocol.
- **Criterion Games / Electronic Arts** — the original Burnout 3: Takedown.
- **reVC-Wii** — the author's earlier Wii port, used as a GX/packaging reference.
- Everyone who tested, measured, and documented the port.
