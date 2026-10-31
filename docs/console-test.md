# The console test (2026-10-23)

Everything below is one sitting with the Wii and the build this commit makes (`build-wii/burnout3.dol`).
Dolphin cannot answer any of it: it has no fixed-point rasteriser and no cache model. Send back `run.log`
(it is in the data folder next to the image) and the photos asked for.

Build the card with `sh tools/wii/package.sh <target>` (no `TEST=1`: `auto.txt` is only for the tests below
that ask for it), or copy `boot.dol` over the app you already have. `args.txt` sits in the **data** folder
(next to `burnout3.iso`, not in `apps/burnout3`), one option a line.

## 1. The warping - the one that decides the clipper

1. Run the race and the place where the picture warped; photograph the screen there. This is the baseline.
2. Put `--clip 0` in `args.txt`, run the same race and place, photograph it. (The default is the clip on.)
3. If it still warps at step 2's place with the clip on: put `--clipband 0` in `args.txt` (without
   `--clip 0`), run it again, photograph it.

What the log says: with the clip on, `gx: cut to the screen per frame: N batches looked at, M past the box and
the depth scan, K drawn cut (...)` once every ten seconds. `K` is how many world batches were actually cut.

How to read it: if the warp is gone (or visibly changes) at step 2, it was the triangles' size - the guard band -
and the clipper is the fix. If the picture is the same with `--clipband 0` as with the clip off, it is not the
triangles' size, and the next suspects are the paths the clipper does not cut (cars, sky, sprites, HUD; see
"what it does not cut" in `docs/next-steps.md`).

## 2. The pattern - the BP's own interpolation

1. Put `auto.txt` in the data folder with this line, then delete it after the test:

       0 gxflags2 536870912

2. Start any race. The screen becomes two grey ramps: the top half is one triangle whose corners are far outside
   the screen, the bottom half the same ramp drawn over a screen-sized quad.
3. Photograph or capture the screen straight on, both halves in frame (a phone photo is enough; the ramps are
   smooth).

How to read it: on a rasteriser that interpolates in floats the two halves have the same slope and the same value
at every x. A step or a different slope in the top half is the Wii BP's fixed-point interpolation - the thing the
PS2's GS did not have. `work/ramp.py` measures the two halves of a frozen Dolphin frame; the same measurement can
be taken from the photo.

## 3. The DSP - the listening test

The DSP mixer is **on by default on the console** (2026-10-23; `--dsp 1` is the same, `--dsp 0` turns it off).

1. Run a race with the sound audible (engine, music, effects) and listen for clicks, dropouts, a wrong pitch or a
   channel swap; the engine and the music should sound as they do with `--dsp 0`.
2. Read the log: `sound: dsp 1: N voices a sample by the DSP (J jobs, B blocks decoded there), ... 0 runs done here
   after all, V voices taken back (W waited for their run); dsp mixer up: R runs, A answered, ...` - `0 runs done here
   after all` is the important part.
   Since stage 2 (2026-10-23) the DSP also decodes those voices' ADPCM blocks itself, reading them by DMA straight
   from sound RAM, which is in MEM2. Dolphin lets the DSP DMA from MEM2; the console has not shown it yet. If the
   DSP's voices come out as noise or silence while the rest is right, that is the first suspect (say so, with the log).
3. Put `--dsp 0` in `args.txt` and run the same race for the A/B.

How to read it: if it sounds the same, the default stays; the theoretical gain is ~0.5-1 ms a frame
(`docs/plan-60fps.md`, N1). `chunks that waited for their sums` should be 0 or near it: Dolphin's LLE shows 0-6 a 10 s
with four runs out (the real DSP is no slower). `--dsp 3` runs each voice both ways and the log says whether the
voices' state or sums differed (it does not use the DSP).
The reverb's log line `sound: the reverb's samples are converted by quantised stores and loads` (or `by C`) says what
the console's self-test chose; both give the same samples.

## 4. The frame time (optional, if the profile is wanted)

The benchmark card (`TEST=1` with `tools/wii/package.sh`) puts `auto.txt` with `0 wstat 5` and `0 benchprof 5`;
it pauses at the race start and profiles. For a plain timing, the log's `slow frame N: M vblanks` lines are the
real frame time: 2 vblanks is 30 fps, 3 is 20, 4 is 15.

## What to send back

- `run.log` from the data folder (it has the clip counters, the DSP counters and the frame lines).
- The baseline photo, the `--clip 0` photo (and the `--clipband 0` one if taken), the pattern photo.
- Whether the sound with `--dsp 1` sounded like the sound without it.
