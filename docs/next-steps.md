# Next steps: the console's frame time, and the warping

Written 2026-10-21 after the cache-coherency fixes, the picture placement and
the EFB work; brought up to date 2026-10-23 (the clipper's cost and its self-check, the DSP measured in a race).
This is the plan of record: what is done, what is next, and how each step is checked.

## Where the goal stands

| goal | state |
|---|---|
| NTFS USB, saving, controls, clean shutdown | done (the HOME/RESET/power path flushes the save and the log and leaves) |
| HUD clipping | fixed: the picture is 640x448 in a 480-line frame, as on the PS2 (`--tvfit 0` for the old stretched picture) |
| hardware-only warping | **a fix is in, untested on the console** (2026-10-22): the cache-coherency fixes did not change it; the clipper (below, "The clipper, built") cuts the world's big triangles and the ones over the near plane to the screen on the CPU, as the PS2's GS did. On by default (`--clip 0` off). The console says whether it was the cause. |
| 30 fps | **not reached**: the console runs the game at 3-4 vblanks a frame (15-20 fps). This is what "hangs in-game" looks like: the game is alive (it exits on HOME) but every frame takes 50-67 ms. |

## The one console run that decides the rest

`docs/console-test.md` is the sitting: the warping with `--clip 0` against the clip on (the one thing that
decides whether the clipper is its fix), the calibration pattern (the BP's fixed point), the DSP's listening
test, and the frame time (`slow frame N: M vblanks`). Send back `run.log` and the photos it asks for.

## Why the frame time is a memory problem

The same work takes ~24 ms in Dolphin and 50-67 ms on the console. The gap is
not instructions, it is bytes: the console's L2 sits behind a 486 MHz core, so
what matters is the traffic per frame.

Per frame the port moves roughly:

- ~4.6 MB of EFB<->RAM traffic (the picture's copies and restores) - now less,
  see below;
- ~1.2 MB of vertex and display-list data for the world;
- the texture sampling (the texture arena is 4 MB in MEM2).

Dolphin charges nothing for the EFB copies; the console does.

## The recompiler, reproducible (2026-10-23)

The game's ELF was not in the workspace, which had blocked every item that needs a new `OVERRIDES` entry. It is on the
PS2 disc (the file named `F:\Roms\Xbox\Burnout 3 - Takedown (USA).iso` is the PS2 image: volume `SLUS_21050`, the
executable `SLUS_210.50` at 4,073,816 bytes). Extracted to `work/SLUS_210.50.elf`, the generator reproduces the
checked-in `gen/` **byte for byte**:

    python -I tools/recomp work/SLUS_210.50.elf --config config/burnout3.py \
        --symbols work/symbol_addrs.txt --out gen            (74 s, 9124 functions, 28 microprograms)

(Without `--symbols` 12 files differ in comments only - the symbol names in the function banners.) So a native override
is a line in `config/burnout3.py` plus a regeneration, with only the callers of the overridden function changing.

**What still blocks E2's call-site overrides**: the generator's override ABI (`tools/recomp/emit.py`,
`OVERRIDE_SUMMARY`) says an override may read a0-a3/t0-t2/gp/sp/ra and clobbers only the GPR/FPU temps - **not the VU0
registers**. A collision/geometry override runs `ps2_vu0_callms` and does write vf/vi, so the summary needs the VU0
bits (or a proof that the game keeps no VU0 register live across a call; the PS2 ABI is caller-saved but that is not a
proof). The native `0x274C90` (the four vectors, the 0x03D8 call, the three outputs; autopilot `collc90`)
was written and **backed out unwired** until that ABI question is settled - its gain is 0.1-0.4 ms and a wrong VU0
liveness assumption would be a silent collision change, so the check mode (`collc90 2`, the same call both ways)
and the native went together.

## The console's own profile, and the exchange rate

(The numbers are in "The console's own profile (what the hardware actually spends time on)" further down.)

The exchange rate for any Dolphin measurement: **instruction-bound work costs ~2.1x its Dolphin ms** (1/IPC), a load that
misses costs 30-100 cycles where Dolphin charges one, and the GPU's work is hidden behind the CPU (which is the bottleneck).
The 60 fps arithmetic: 16.7 ms at IPC 0.5 is 12.1 M cycles, i.e. **~6 M instructions**, against ~17 M today. The item list
adds up to 8-13 ms (**~35-40 ms, 30 fps**); the structural items (S1-S3) and the memory work are what change the class.

## What was done for the console (2026-10-23)

- **The DSP mixer is on by default on a console** (`platform/wii/dsp_mix.c`), off under Dolphin (HLE does not know the
  microcode, LLE is slower than the CPU it would relieve; `plat_wii.c`'s `/dev/dolphin` branch turns it off, `--dsp 1`
  re-enables it there). `--dsp 0` turns it off anywhere. Its dry voices are what it takes; the remaining sound work is
  `mix_out`/`rev_add`/`decode_at`. **Stage 2 (2026-10-23, later): the DSP decodes those voices' ADPCM itself.** The
  CPU walks their places as before (end flags, loops, interrupts, on time) and lists the blocks entered as runs in sound
  RAM; the microcode takes them by DMA (from MEM2: unverified on a console) and decodes them as decode_at does (bit for
  bit: tools/dsp/dspmix_test.c, 103 checks in LLE). A voice's decoder is a 64-byte record in MEM1 while the DSP has it
  (dsp_hand / dsp_sync in runtime/iop_sd.c). So the CPU no longer touches a DSP voice's samples at all: no decode, no
  stream copy and no dcbf of it; what is left is its walk, a few stores of its job, and dsp_add (now quantised loads,
  five instructions a sample). In a race, "dsp 3" (both ways, compared) found 0 differences of state, records or sums in
  ~500k voice-chunks. A run takes up to 24 jobs and four runs can be out at once; the DSP's mail box holds one mail,
  so the driver keeps the others and mails each as the box empties (dspmix_poll). In Dolphin's LLE race ("dsp 1",
  work/auto_dsp1.txt): every run answered, 0 done here after all, 0-1 voices a 10 s with no room in a run (59-326 with
  stage 2's first 20 jobs) and 0-6 chunks a 10 s that waited for their sums, 4.4 ms in all at the most (130-289 with
  two runs out).
- **The wet voices stay on the CPU, and why**: the DSP's data memory is exactly full (nibble table 0x000, job fields
  0x010, command block 0x020-0x1AF, variables, scale and filter tables, run list 0x1B0-0x1FF, resampled voice 0x200,
  decoded stream 0x300-0x744, blocks as DMA brings them 0x750, record 0x7A0, the two cores' sums 0x800-0xFFF), so a second
  (wet) sum per core has nowhere to go; a dry+wet voice would also need two gain pairs in one job, and the 24-job block
  (16 words each) would grow past 0x1FF. The wet voices are 2.3 of 8.7 heard (the stretch of all of them is 0.7 %), so
  the gain would be ~0.1 ms against a microcode redesign - not the free win it looks like. (The 0.7 % rests on the
  profile's mix_out/voice_stretch bucket mix-up, see "The console's own profile": the stretch is ~3 % there, so the
  wet voices' share is nearer 1 % - still behind the decode and the CPU-side costs of the dry voices.)
- **The ADPCM decode** of the DSP's voices: done (stage 2, above). decode_at is left for the wet voices and for
  warm_up, which begins a decoder again after a voice was unheard.
- **The reverb a chunk at a time** (2026-10-24, `runtime/iop_rev.h`): `rev_add` ran `rev_step` (the SPU2 reverb; hall
  on core 1 in a race) a sample at a time, and each sample worked out its fourteen places in the work area again (an
  offset taken round the delay line's end and a choice of side each: ~290 instructions a sample, the profile's
  `rev_add` 2.1 %). `rev_run` keeps the places as pointers that move one sample a pair, between the points where one of
  them comes round the end, with the loads and stores in rev_step's order (hall's two sides have places 4 samples
  apart); the input and output are converted by quantised stores and loads (a startup self-test against the C
  conversions picks them). The same bits: tools/iop/revtest.c on the host (~108 M samples of random states: every
  wrap, equal places, edge volumes and inputs) and "rev 2" in a race (12,914 chunks, 0 differences). Dolphin's count:
  2.63 % -> 1.55 % of the time; it charges a mullw 5 cycles and both ways have the same 17 a sample, so the rest went
  from ~315 to ~150 cycles. "rev 0" is the old way, for an A/B.
- **`dcbt` prefetch where the console profile says the misses are**: the texture source check's 16 scattered samples are
  touched before they are read and its full re-reads' hash loop two lines ahead (`ext_source_ident`: 2.7 % = 1.3 ms,
  every sample a line; the re-reads are 89 KB a frame), and the copy loop's source is touched two lines ahead
  (`memcpy`: 10.1 %, `dma_spr` half of it - in its dcbz path only, so under Dolphin, where that path is not taken,
  the copy loop is untouched). All are hints: no result changes, and only the console can say how much they win.
- **Not changed**: the 8-tap speed blur (GPU-side, overlapped; 6 taps would change the picture for ~0.2 ms of GPU time),
  the box-on-screen clipper skip (1-2 batches a frame, dropped), the EFB copies (the CPU only pays their sync, 0.8 %).

## The warping: what is left, and how to tell the two candidates apart

The console run of 2026-10-21 says the warping is **still there** after the
cache-coherency fixes, so the stale display lists and the stale texture cache
were not its cause. Two candidates are left, and they look different on the
screen:

1. **The near plane (B1).** The PS2's world microprograms clip a triangle
   that crosses the near plane themselves and reverse its winding, so the
   visible piece is drawn. GX clips it and keeps the winding, so the
   rasteriser's culling decision for that triangle is the opposite of the
   PS2's: the piece of road under the car is culled instead of drawn. The
   port's `native_cull()` takes a `behind` argument, works it out with
   `behind_camera()` and then throws it away (`(void)behind;`) - a hole in
   the code, and the document says so.
   Measured: **165 batches and 1194 triangles a frame** cross the near plane.
   The effect is a *missing piece* of the picture, with a straight edge where
   the near plane cuts it.
2. **The guard band (B2).** A triangle that reaches far outside the screen
   is clipped by the PS2's GS and by GX at different places, and the Wii's
   GPU interpolates the texture over the whole triangle with a reciprocal of
   limited precision - the more of the screen a triangle covers, the more the
   texture is stretched compared with the PS2's.
   Measured: **4406 triangles a frame** reach more than half a screen width
   outside the screen, 1729 of them several screen widths out. The effect is
   a *stretched or smeared texture* over the road, worst where a triangle is
   largest (the road near the camera, the ground plane).

The picture decides which one it is, and no further guessing is worth much:
a straight-edged hole at the bottom of the screen is (1), a smeared texture is
(2). A photo of the warping on the console settles it.

**But the near plane is weak.** The diagnostic that leaves out the crossing
batches (gxflags2 16777216) makes a hole exactly where the road under and
around the car is - and the normal picture draws that road, in Dolphin, whose
GX is an accurate emulator. So the near plane is not dropping anything, and
the cull sense that the inverted diagnostic changes (9% of the picture, the
road's surface) is the right one already.

**Which leaves the vertex arrays.** `dl_touch()` writes the game's vertex
arrays out to RAM only when a list is new, when it comes back after frames
away, or every 16th frame. On the console the GPU reads those arrays from
RAM, never from the CPU's cache: if the game rewrites vertices in place (the
billboards, the signs), the GPU can read vertices up to 15 frames old. In
Dolphin the memory is coherent, so it never shows - and the "do the arrays
change in place?" check that says they do not was run in Dolphin, where it
cannot see the effect.

**The console can test that today, with the build it has:**

```text
0 gxflags2 256
```

"write the lists' arrays out every frame" - the port has the switch. If the
warping changes, that is it, and the fix is to do it by default (84 lists a
frame, cheap).

The other vertex item, `GX_InvVtxCache()` before every indexed draw, needs
a code change.

A diagnostic build for (1) is in the works: it leaves out the batches that
cross the near plane, so the piece of road under the car is simply not drawn.
If the warping is where the hole appears, it is (1). It is `gxflags2 16777216`
(the picture it makes in a race: the whole road under and around the car is
gone - the crossing batches cover that much).

A second one, `gxflags2 33554432`, draws the crossing batches with the
*other* cull sense, which is what the PS2's microprogram does when it clips
such a triangle and reverses its winding. That is close to a fix (the
fully-in-front triangles of those batches would be culled instead, a small
hole), and it shows on the console what the near-plane case would look like.

The probe the console build already has (`gxflags2 8388608`) counts the same
two things per frame, so putting

```text
0 gxflags2 8388608
```

in the auto.txt of the build that is on the console now says whether the
near plane and the guard band are even busy on the console (the numbers are
Dolphin's: 165 batches and 1194 triangles over the near plane, 4406
triangles far outside the screen).

## The clip probe's depth had the wrong sign (found 2026-10-22)

`gxflags2 8388608` computed W as minus the camera's fourth row (the port's own, for the clipper, has the third
GX row, which is already minus: W = -(row 3 of m)). The PC host's reference (`runtime/gs_soft.c`
`worldcmp_batch`, checked against VU1) has W = +(1014..1017).w applied to the position, in front of the eye
positive. So the probe's "behind the eye" were the vertices in front, and its "far outside the screen" counts
(4406 a frame, 1729 very far) were measured on the mirror image behind the camera. The near-plane crossings
(165 batches, 1194 triangles) were mixed-sign counts and survive the sign only in kind, not in number.

