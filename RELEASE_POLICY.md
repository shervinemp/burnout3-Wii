# Release policy

This repository is **source-only**. There are no binary releases, and there
should not be any.

Do **not** publish or attach any of these to a release, GitHub Actions
artifact, package, website, or mirror:

- `burnout3.iso`, CHD/BIN/CUE images, or any extracted game files
- `gen/` or `gen_iop/` (generated from the game's executable)
- `build-wii/burnout3.dol` or `build-wii/burnout3.elf`
- `build-pc/burnout3` or any other built binary
- memory-card images, saves, or game data

## Why

A built binary contains the recompiled game code, which is derived from
Burnout 3's copyrighted executable. Distributing it would raise copyright
questions that this project deliberately avoids, and the GPL's
corresponding-source requirement would pull the generated `gen/`/`gen_iop/`
trees into the distribution as well.

The runtime, recompiler, tools and documentation are original source and are
licensed under GPL-3.0-or-later. The game is not.

## If you build it

Build and run it locally from your own legally owned disc. Do not upload the
result. If you want to share a build, share instructions and source changes,
not the binary.

## Forks and mirrors

Forks and mirrors should follow the same rule. A source-only mirror is what
this project is designed to be. Use `tools/check_repo.py` before publishing;
CI runs the same check.

## Non-commercial

This is an unofficial, non-commercial fan project. Do not sell it, put it
behind a paywall, add monetised ads to it, or present it as an official
EA/Criterion/Nintendo product. Do not use official logos or cover art.

## Takedown requests

The public mirror is separate from the private development repository, so a
takedown of the public repository does not remove the private one. If a
copyright claim arrives, evaluate it with counsel before responding. A
counter-notice is an option for a mistaken claim, but it is not a substitute
for legal advice.

This policy is not legal advice.
