"""Recompile every IOP module listed in config/iop_modules.py.

    python -I tools/iop/regen.py --disc work/disc --out gen_iop
"""
import argparse
import filecmp
import importlib.util
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disc", default=os.path.join(ROOT, "work", "disc"))
    ap.add_argument("--out", default=os.path.join(ROOT, "gen_iop"))
    ap.add_argument("--config", default=os.path.join(ROOT, "config", "iop_modules.py"))
    args = ap.parse_args()
    spec = importlib.util.spec_from_file_location("iop_modules", args.config)
    cfg = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cfg)
    names = []
    for mod, m in cfg.MODULES.items():
        tmp = os.path.join(args.out, mod + ".new")
        dst = os.path.join(args.out, mod)
        shutil.rmtree(tmp, ignore_errors=True)
        r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "recomp"),
                            os.path.join(args.disc, m["file"]), "--irx-base", hex(m["base"]),
                            "--module", mod, "--config", args.config, "--out", tmp],
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit(f"{mod}: recompiler failed\n{r.stdout}{r.stderr}")
        summary = [ln for ln in r.stdout.splitlines() if "functions," in ln]
        print(f"{mod}: {summary[-1] if summary else 'ok'}")
        os.makedirs(dst, exist_ok=True)
        for f in os.listdir(tmp):          # replace only changed files so make rebuilds the minimum
            a, b = os.path.join(tmp, f), os.path.join(dst, f)
            if not os.path.exists(b) or not filecmp.cmp(a, b, shallow=False):
                shutil.copyfile(a, b)
        shutil.rmtree(tmp)
        names.append(mod)
    with open(os.path.join(args.out, "modules.c.new"), "w", newline="\n") as f:
        f.write("/* generated: list of recompiled IOP modules */\n#include \"iop.h\"\n\n")
        for mod in names:
            f.write(f"extern const IopModuleImage iop_module_{mod};\n")
        f.write("\nconst IopModuleImage *const iop_modules[] = {\n")
        for mod in names:
            f.write(f"    &iop_module_{mod},\n")
        f.write("    0,\n};\n")
    a, b = os.path.join(args.out, "modules.c.new"), os.path.join(args.out, "modules.c")
    if os.path.exists(b) and filecmp.cmp(a, b, shallow=False):
        os.remove(a)
    else:
        os.replace(a, b)


if __name__ == "__main__":
    main()