Measured again with the sign right (the scripted race at Silver Lake, the car at the start):

```text
51-64 batches with a triangle over the near plane, 250-260 such triangles;
720-1050 triangles reach more than a screen's half width outside the screen
(180-400 of them very far), in about 100 batches
```

A quarter of what the design assumed. The probe measures in the GS scissor rectangle's screen halves now.

## The clipper, built (2026-10-22)

`platform/wii/clip.h` (host-tested by `tools/clip/cliptest.c`) and `clip_batch` in `gs_gx.c`. It replaced the
screen-space clipper with the ray solve (a design that needed a 3x3 solve per cut vertex and left the triangles
that cross the near plane to the GPU - the very ones close to the camera) by a clip in the world's space against
the five planes through the eye: the near plane and the four sides of the screen. X, Y and W are affine in the
world position, so a cut vertex is the linear interpolation of the edge's two (position, X, Y, W, and the texture
coordinate, which is an affine map of the triangle's plane); only the colours are taken at the screen's
parameter, because the GPU interpolates them linearly in the screen.

What the host test measures (40000 triangles, camera at 3 km, triangles up to 2.5 km, some across the near plane,
against a double precision reference): the polygon covers the screen points the triangle does (231000 points,
0 differ), the texture coordinate of every vertex is the original's to 0.2% of a 64-unit range at the worst, W to
0.02%, the colours to 7% at the worst in a few sliver triangles. It also found that the old core's cap of five
vertices per polygon cut the sixth and seventh off (a big triangle across three corners has seven).

