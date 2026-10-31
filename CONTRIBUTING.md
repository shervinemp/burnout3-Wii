# Contributing

Thanks for helping. This is a research port, so small, measured changes are
more useful than large rewrites.

## Licence of contributions

By submitting a change, you agree that it may be distributed under the
project's licence, **GPL-3.0-or-later**. Do not copy code from a project whose
licence is incompatible with GPL-3.0-or-later. If you adapt third-party code,
say so in the commit message and add the attribution to
`THIRD_PARTY_NOTICES.md` and `CREDITS.md`.

## Never commit game data

Do not add any of these to git:

- ISO, CHD, BIN/CUE, or other disc images
- extracted game files (`SLUS_210.50`, `IOP/*.IRX`, data archives)
- memory-card images (`*.ps2`)
- generated code (`gen/`, `gen_iop/`, `gen.*/`)
- build outputs (`build-*/`, `*.dol`, `*.elf`, `*.o`, `*.a`)

Run this before every commit or push:

```sh
python -I tools/check_repo.py
```

The check scans the git index, so it catches staged files even if they are
already deleted from disk. CI runs the same check on every push.

This project is source-only: no releases or artifacts may contain a built
`.dol`/`.elf`, an ISO, or generated `gen/`/`gen_iop/` code. See
[`RELEASE_POLICY.md`](RELEASE_POLICY.md).

## Building and testing

Follow `README.md`:

```sh
python -I tools/setup_assets.py --iso /path/to/your/burnout3.iso
make -f Makefile.wii -j8
```

For the PC host:

```sh
make -f Makefile.pc -j6
./build-pc/burnout3 --iso /path/to/your/burnout3.iso
```

If your change affects generated code, say which `config/` or `tools/recomp/`
change produced it. Do not commit the generated output.

## Maintainer publishing

This repository is mirrored to a public source-only repository. The local git
remotes are set up as:

- `origin` — private repository (fetch and push)
- `private` — private repository only
- `public` — public repository only

The public mirror is deliberately a **single-commit snapshot**, not the full
development history. Old public commits (for example earlier versions that
contained third-party patch text) are not published.

```sh
git push origin wii-port                  # private only
python -I tools/publish_public.py         # private first, then a fresh public snapshot
python -I tools/publish_public.py --skip-private   # re-publish public after a takedown
```

There is also a local alias:

```sh
git config alias.publish '!python -I tools/publish_public.py'
git publish
```

`publish_public.py` creates a root commit from the current `HEAD` tree and
force-pushes it to the public mirror. It does not change the private branch or
its history. The private push is attempted first; if it fails, the public
mirror is untouched. If the public push fails, the private repository still has
the commit.

Before publishing, run `python -I tools/check_repo.py`. Never publish a tree
containing a game image, `gen/`, `gen_iop/`, or a built binary. See
[`RELEASE_POLICY.md`](RELEASE_POLICY.md).
