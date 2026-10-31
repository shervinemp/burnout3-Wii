#!/usr/bin/env python3
"""Extract the build inputs from your own Burnout 3 disc and generate the C.

    python -I tools/setup_assets.py --iso /path/to/burnout3.iso
    python -I tools/setup_assets.py --disc work/disc

The disc image is not copied into the repository. Only the executable and the
IOP modules needed by the recompilers are extracted, into work/disc by default.
The Wii runtime reads the ISO directly from the SD card or USB drive.

The script then runs:

    tools/recomp      -> gen/
    tools/iop/regen.py -> gen_iop/

Use --full to extract the whole ISO as well (only needed for a file-based PC
run), and --disc to reuse an extraction you already have.

SPDX-License-Identifier: GPL-3.0-or-later
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import shutil
import subprocess
import sys
from pathlib import Path
from typing import NoReturn

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DISC = ROOT / "work" / "disc"
DEFAULT_GEN = ROOT / "gen"
DEFAULT_GEN_IOP = ROOT / "gen_iop"
GAME_ELF = "SLUS_210.50"
SYSTEM_CNF = "SYSTEM.CNF"


def fail(message: str) -> NoReturn:
    print(f"\n[setup] error: {message}", file=sys.stderr)
    raise SystemExit(1)


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        fail(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def iop_files() -> list[str]:
    cfg = load_module("iop_modules", ROOT / "config" / "iop_modules.py")
    return sorted({entry["file"] for entry in cfg.MODULES.values()})


def read_iso_file(iso: Path, wanted: str) -> bytes | None:
    isoextract = load_module("isoextract", ROOT / "tools" / "iso" / "isoextract.py")
    with open(iso, "rb") as f:
        for path, lba, size in isoextract.walk(f):
            if path.upper().split(";")[0] == wanted.upper():
                f.seek(lba * isoextract.SECTOR)
                return f.read(size)
    return None


def check_iso(iso: Path) -> None:
    if not iso.is_file():
        fail(f"disc image not found: {iso}")
    if iso.suffix.lower() == ".chd":
        fail(
            "CHD input is not supported directly. Convert it to ISO first:\n"
            "  see the 'CHD images' section of README.md (tools/chd2iso)"
        )
    size = iso.stat().st_size
    if size < 512 * 1024 * 1024:
        print(f"[setup] warning: {iso.name} is only {size / (1024 ** 3):.2f} GiB; "
              "a complete Burnout 3 DVD image is about 2.9 GiB", file=sys.stderr)

    try:
        cnf = read_iso_file(iso, SYSTEM_CNF)
    except AssertionError:
        fail(
            "the image is not an ISO9660 image. Use a 2048-byte/sector ISO "
            "(for CHD, see the 'CHD images' section of README.md)."
        )
    except Exception as exc:  # noqa: BLE001 - report a useful message for any bad file
        fail(f"cannot read {iso}: {exc}")
    if cnf is None:
        fail(
            "SYSTEM.CNF was not found in the image. The image must be a "
            "2048-byte/sector ISO9660 PS2 DVD image, not a raw CD image or an archive."
        )
    text = cnf.decode("ascii", "replace").upper().replace("-", "_")
    if "SLUS_210.50" not in text:
        fail(
            "this is not the NTSC-U release SLUS_210.50.\n"
            "This port is configured only for Burnout 3: Takedown (PS2, NTSC-U)."
        )
    if "NTSC" not in text:
        print("[setup] warning: SYSTEM.CNF does not say NTSC; "
              "continuing, but the recompiler may reject the executable", file=sys.stderr)


def check_extracted(disc: Path, config: Path) -> None:
    elf = disc / GAME_ELF
    if not elf.is_file():
        fail(f"{elf} is missing. Run the script with --iso, or extract the disc to {disc}.")
    try:
        elf_mod = load_module("elf", ROOT / "tools" / "recomp" / "elf.py")
        image = elf_mod.Image(str(elf))
    except Exception as exc:  # noqa: BLE001 - report a useful message for any bad file
        fail(f"cannot read {elf}: {exc}")
    title = load_module("title_config", config)
    want = getattr(title, "ELF_SHA1_ROM", None)
    if not want:
        return
    got = hashlib.sha1(image.data).hexdigest()
    if got != want:
        fail(
            f"{elf} is not the executable this port expects.\n"
            f"  expected SHA-1 {want}\n"
            f"  got          {got}\n"
            "Use an unmodified NTSC-U SLUS_210.50 image."
        )


def extract(iso: Path, disc: Path, full: bool) -> None:
    print(f"[setup] extracting {'the whole disc' if full else 'build inputs'} to {disc}")
    cmd = [sys.executable, "-I", str(ROOT / "tools" / "iso" / "isoextract.py"),
           str(iso), str(disc)]
    if not full:
        cmd += [SYSTEM_CNF, GAME_ELF, *iop_files()]
    subprocess.run(cmd, cwd=ROOT, check=True)


def is_generated_ee(path: Path) -> bool:
    name = path.name
    return (
        (name.startswith("recomp_") and name.endswith(".c"))
        or name in {"funcs.h", "functable.c", "vu_progs.c"}
        or ((name.startswith("vu0_") or name.startswith("vu1_")) and name.endswith(".c"))
    )


def sync_tree(src: Path, dst: Path) -> int:
    """Copy changed files, keep unchanged mtimes, remove stale generated files."""
    dst.mkdir(parents=True, exist_ok=True)
    src_files = {p.relative_to(src): p for p in src.rglob("*") if p.is_file()}
    changed = 0
    for rel, source in src_files.items():
        target = dst / rel
        if not target.is_file() or target.read_bytes() != source.read_bytes():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
            changed += 1
    for target in list(dst.rglob("*")):
        if target.is_file() and target.relative_to(dst) not in src_files:
            if is_generated_ee(target):
                target.unlink()
    return changed


def run_recompiler(disc: Path, gen: Path, config: Path, symbols: Path | None) -> None:
    tmp = gen.with_name(gen.name + ".new")
    shutil.rmtree(tmp, ignore_errors=True)
    cmd = [sys.executable, "-I", str(ROOT / "tools" / "recomp"),
           str(disc / GAME_ELF), "--config", str(config), "--out", str(tmp)]
    if symbols is not None:
        cmd += ["--symbols", str(symbols)]
    print("[setup] recompiling the PS2 executable")
    try:
        subprocess.run(cmd, cwd=ROOT, check=True)
        changed = sync_tree(tmp, gen)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"[setup] gen/: {changed} file(s) changed")


def run_iop(disc: Path, gen_iop: Path, force: bool) -> None:
    if force:
        shutil.rmtree(gen_iop, ignore_errors=True)
    print("[setup] recompiling the IOP modules")
    subprocess.run(
        [sys.executable, "-I", str(ROOT / "tools" / "iop" / "regen.py"),
         "--disc", str(disc), "--out", str(gen_iop)],
        cwd=ROOT,
        check=True,
    )


def main() -> None:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)
    ap = argparse.ArgumentParser(
        description="Extract the Burnout 3 build inputs and generate the C sources.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "examples:\n"
            "  python -I tools/setup_assets.py --iso game.iso\n"
            "  python -I tools/setup_assets.py --disc work/disc --no-symbols\n"
        ),
    )
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--iso", type=Path, help="your own Burnout 3 NTSC-U ISO")
    src.add_argument("--disc", type=Path, help="an already extracted disc directory")
    ap.add_argument("--full", action="store_true",
                    help="extract the whole ISO (needed only for file-based PC runs)")
    ap.add_argument("--gen", type=Path, default=DEFAULT_GEN, help="EE output directory")
    ap.add_argument("--gen-iop", type=Path, default=DEFAULT_GEN_IOP, help="IOP output directory")
    ap.add_argument("--symbols", type=Path,
                    help="optional symbol_addrs.txt for readable generated names")
    ap.add_argument("--no-symbols", action="store_true",
                    help="do not use work/symbol_addrs.txt even if it exists")
    ap.add_argument("--skip-iop", action="store_true", help="do not regenerate gen_iop")
    ap.add_argument("--force", action="store_true",
                    help="delete gen_iop first (full IOP rebuild)")
    args = ap.parse_args()
    if args.full and args.iso is None:
        ap.error("--full requires --iso")

    config = ROOT / "config" / "burnout3.py"
    disc = args.disc.expanduser().resolve() if args.disc else DEFAULT_DISC
    iso = args.iso.expanduser().resolve() if args.iso else None
    gen = args.gen.expanduser().resolve()
    gen_iop = args.gen_iop.expanduser().resolve()

    if iso is not None:
        check_iso(iso)
        extract(iso, disc, args.full)
    elif args.disc is None:
        if (DEFAULT_DISC / GAME_ELF).is_file():
            disc = DEFAULT_DISC
            print(f"[setup] using the existing extraction in {disc}")
        else:
            ap.error("give --iso /path/to/burnout3.iso, or --disc work/disc")

    check_extracted(disc, config)

    symbols = args.symbols.expanduser().resolve() if args.symbols else None
    if symbols is None and not args.no_symbols:
        default_symbols = ROOT / "work" / "symbol_addrs.txt"
        if default_symbols.is_file():
            symbols = default_symbols
            print(f"[setup] using {default_symbols.relative_to(ROOT)} for symbol names")
    if symbols is not None and not symbols.is_file():
        fail(f"symbol file not found: {symbols}")

    try:
        run_recompiler(disc, gen, config, symbols)
        if not args.skip_iop:
            run_iop(disc, gen_iop, args.force)
    except subprocess.CalledProcessError as exc:
        fail(f"command failed with exit code {exc.returncode}: {' '.join(map(str, exc.cmd))}")

    print("\n[setup] assets ready")
    print(f"  disc:     {disc}")
    print(f"  EE code:  {gen}")
    if not args.skip_iop:
        print(f"  IOP code: {gen_iop}")
    print("\nBuild the Wii app with:")
    print("  make -f Makefile.wii -j8")
    print("Package it with:")
    print("  sh tools/wii/package.sh /path/to/sd-root /path/to/burnout3.iso")


if __name__ == "__main__":
    main()