How it runs in the port:

- the game flags the batches that reach the edge of the view (VU1 1013.x): only those are looked at;
- a batch is drawn cut only if a triangle of it crosses the near plane or reaches more than `--clipband` percent
  (default 200) of the screen's size outside it; then its strips go out as the list has them (without the cut
  and the left-out triangles) and the cut ones as fans from arrays of their own; else its list is called, as
  before;
- which triangles are looked at is cut down: the box (wholly behind the eye: nothing), the 24 biggest triangles
  of each list (kept in the list; only a long one reaches from the screen into the band), the vertices' depths
  for a box that reaches the near plane, and a batch with nothing to cut is looked at every `--clipevery`
  (default 4) frames only. The cost, in Dolphin's count: 0.4 ms a frame on the start grid, 1-1.5 ms in the
  scripted race (159-382 triangles cut a frame in 21-42 batches), 5 ms in the race's intro cameras (where the
  camera is inside the geometry and half the batches reach the near plane);
- the look's own work was cut on 2026-10-23 (the same cuts, the same triangles): the depth scan computes **W
  alone** (the X and Y it used to build were dead), `cl_tri_bad` is inlined (as a call it spilled and restored
  twenty registers a candidate), and the per-vertex class pass is **skipped unless a vertex is behind the eye** -
  most batches in a frame have none, and a batch's classes are now built only when a triangle is actually cut.
  The race start's own status line went from 380 to 355 us at the same moment of the scene; the benchmark's p50
  went 26.14 -> 25.61 ms with `-O3` on the hot functions in the same build (`docs/plan-60fps.md`);
