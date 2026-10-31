"""PS2 static recompiler driver.

    EE executable:
        python -I tools/recomp <elf> --config config/burnout3.py --out gen [--symbols symbol_addrs.txt]
    IOP module, relocated to a fixed IOP address:
        python -I tools/recomp <MODULE.IRX> --irx-base 0x40000 --module rwa --out gen_iop/rwa
"""
import argparse
import hashlib
import importlib.util
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "iop"))

import lift                                                # noqa: E402
import patches                                             # noqa: E402
from analyze import Config, Function, Program, read_symbol_funcs   # noqa: E402
from elf import Image                                      # noqa: E402


def load_config(path):
    spec = importlib.util.spec_from_file_location("title_config", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def setup_ee(args):
    title = load_config(args.config)
    img = Image(args.elf)
    sha = hashlib.sha1(img.data).hexdigest()
    if sha != title.ELF_SHA1_ROM:
        sys.exit(f"ELF load segment SHA-1 {sha} does not match {title.ELF_SHA1_ROM}")
    cfg = Config(title.TEXT_RANGES, title.DATA_RANGES, title.EXTRA_FUNCS, title.NOT_FUNCS)
    prog = Program(img, cfg)
    prog.overrides = dict(getattr(title, "OVERRIDES", {}))
    prog.override_vu0 = set(getattr(title, "OVERRIDE_VU0", ()))
    prog.debug_wrap = set(getattr(title, "DEBUG_WRAP", ()))
    prog.io_funcs = set(getattr(title, "IO_FUNCS", ()))
    prog.entry_hooks = dict(getattr(title, "ENTRY_HOOKS", {}))
    prog.vcall_sites = dict(getattr(title, "VCALLMS_SITES", {}))
    from emit import parse_res   # noqa: E402
    prog.vcall_inline = {pc: (fn, parse_res(r), parse_res(w))
                         for pc, (fn, r, w) in getattr(title, "VCALLMS_INLINE", {}).items()}
    prog.inline_variants = set(getattr(title, "INLINE_VARIANTS", ()))
    prog.hot_funcs = set(getattr(title, "HOT_FUNCS", ()))
    prog.manual_jump_tables = dict(getattr(title, "JUMP_TABLES", {}))   # jr pc -> (table address, entries)
    prog.code_patches = patches.build(img, title.TEXT_RANGES, getattr(title, "CODE_PATCHES", {}))
    prog.ind_safe = prog.ind_safe_mw = 0
    prog.must_ret = {}
    prog.ind_summary = None
    return title, prog


def setup_iop(args):
    from irxload import IrxImage
    base = int(args.irx_base, 0)
    img = IrxImage(args.elf, base)
    mod = args.module
    t0, tsize = img.text
    d0, dsize = img.sections.get(".data", (0, 0))
    exported = {f for funcs in img.exports.values() for f in funcs}
    cfg = Config(img.code_ranges(), [(base + d0, base + d0 + dsize)] if dsize else [], {img.entry} | exported, set())
    prog = Program(img, cfg)
    Function.prefix = prog.prefix = f"{mod}_"
    prog.table = f"iop_{mod}_table"
    prog.defines = ["#define PS2_IOP 1"]
    prog.wide64 = False
    prog.overrides = {a: f"{mod}_imp_{lib}_{n}" for a, (lib, n) in img.imports.items()}
    if args.config:
        spec = load_config(args.config).MODULES[mod]
        prog.manual_jump_tables = {base + pc: (base + t, n) for pc, (t, n) in spec.get("jump_tables", {}).items()}
        prog.debug_wrap = {base + a for a in spec.get("debug_wrap", ())}
    lift.IO_TEST[0] = lift.iop_io_addr
    return img, prog


def write_iop_support(img, prog, out_dir, mod):
    """imports.c: one trampoline per import stub; module.c: the relocated image."""
    with open(os.path.join(out_dir, "imports.c"), "w", newline="\n") as f:
        f.write("/* generated: calls from the module to other IOP libraries */\n#include \"funcs.h\"\n")
        f.write("#include \"iop.h\"\n\n")
        for a, (lib, n) in sorted(img.imports.items()):
            name = prog.overrides[a]
            f.write(f"static IopImportSlot s_{a:08X} = {{\"{lib}\", {n}, 0}};   /* {img.import_name(lib, n)} */\n")
            f.write(f"void {name}(R5900 *c) {{ iop_import(c, &s_{a:08X}); }}\n")
    words = [img.u32(img.base + k) for k in range(0, len(img.data) & ~3, 4)]
    with open(os.path.join(out_dir, "module.c"), "w", newline="\n") as f:
        f.write("/* generated: relocated module image (from the game disc: do not distribute) */\n")
        f.write("#include \"funcs.h\"\n#include \"iop.h\"\n\n")
        f.write(f"static const uint32_t image[{len(words)}] = {{\n")
        for k in range(0, len(words), 8):
            f.write("    " + ", ".join(f"0x{w:08X}u" for w in words[k:k + 8]) + ",\n")
        f.write("};\n\n")
        f.write(f"extern const PS2FuncEntry {prog.table}[];\nextern const unsigned {prog.table}_count;\n\n")
        upper = re.sub(r"[^A-Z0-9]", "_", os.path.splitext(os.path.basename(img.path))[0].upper())
        f.write(f"const IopModuleImage iop_module_{mod} = {{\n")
        f.write(f"    \"{upper}\", 0x{img.base:08X}u, image, {len(words) * 4}u, 0x{img.memsz:X}u, "
                f"0x{img.entry:08X}u,\n")
        f.write(f"    {prog.table}, &{prog.table}_count,\n}};\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--config")
    ap.add_argument("--symbols")
    ap.add_argument("--out")
    ap.add_argument("--analyze-only", action="store_true")
    ap.add_argument("--funcs-per-file", type=int, default=400)
    ap.add_argument("--irx-base", help="recompile an IOP module relocated to this IOP address")
    ap.add_argument("--module", help="C name of the IOP module (with --irx-base)")
    args = ap.parse_args()

    t0 = time.time()
    if args.irx_base:
        if not args.module:
            sys.exit("--module is required with --irx-base")
        img, prog = setup_iop(args)
        title = None
        sym = {}
    else:
        if not args.config:
            sys.exit("--config is required")
        title, prog = setup_ee(args)
        sym = read_symbol_funcs(args.symbols) if args.symbols else {}
    prog.collect_seeds(set(sym.keys()) | (set() if args.irx_base else set(prog.overrides)))
    prog.analyze()
    funcs = prog.funcs
    n_insn = sum(len(f.insns) for f in funcs.values())
    n_tables = sum(len(f.jump_tables) for f in funcs.values())
    n_unres = sum(1 for f in funcs.values() for pc, i in f.insns.items()
                  if i.op == "jr" and i.rs != 31 and pc not in f.jump_tables)
    print(f"{len(funcs)} functions, {n_insn} reachable instructions, "
          f"{n_tables} jump tables, {n_unres} unresolved jr, {time.time() - t0:.1f}s")
    if args.analyze_only or not args.out:
        return
    from emit import emit_program   # noqa: E402
    emit_program(prog, title, args.out, args.funcs_per_file, names=sym)
    if title is not None and hasattr(title, "VU_CHAINS"):
        # the VU microprograms, recompiled from the same executable
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "vu"))
        import vurecomp                                    # noqa: E402
        live = getattr(title, "VU0_LIVE", None)
        live = vurecomp.read_vu0_live(os.path.join(os.path.dirname(os.path.abspath(args.config)), live)) if live else None
        vurecomp.compile_all(Image(args.elf), title.VU_CHAINS, set(getattr(title, "VU0_CHAINS", [])),
                             getattr(title, "VU_ENTRIES", {}), args.out, title.TEXT_RANGES,
                             getattr(title, "VU_NATIVE_SUBS", {}), live)
    if args.irx_base:
        write_iop_support(img, prog, args.out, args.module)
    print(f"done in {time.time() - t0:.1f}s")


if __name__ == "__main__":
    main()