- that bookkeeping was then **checked against the old one** (a temporary `CLIP_CHECK` build recomputes the old
  classes and both lookups for every batch looked at and compares them). It found one real difference, in 436 of
  10,387 batches of the race start: a triangle cut by the *look* in a batch that also crosses the near plane kept
  its `NORMAL` class, so the strips drew it as well as its clipped fan - the fan was drawn last and covered it,
  so the picture looked right and the GPU paid for both. Fixed (a cut triangle leaves the strips too), and
  rechecked: **46,008 batches** looked at in the race start (19,152 + 10,387 + 16,469 in three status windows),
  **0 differed**. A batch that is cut is now drawn exactly as the old code drew it;
- one more early-out was measured and dropped (2026-10-23): a batch whose eight box corners are all in front
  and on the screen has no triangle that can reach the band, so it could skip the vertex scan. In the race
  start it skipped **1-2 of the 30-146** looked-at batches a frame (the world's near boxes span the screen), so
  the X/Y the corner test costs every batch is not paid back; the code is not there.
- checked in Dolphin (2026-10-22/23): the picture is the same. With `--clipband 0` (every triangle that leaves
  the screen cut) and the clip off, the race start at tick 700 (46-47 mph: the runs' freeze lands one tick
  apart): the two frames are the same scene, the sky and the HUD identical, and the difference image is the
  edge outlines of the camera's one-tick move - nothing missing at the screen's sides, the bottom (the near
  road) or where the near plane cuts. HLE does not show the warping, so only the console can say whether it is
  gone.

`gx: cut to the screen per frame:` in the status block says what it did. Autopilot `clip`, `clipband`,
`clipevery`; args.txt `--clip`, `--clipband`, `--clipevery`.

What it does **not** cut: the world batches the native list path does not draw at all (> 220 vertices, `dl_get`
returns NULL; the VU1/GS path draws those, the emulated PS2 pipeline, which clips as the GS did), the cars
(`gs_backend_car`), the sky, the sprites and the HUD. The picture the user saw warp on the console is the world's
(the road and the scenery); a warp that survives with the clip on may be in one of these paths instead, and the
status line's "batches looked at", "past the box and the depth scan" and "drawn cut" say how many of the looked
ones are cut.

The console test: the same race with `--clip 0` and without, a photo of the warped place. If it is still there
with the clip on, it is not the triangles' size; `--clipband 0` (cut everything that leaves the screen) is the
strongest the clipper gets. `docs/console-test.md` has the whole sitting (this, the pattern, the DSP and what to
send back).

## The vertex cache: audited, and mostly closed (2026-10-21)

- The GPU's vertex cache is dropped **once per frame**, at the end of every
  frame (`GX_InvVtxCache()` in the frame's end, just before the present).
  That has been there since the first commit, so the console's build has it
  too.
- Every path that draws indexed arrays flushes the memory the CP reads: the
  port's own arrays (`dl_touch`, the car lists, the strip path, the sky's
  list) and the guest's arrays in the lit path (`lit_draw`).
- `lit_draw` draws the guest's memory directly and does not invalidate the
  cache itself; the per-frame invalidation covers it.

So "the console draws vertices 15 frames old" is weaker than it looked: the
arrays the port writes are flushed when they are written, and the cache is
dropped every frame anyway. `0 gxflags2 256` on the console is still worth
running (two lines, and it also proves the flush cost), but it is no longer
the favourite.

That leaves the guard band and the interpolation precision as the favourite,
which is exactly what the Software Renderer can test here.

## The warping: the drawing-area clip (2026-10-21, sharpened)

What the code says, not what we assumed:

- `native_cull` (L4246) reproduces the PS2's *winding* convention and does no
  clipping at all (`(void)behind;`) - the port never clips the geometry.
- The port maps the GS's **scissor** (a pixel mask) to `GX_SetScissor` (L2475)
  but the GS register list has **no DISPENV** (`runtime/gs.h`): the PS2's GS
  clipped every triangle to its **drawing area** (the screen), and nothing here
  reproduces that.
- The Wii's BP clips to its **guard band**, which is far bigger than the screen,
  and interpolates the texture coordinates in fixed point over the clipped
  triangle. The port's own note (L4741) says exactly this: "a triangle that
  reaches far outside the screen is clipped by the PS2's GS and by GX, but at
  different places, and the edge's interpolation differs" - 4406 triangles a
  frame reach far outside the screen.
- Dolphin hides it: its HLE GPU interpolates in floats. So the picture is right
  in Dolphin and wrong on the console, and the road under the camera (the
  biggest screen-space triangles) is where it shows.

The near plane is out (the console test with the winding flip, `gxflags2
33554432`, did not change the picture). The caches are out (audited). The EFB
noise is out (fixed).

So the fix is to reproduce the PS2's clip on the CPU: clip the world's
triangles to the drawing area **in clip space** (so the depth stays exact), and
in the same pass the near-plane clip with the winding flip the PS2's
microprogram did (1194 triangles a frame).

**The cost, and the way around it.** The clip depends on the camera, so the
world's vertex arrays and its display lists would have to be rebuilt every
frame, and the CP compiles the display lists. The way around it: build each
batch's list **once**, with a fixed layout - every input triangle gets five
output slots, the unused ones degenerate - and rewrite only the vertex arrays
each frame. The list's indices do not change, so the CP's compilation stays out
of the frame.

**How we measure it.** Not with the game's race frame: the Software Renderer
runs the race at ~0.4 fps (tick 500 = 5664 frames = 4 hours), so it is a tool
for synthetic scenes, not for the game. Instead a **calibration pattern**: a few
big triangles with a known UV gradient, one of them reaching far outside the
screen, drawn by

- (a) Dolphin's HLE - the float reference,
- (b) Dolphin's Software Renderer - the fixed-point reference,
- (c) the console - the truth.

(b) vs (a) is the hardware's arithmetic error; the console must match (b); with
the clipper on, all three must agree. The same pattern in the game gives the
console's own numbers, so the fix can be tuned without a console in the loop.

**The cheapest thing that needs nothing new:** a screenshot of the race taken on
the console (the console's own screenshot function, saved to the SD card),
diffed against the HLE frame at the same tick. The shape of the difference says
which cause it is: a shear along the road is the guard band, a hole is the
near plane, geometry in the wrong place is the vertex cache, wrong texels are
the texture cache.

## The calibration pattern (gxflags2 536870912)

Drawn into the display target at the end of every frame: two triangles that
reach far outside the screen, with a colour ramp across it.  The BP's
interpolation of the ramp is the guard band's error, and it is what the
Software Renderer and the HLE disagree about (if the Software Renderer models
the BP's fixed point, which is the question the pattern answers).

The things that cost time on it, worth writing down (2026-10-23):

- The vertex descriptor needs the normal as well: with `GX_VTXFMT0` and the
  descriptor missing `GX_VA_NRM` the port faults, and Dolphin's watchdog
  reports it as a hang ("no emulated vblank for 3 s", the main thread in
  libogc's exception handler).
- `ortho_mode()` returns early when no native draw is active, so the pattern
  was drawn with whatever descriptors the frame's last draw left. The pattern
  sets `g_native_mode = -1` before it now, and sets the colour channel to
  `GX_SRC_VTX` itself.
- The colour comes from the port's untextured Tev recipe: the rasterised colour
  in the **fourth** input with `GX_TEV_ADD`.
  `GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE)` puts the *texture's* colour in the
  first input, and with no texture bound that is black - and so was the whole
  pattern (and `efb_fill_test`, which uses the same call).
- `calib.rgba` (the EFB read back with `GX_CopyTex`) is **black for every
  draw**, even a full-screen quad filled from the Tev's constant colour, so
  that dump is broken; the measurement uses the frozen display frame
  (`work/frz.sh`, `work/frz_sw.sh`) instead. There, the pattern is right: HLE
  shows a ramp from Y 75 at x=0 to 111 at x=639, the same in the huge
  triangle's half and the screen-sized quad's, column for column.
- The earlier "the channel has to be `GX_ENABLE`" note was read off the broken
  dump; with the frozen frame both channel settings draw.
- **The Software Renderer is not the fixed-point reference it was taken for.**
  Dolphin's VideoSoftware died at 2.8 s of emulated time (frame ~140, before the
  tick-100 freeze at frame 181) in this build, silently, as it did at 54
  minutes in the earlier run (the GFX FIFO's "Unknown Opcode 0x43"). And its
  rasteriser interpolates the attributes in **float**, like the HLE, so even a
  run that reached the frame would not show the BP's arithmetic error. The
  pattern's fixed-point reference is therefore **the console itself**: draw it
  (`gxflags2 536870912`), take a screenshot of the frame, and compare the two
  halves' ramps - the console's BP is the only fixed-point rasteriser in reach.
  `work/ramp.py` reads a frame's two halves (per column, slope, worst deviation
  from a straight line); the HLE's is 75 at x=0 to 111 at x=639 in both, worst
  deviation 1.97 (8-bit rounding).

## The clipper, designed (2026-10-21, no console)

The console is not available, so everything has to be settled on the PC:

- Dolphin's **Software Renderer** was meant to be the Wii's BP in software, but
  it is not an oracle: it died at 2.8 s of emulated time in this build (GFX
  FIFO, the `frz_sw.sh` run; the earlier one after 54 minutes, "Unknown Opcode
  0x43"), and its rasteriser interpolates in float like the HLE, so it would
  not show the BP's fixed-point arithmetic even if it ran (see "The
  calibration pattern" above). The local references that do exist are the
  HLE's picture and `tools/clip/cliptest.c`'s exact arithmetic; the console is
  the only fixed-point reference.
- The **HLE** frame at the same tick is the reference for what the picture
  should be. `work/warpmap.py` compares the two (blurred, because the two
  renderers dither differently) and writes the difference map and a side by
  side.

The clipper's design (the exact fix for the guard band):

- The clip is done against the **five planes** of the frustum (the four sides
  of the target and the near plane W=0), in the world space, where every
  intersection is a line against a plane - no ray, no solve.
- The vertices that cross the near plane are **left to the BP** for now: their
  screen position is undefined at W=0, and the winding the PS2's microprogram
  gave them is a separate difference (B1, the near-plane cull).
- The clipped vertices keep the **world position**, so the depth stays right,
  and the texture coordinates are interpolated the way the GPU interpolates
  them (perspective-correct, with the barycentrics of the screen position):
  the picture is then the same as the unclipped one's, and only the triangle's
  extent shrinks to the screen. That is the whole point: the BP's fixed-point
  interpolation over a screen-sized triangle is the PS2's.
- The list of a clipped batch is made **once**, with a fixed layout - five
  slots and three triangles for every triangle of the batch, the ones not
  needed degenerate - so the CP compiles it once and only the vertex arrays
  are written every frame. The arrays are the port's own memory, in MEM2, and
  they are written out (and the vertex cache invalidated) like every other
  array the GPU reads.
- The cost: ~5000 triangles a frame, about 1-2 ms on the console, and it
  removes the off-screen parts of the geometry, so the GPU has less to do.

The check that it works: the HLE picture must not change (the clip only
removes what is off the screen), and under the Software Renderer the warp
must be gone.

## A reference renderer on the PC: Dolphin's Software Renderer

`Dolphin.exe -v "Software Renderer"` (it is in the installed build; the
command line is the only way to pick it - `GFX.Main.Renderer` in `GFX.ini`
does nothing). It is a CPU implementation of the Wii's rasteriser (the real
BP/XF pipeline) instead of Dolphin's HLE's float shaders, so it models the
GPU's *arithmetic*: the guard band clipping, the near plane, the winding and
the rasteriser's fixed-point rules.

It is far too slow to play, but it is fast enough to be a **reference**: one
frame at one tick. `work/frz_sw.sh` is `work/frz.sh` with that switch, and
the frames land in `work/frames/frz_sw*.bin` like the others.

What it decides: the *GPU-side* candidates - the guard band (4406 triangles
a frame reach outside the screen), the near-plane clipping and winding, the
interpolation precision. What it does not decide: the *memory-side* ones -
the stale vertex arrays and the CPU's cache against the GPU's reads - because
it reads the guest memory coherently, exactly like Dolphin's HLE does.

The texture cache is in between: Dolphin's HLE models it (Graphics -> Hacks
-> Texture Cache Accuracy: "Fast" ignores the game's invalidations, so an A/B
with it approximates the stale-texture hypothesis), the Software Renderer
most likely does not.

The decompals "Dolphin LLE" GPU (a separate Dolphin build that JITs the Wii
GPU's own ARM code) is the stronger version of the same idea - the real
clipper, guard band and interpolation. Its project is no longer in the
decompals organisation, so it would have to be found before it can be used.

## The loading screens' noise (fixed 2026-10-21)

"Graphical corruption and glitches, mostly on game load, outside the load
image."

The console's EFB is **not zeroed at the start** and the game clears only
what it draws into (the load image's rectangle), so the area around it showed
whatever the EFB held: noise on the console, black in Dolphin (whose EFB
starts zeroed) - which is why it was never seen here.

Fixed: a big target that is bound with nothing to restore (the first time, or
after the region was given away) has its region cleared - the colour and the
depth with it.

`gxflags2 134217728` fills the EFB with a colour at the start, so the
difference can be seen in Dolphin: with it on, the loading screen's
background is black and the race's picture is the same (512 pixels of the
frame's last line apart).

## The console's own profile (what the hardware actually spends time on)

`E:\burnout3\prof.bin` (50,008 samples), mapped with the ELF of the build that
took it (`work/console7/burnout3.elf` - the current ELF gives different names
for the same code):

```text
10.11%  memcpy            7.44%  nc_shade_ps_pairs   2.97%  pc_cached
 2.68%  ext_source_ident  2.57%  mix_out             2.10%  rev_add
 2.00%  draw_setup_slow   1.99%  nw_lit_ps_quads     1.72%  gs_backend_car
 1.50%  decode_at         1.39%  vif_feed_words      1.37%  dl_get
 1.34%  gif_path_bulk     1.16%  dma_run             0.88%  tex_rest
 0.78%  vu0_vis           0.70%  put_row             0.67%  placed_shape
```

- **memcpy 10.1%**, and the stacks say where: `dma_spr` (the scratchpad DMA)
  4.8%, the VIF's `unpack_bulk` 1.5%, the disc's `lazy_data` 1.4%, the VIF's
  `vif_mpg_flush` 0.7%. So the scratchpad's copies alone are about 2.4 ms a
  frame on the console. The scratchpad is guest RAM (MEM2); the PS2 had it in
  1T-SRAM. Moving it needs the recompiled code's own accesses to move with
  it, and those use computed addresses (the recompiler would have to
  recognise the 0x7xxxxxxx segment) - the runtime's DMA path alone is not
  enough, the two must be the same memory.
- **the audio** (mix_out, rev_add, decode_at, voice_stretch) is about 6.9%,
  3.5 ms a frame. The plan's P4 (the mixer on the Wii's DSP) would take it off
  the CPU; it is not bit-exact, so it needs a listening test.
  *Correction (2026-10-23):* the "mix_out 2.57%" is a bucket mix-up: wprof sums whole 64-byte buckets, and the
  bucket at 80060400 holds voice_stretch's loop and only mix_out's first 16 bytes. By buckets: voice_stretch ~3.0%,
  mix_out ~0.3%, rev_add 2.1%, decode_at 1.5%. (The profile is the cache-model's, not a console's; by the hardware's
  arithmetic the stretch is ~14 cycles a voice-sample in L1, ~1.5% of the CPU at 16 voices heard, and a block's
  decode ~300 cycles plus a MEM2 miss.) So the stretch is the biggest piece of the sound, and the DSP's dry voices
  take most of it.
- **the car's shading** 7.4% (hand-written PowerPC, two vertices a register):
  the only way further is GPU lighting, which is not bit-exact either.
- **the texture path** (ext_source_ident, tex_rest, put_row, placed_shape,
  gs_load_clut) is about 5%: the port re-reads the placed textures' sources
  every frame (96 KB) and hashes them to notice a change that has never
  happened ("0 found changed so far"). The sources live in the GS's memory, so
  the GS's own write versions could answer "has it changed" exactly, without
  the re-read.
- **`pc_cached` 3%**: the render-packet validation (the plan's S1c: a
  generation instead of a check, if an audit shows the render data is written
  once).

The profile covers the whole run, so the ISO load (10%) and the menus are in
it. A race-only profile needs an `auto.txt` with one line:

```text
300 prof 20
```

(the autopilot's `prof S` starts a sampling profile of S seconds at the game
tick 300, with the counters reset - `work/wprof.py` maps it). Then
`prof.bin` and `profstack.bin` from the data folder are worth more than
anything the guesswork above produced.

### What the console's profile says it is (re-read 2026-10-22)

From `work/console7` (the benchmark run, `pace 0`, one update pass a frame, 50 ms a frame at the race start):

```text
race frames: IPC 0.46-0.49; instruction fetch miss cycles 13.9% of the cycles, L1 load-miss cycles 27.5%
             (41% of all the cycles wait for memory); 277000 L1 instruction misses, 140000 L1 data misses and 90000
             L2 data misses a frame
inclusive:   the frame's DMA chain 50% (the VIF/GIF/GS layer: cars 15%, world batches 14%, microprogram starts 14%,
             texture/GIF paths 14%), the game's own render build 26% (the walker), sound 8.6%, the game's update
             8.5% (so an extra update pass costs only 4 ms)
```

So the game's logic is not the problem; the drawing layer's code and data footprint is: the per-batch path touches
hundreds of lines that are cold (the whole frame is 2-2.5 times Dolphin's count, more in the render path than in
the game's code). It is flat: no function above 10% but memcpy (10%: the scratchpad DMA's copies, the VIF's
UNPACKs; every cold line a miss). What would decide the next step is the same numbers by part: `ptstat 1` on the
console now also prints `pmc by part [...]` (IPC and the misses per 1000 cycles of EE, VIF, GIF, GX, NAT ...), and
the boot log has `membench` lines (a dependent load and a line in order, in MEM1 and in MEM2, and memcpy's speed).

## What has been done about it

A race frame made **two copies and two restores** of the 640x448 picture
(about 4.6 MB between the EFB and RAM per frame, plus two full-screen quads).
Dolphin charges nothing for that; the console's copy engine and RAM do.

- **Neither big target is restored any more.** The restores put a target's
  last content back into the EFB region, and for the two big targets nothing
  can see it: the world covers the scene, and the game's final blit covers
  the display opaquely (its alpha register and its vertex colour make it a
  copy of the source's colour) before anything is drawn into either. The
  status line now says "2 target copies, 0 restores", where it said two of
  each. The single-player frozen frame at tick 650 is the same picture (2,685
  pixels - the HUD emblem's animation phase, the same as between two runs of
  one build), and the split-screen at tick 1100 is unchanged in structure.
  `gxflags 16777216` brings the restores back.
- **The display buffer's picture is not copied out either.** A counter in
  `rt_for_texture` showed the picture is never read as a texture, in a race
  or in split-screen, so it does not have to be saved when the display buffer
  gives the EFB region to the scene; the region is not cleared either (the
  game clears the scene, and that covers the same rows).
- The Wii's EFB cannot hold two 640x448 regions (a test build with the big
  target at row 512 broke the picture completely - the depth buffer wraps at
  512 lines), so the PS2's arrangement cannot be copied over; the copies are
  the price of having one EFB region for both.

What is left of the picture's traffic: one copy (the scene's - the blit
samples its texture, so it has to be in RAM) and the final copy to the XFB.


## What is next, in this order

1. **The world sections as replayed display lists** (the plan's P1, the design
   in `docs/design-s1.md`). This is the only item big enough to matter, and it
   attacks both currencies: it removes the per-frame VIF walk (99k words a
   frame), the parse of 396 world packets and the per-batch backend work for
   492 batches - and with it the traffic of re-walking the chain. The careful
   sizing in `docs/design-s1.md` says 0.7-1 ms in Dolphin's count, more on the
   console, because the work it removes is per-batch and memory-bound.
   A group's identity: a section's batches of one texture and one state.

2. **The console's memory layout** (the plan's P6, console-only, invisible in
   Dolphin):
   - the guest scratchpad in MEM1 or in the locked L1 cache. `dma_spr` is a
     third of the profile's `memcpy` time (11% of the frame) and the scratchpad
     is 16 KB - exactly the locked cache's size. The DMA path is in the
     runtime, so it can be moved; the recompiled code's own scratchpad
     accesses use computed addresses, so those need the recompiler to
     recognise the segment.
   - MEM1 is nearly full (about 2 MB free), so moving the hot guest data
     there needs the recompiled code to shrink first.

3. **The exact native clipper** - only if the warping is still there after the
   cache fix. Measured: 165 batches and 1194 triangles a frame cross the near
   plane, 4406 reach far outside the screen. It needs per-frame vertex arrays
   and per-frame display lists, and the PS2's exact interpolation rules.

4. **The car's shading and the audio** (7.4% and 6.9%): both are already
   careful code (hand-written PowerPC, two vertices a register; a fixed-point
   mixer). The only way further is to move them to the hardware - the car's
   lighting to the GPU, the mixer to the Wii's DSP - and neither is
   bit-exact, so both need a comparison (a picture for the car, listening for
   the sound).

5. Not the IOP, after all: with the ELF of the build that took the profile it
   is 1.0% of the frame (the 3.4% in an earlier reading was a misattributed
   bucket). The calls into the IOP are not worth a look.

5. **The car shading** (about 9% of the frame): already hand-written
   PowerPC with two vertices a register. The only way further is to move the
   lighting to the GPU, which is not bit-exact.

## Things that were tried and did not work (do not repeat them)

- The global symmetric viewport: it breaks every user of the native
  projection, car select included.
- A CPU clipper drawn with `GX_Begin`/`GX_Flush` in the VIF backend: it
  desynced the GPU every time.
- `gxflags 128` (the whole world through the emulated microprogram): far too
  slow (578 of 578 batches emulated).
- The whole-batch `GX_CULL_NONE` for near-plane batches: it changed 80,367
  pixels - buildings and sky, not the road edge.
- The edge-only VU1 fallback: 20-35% of the world's batches have a triangle
  over the near plane, so it is far too slow.
- `dcbt` prefetch in `memcpy`: the profile says memcpy is slower with it.

## How each step is checked

- The frozen frame at a game tick (`work/frz.sh`) is the authority; the PNG is
  only the window. Two runs of one build differ by up to ~3,000 pixels in the
  HUD emblem's area (its animation phase), so a difference inside that box is
  not a difference in the rendering.
- The split-screen frames are **not** comparable between runs (the two cars are
  at different places), so a split-screen change has to be judged by looking
  for artifacts, not by a pixel count.
- The console is the only place where the frame time and the caches count.
