# Bug cases and the next real performance work

Written 2026-10-20 after the first hardware profile. This is an investigation,
not a claim that a fix is already in the build.

## What was measured

The profiling run did complete. `E:\burnout3\prof.bin` (68,812 bytes),
`E:\burnout3\profstack.bin` (246,208) and the new `run.log` contain:

```text
profile: 50008 samples (0 outside MEM1) -> prof.bin
```

The log's timestamp was stale because libntfs does not update the directory
timestamp of a truncated file. The content was new; the logger now deletes the
old file before opening it.

The run was also in benchmark mode (`pace 0`, `wstat 5`, `benchprof 5`,
`ptstat 1`). Its p50 work was 48.2 ms and its mean 58.7 ms; that is **not**
normal-play performance. The profiler itself and the benchmark change the
timing. `auto.txt` has been removed, and normal adaptive play is restored.

The profile's largest self times:

| % | function | what |
|---:|---|---|
| 10.11 | `memcpy` | copies (scratchpad DMA, VIF UNPACK, display lists, texture rows) |
| 7.44 | `nc_shade_ps_pairs` | native car vertex lighting |
| 2.97 | `pc_cached` | render-packet validation |
| 2.68 | `ext_source_ident` | texture-source identity (16 sampled words) |
| 2.57 | `mix_out` | audio mix |
| 2.10 | `rev_add` | audio reverb |
| 2.00 | `draw_setup_slow` | GS state application |
| 1.99 | `nw_lit_ps_quads` | native lit-world shading |
| 1.72 | `gs_backend_car` | car batches |
| 1.50 | `decode_at` | audio decode |
| 1.39 | `vif_feed_words` | VIF commands |
| 1.37 | `dl_get` | display-list lookup/build |
| 1.34 | `gif_path_bulk` | GIF register/data path |
| 1.16 | `dma_run` | DMA channels |

Time split, in the profiled frame:

```text
EE 48%  VU0 0%  VU1 2%  VIF 12%  GIF 10%  upload 1%  sync 1%
prim 2%  flush 0%  tex 1%  gx 11%  idle 0%  kick 1%  native 12%
```

The profiler's stack categories put native code at roughly 72% and the
recompiled game code at 21%. The "EE 48%" label is the EE thread, which
includes native work called from it; it is not 48% recompiled game logic.
**`sync` is only 1%.** The two full-screen EFB copies are not the main cost;
removing the final display blend cannot be the route to 30 fps.

## Numerical error at the camera edge

This is plausible for triangles near the near plane, but not for every
bottom-of-screen edge. `work/precisionsim.py` models the port's world/camera
float path with a camera translation of 5000 world units (the order of Silver
Lake) and a point near the right edge. Float32 versus float64 screen X:

| W | float32 screen error |
|---:|---:|
| 1.0 | 0.03 px |
| 0.1 | 0.50 px |
| 0.03 | 1.54 px |
| 0.01 | 6.75 px |
| 0.003 | 24.50 px |

The error grows as `1/W`: the translation is already rounded in float, and the
perspective divide magnifies it. A triangle that crosses `W = 0` can therefore
have a visibly bent edge even if the rasteriser's clipping is working.

The native world path does not clip on the CPU. It leaves the polygon to GX;
the PS2 VU1 microprogram clips in VU1. That difference exposes GX's guard band,
clip precision, and perspective-correct interpolation to much larger
homogeneous coordinates. The 4.12 UVs (12 fractional bits) and the S16 texture
coordinates add their own quantisation at a clipped edge. This is the right
mechanism for warping/skew that appears only when geometry runs into the near
plane or the side of the screen.

It is **not** a good explanation for a road quad that is wholly in front of the
camera and merely half off the bottom of the screen: there `W` is not small,
the winding is stable, and float error is sub-pixel. For that case look at the
scissor/target offset, the 448-to-480 copy, the VI/16:9 path, and the texture
sampling.

A precise test is to log the minimum `W` of each world batch in the bad frame.
If the affected batch has `W < 0.05`, the numerical/clipping explanation is
live. The fix is then a per-triangle near/screen clip or split **before** GX,
with position/UV/colour interpolation, not a whole-batch cull override (that
was tested and changed buildings/sky).

## W probe result (2026-10-20)

A temporary probe (`gxflags 2097152`) logged 2,000 world batches over the
start/race frames 5810-5812:

```text
489 batches with a vertex behind the eye (W < 0)
  1 batch with minW in (0, 0.5): 0.314575, max screen 14387 x 7307
  0 batches with minW in (0, 0.1)
  all other positive minW values were 4 or more
```

At W = 0.314 the modeled float32 translation error is about 0.16 px, so pure
float divide precision is **not** the explanation at the start. The one
interesting batch has a screen coordinate of 14,387 with W = 0.314: that is the
GX guard-band/clip case, not the divide-rounding case. The 489 behind-eye
batches are also where GX clips and where the PS2 VU1 clipper would have done
the work first.

So the edge-warping suspect is now **GX's guard band and clip/interpolation of
huge screen-space triangles**, with CPU clipping/splitting as the exact fix.
Float translation precision becomes important only if a visible vertex really
has W below about 0.05; the start frames did not show one.

## The bug: separating the cases

The important distinction is whether a triangle is merely outside the screen
on the bottom/side, or whether it crosses the camera's near plane (`W <= 0`).

`work/clipsim.py` models the transform used by `native_projection`: `X=x`,
`Y=-y`, `W=-z`, screen `(X/W*320, Y/W*224)`. It gives:

| case | raw screen winding | what GX does |
|---|---|---|
| road quad below the bottom edge, all vertices in front | unchanged | clips the visible part; culling is not the cause |
| far-outside left edge, all in front | unchanged | clips; not the cause |
| quad crossing the bottom screen edge | unchanged | clips; not the cause |
| road quad with one vertex behind the eye | **flips** | a `GX_CULL_FRONT/BACK` test can drop the visible part |
| long road strip from under the car, two vertices behind | **flips** | same near-plane/culling risk |

The native code has a literal hole here: `native_cull()` in
`platform/wii/gs_gx.c` takes a `behind` argument, computes it with
`behind_camera()`, then does `(void)behind;` and culls only by the game's
`1013.z`. A batch that straddles the near plane can therefore be culled with a
winding that the PS2 clipper would have turned into a visible, clipped triangle.

But a road segment that is only partly below the screen, with all vertices in
front, is **not** this case. If that is what the user sees, look next at:

- `draw_setup()` scissor clamping (`x0/y0` are not clamped; `x1/y1` are);
- the target offset (`rt->ex`, `rt->ey`) on split-screen;
- `present()` copying `BIG_W x BIG_H` (640x448) and scaling to the XFB;
- the 16:9 path: `runtime/patches.c` and the 704-column VI width in
  `plat_wii.c`.

## Candidate causes, ranked

### B0. Native batch visibility test (low)

`world_batch_native()` runs the game's VU0 box/frustum test through
`vu0_box_world()`; if it returns "outside", the batch is not drawn at all.
The existing A/B checked 1.25 million calls with 0 differences (mode 1 56,200,
outside 513,740, inside 305,630, partly 379,430), and the wbn0/wbn1 benchmark
p50 was the same (24.87 vs 24.55 ms). This is therefore unlikely. It is still
worth one run at the actual bad spot with `0 wbn 0` if the visual case is
reproducible.

### B1. Near-plane culling (high)

`native_cull()` ignores `behind_camera()`. The PS2 world microprograms have a
per-vertex clipping path for the `1013.x` cases; the native path relies on GX
clipping and then applies a GX back-face test. A triangle that crosses `W=0`
can have its projected winding reversed. This is a plausible reason for a
sharp road edge or a missing piece under the car.

A test build can set `GX_CULL_NONE` when `behind_camera()` is true, or let the
VU1 path draw only those batches. The former is cheaper and keeps the visible
part; it may draw back faces that the PS2 would not, so validate a ground/hedge
frame. The latter is exact only if the emulated VU1 clipper is correct, which
the handoff says is not yet true at the hedge spot.

**Superseded (2026-10-22/23)** by the clipper in B2: a cut batch's near-plane
triangles are clipped in the world space and drawn as polygons with the
triangle's own winding, so `native_cull`'s guess no longer applies to them.

### B2. PS2 clip mode vs GX clipping (high)

`gs_backend_world()` draws a batch with `1013.x` through GX unless `gxflags 128`
is set. `gxflags 128` is too broad: at 160 s it produced

```text
world batches 0 native, 578 emulated
```

so it sends essentially the whole world through VU1 and is much slower. The
native path therefore uses GX's clip/guard band, not the PS2 microprogram's
per-vertex clipper. Differences at the screen edge, near plane, and in winding
are expected. A native, exact clipper for the `1013.x` batches is the real fix.

**Resolved (2026-10-22/23).** The native clipper is built: `platform/wii/clip.h`
and `clip_batch()` in `gs_gx.c` cut a flagged batch's triangles that cross the
near plane, or reach more than `--clipband` percent of the screen outside it,
against the five planes through the eye (the near plane and the screen's four
sides), in the world space, where every attribute is affine - so the picture is
mathematically unchanged and only the triangle the GPU interpolates over gets
smaller. The cut triangles are drawn from per-frame arrays, the rest of the
batch from its list as before. The host test (`tools/clip/cliptest.c`) checks
the exactness on 40000 triangles, and in Dolphin the race-start frame cut
against not cut is the same scene (no hole at the sides, the bottom or the near
plane). B1 is subsumed: the clipper cuts the near-plane triangles itself, in the
triangles' own winding, instead of leaving them to `native_cull` and the guard
band. The console run (`--clip 0` against not) is what says whether the warping
was this.

### B3. Z / near-far range (medium)

`native_projection()` uses `zmul=1`. The comment records that `zmul=16` made
the nearest geometry pass the far plane ("holes in the road, cars cut open").
`gxflags 8388608` is the half-range experiment (`ZDS=0.5`). The game's GS Z has
more range than GX's 24-bit depth; a near road vertex can be clipped. This is
deterministic enough to reproduce in a freeze if the geometry is the same; it
has not been isolated yet.

### B4. Scissor, target offset, and the 448-to-480 copy (medium)

The scene is 640x448 at the top of a 640x528 EFB; `present()` copies only
`BIG_H=448` and scales it to the XFB height. A bottom HUD element at row 447 is
inside that, but TV overscan can still cut it. `draw_setup()` clamps `x1/y1`
to the target but not `x0/y0`; a scissor starting at or beyond the target
returns 0. Split-screen adds `rt->ex/ey` offsets. These are the first things
to check for "HUD clipped at the bottom" or a road strip cut exactly at a
target boundary.

### B5. Strip conversion / ADC (low to medium)

All native world/car paths convert PS2 strips with:

```c
if (LW(b->pos + 16 * i + 12) & 0x8000) { i++; continue; }
```

The ADC/restart bit decides which triangle ends at a vertex. A mistake here
would produce a missing triangle or a sharp diagonal at a batch edge. The
existing checks report 0 differences for the batches they compare, so this is
less likely than B1/B2, but a synthetic strip that crosses the bottom edge is
worth adding to the unit/check tests.

### B6. GPU cache coherency (hardware only, medium)

The hardware-only symptoms fit the Wii's caches, while Dolphin has no cache
model. The relevant paths are:

- display lists and indexed vertex arrays: `dl_touch()` flushes the arrays only
  when a list is new, returns after frames away, or every 16th frame;
- `GX_InvVtxCache()` was added only at those flushes, not before every indexed
  draw;
- `tex_decode()` writes a new texture and calls `DCFlushRange`, but the only
  `GX_InvalidateTexAll()` calls are on eviction/copy/TLUT paths, not after every
  decode;
- `tlut_lookup()` writes a new TLUT and flushes it, but does not globally
  invalidate GX's texture cache.

These cannot be proved in Dolphin. The tests are `gxflags2 256` (flush every
frame) and a temporary `GX_InvalidateTexAll()` after `tex_decode()`/TLUT writes.
If the hardware skew/flicker changes, this is the cause. If it does not, stop
adding cache flushes.

### B8. The display lists were never written out (hardware only, high)

Found on 2026-10-21 while reading the cache paths. This is the strongest
candidate for the hardware-only *geometry* warping, and it is a plain bug.

`GX_CallDispList()` does not copy the list into the FIFO: libogc writes the
list's **address** and size as a CP register write into the write-gather pipe
(`gx.o`: `stb 0x40` at `0xCC008000`, then the address and the size), and the
graphics processor **fetches the list from that address in RAM**. The list is
therefore read by a bus master that does not see the CPU's cache.

Every list was built like this:

```c
buf = dl_room(cap + ext);
DCInvalidateRange(buf, cap);      /* throw away what was cached there */
GX_BeginDispList(buf, cap);       /* the CPU writes the list */
... GX_EndDispList();
```

`DCInvalidateRange` is *before* the writes, and there was no `DCFlushRange`
after them: the CPU's copy of the list stayed in its cache and the RAM kept
whatever was there before. The console could fetch an older list.

Why the picture was not a mess: the arena is a ring of eight segments, and a
list that is moved when the ring turns is copied with `memcpy` and flushed
(`dl_room`), so most lists in view had correct RAM behind them. A list built
for the first time (a section that has just streamed in) had stale RAM behind
it until the next turn: the console drew a *different* list's triangles, with
another section's vertex indices - geometry that looks warped rather than
missing.

The same gap was in the car's lists (`car_dl_get`, the normals behind the list
included), the sky's layers (`sky_mesh`) and the menu/speed-blur strips
(`gs_backend_strip`).

Fixed 2026-10-21: every list is written out (`DCFlushRange`) right after it is
built, before it is called. The count is in the status line ("gx: cache per
frame: %u lists written out to RAM").

### B9. The texture cache was not invalidated when its memory was reused (hardware only, high)

The same class, for textures. `tex_decode()` writes the texels into the texture
arena (`g_texmem`, MEM2), flushes them, and the texture object points at that
memory. The GPU's texture cache is keyed by address: when a span is released
and later given to another texture, the GPU may still have the *old* texels
cached for that address, and the console shows the old texture where the new
one should be. `GX_InvalidateTexAll()` was only called when the whole cache was
dropped, when a render target was copied, or on eviction.

The palettes (`tlut_lookup`) had the same gap: a TLUT slot that is used again
keeps its old palette in the GPU's cache.

Fixed 2026-10-21: a span remembers whether the GPU may have its old texels
cached (`Span.stale`), and `GX_InvalidateTexAll()` is called when such a span
is written into; a TLUT slot that is reused invalidates the cache too. The
count is in the status line.

Both fixes are invisible in Dolphin (its memory is coherent), so the frozen
frame at tick 650 must not change; the hardware is the only test that can
confirm the picture.

### B10. What is *not* a cache problem

Checked and cleared while looking for B8/B9:

- the display list arena, the texture arena and the guest RAM are all in MEM2,
  and the CPU's FIFO in MEM1 is the one libogc reads through the write-gather
  pipe, so the memory map is not the problem;
- `native_mode()` sets `GX_CLIP_ENABLE` for the native draws (only the GS path
  runs with the clip disabled, and it draws 2D quads);
- the vertex arrays the lists index are flushed when a list is made and every
  16th frame, and `gxflags2 128` counted no list whose arrays change in place
  in a 360 s drive, so the 16-frame interval does not hide a change;
- the colours and the UVs the GPU reads as arrays (the lit world's, the cars',
  the props') are flushed where they are written.

### B11. How much clipping the world would need (measured 2026-10-21)

`gxflags2 8388608` counts, per frame, the world triangles that cross the near
plane (the PS2's microprogram clips them and flips their winding, which GX
does not) and the triangles that reach far outside the screen (the guard band).
In a race, at the game's own interval:

```text
gx: clip work per frame: 165 batches with a triangle over the near plane,
1194 such triangles; 4406 triangles far outside the screen (1729 of them very
far), 279 batches with those
```

So 20-35% of the world's batches have a triangle over the near plane, and
57% of them have a triangle that reaches more than half a screen width
outside. Two consequences:

- **the edge-only VU1 fallback is not viable**: it would send a third of the
  world through the emulated microprogram, which is several times slower than
  the native path;
- **an exact clipper on the CPU is a per-frame job of about 5000 triangles**,
  with per-frame vertex arrays and per-frame display lists behind it. It is
  worth doing (it is the only way to reproduce the PS2's clipping exactly),
  but it is a project, not a patch, and it costs perhaps 1-2 ms a frame on the
  console.

The probe is left in the build (`gxflags2 8388608`) so the numbers can be
checked again in another scene.

### B13. The frame's EFB copies: what they cost on the console (measured 2026-10-21)

`gxflags 4194304` now logs every target's copy and restore (the big ones
included). A race frame, per frame:

```text
rtev 998 00000 copy 1: 640x448      <- the display buffer is saved to RAM, the region cleared
rtev 998 008C0 bind 0: 640x448     <- the scene takes the EFB region
rtev 998 008C0 restore 0: 640x448  <- the scene's last frame is drawn back in
... 1178 draws: the world ...
rtev 998 008C0 copy 1: 640x448     <- the scene is saved to RAM, the region cleared
rtev 998 00000 bind 0: 640x448     <- the display buffer takes the region
rtev 998 00000 restore 0: 640x448  <- the display buffer is drawn back in
```

So **four full-screen transfers** (two EFB->texture copies, two texture->EFB
restores) of 640x448 RGBA8: about 4.6 MB of traffic between the EFB and RAM
every frame, plus two full-screen textured quads. Dolphin does not charge for
it; the console's copy engine and its RAM do.

There is a way out that the port already has: `blit_alias` hands the EFB
region and its content from the scene to the display buffer when the game's
final blit is a plain 1:1 copy, and then neither the copy nor the restore is
needed. **It never fires**, for two reasons found on 2026-10-21:

- the game's final blit is not a plain copy: its vertex colour is 0xFFFFFF
  (the GS's 0xFF is 2.0, so the draw multiplies the picture by 1.99 and
  clamps it) where the alias wants 0x808080;
- the blit's frame register has 0xA0A0000 in its upper half, and the alias
  refused any draw whose frame mask was not zero. The mask's rule is the
  drawing path's own (`apply_state`): the colour and the alpha are written
  whole unless the mask covers them, and a mask that covers only part of the
  colour changes nothing. The check was relaxed to that rule (it still does
  not fire for this blit, because of the colour).

**The copies and restores are needed.** A test build that skipped the
restores of the two big targets changed 2,378 pixels of the frozen frame at
tick 650 - exactly the EA Games logo, which is drawn with alpha over the
region's old content. On the PS2 the scene (0x8C0) and the display (0x0) are
separate EFB regions, so the scene's content is still there when the game
draws into it again; the copy and the restore are how the port reproduces
that. They cannot simply be dropped.

**But the restores can.** The restores put a target's last content back into
the EFB region, and for the two big targets nothing can see it: the world
covers the scene, and the game's final blit covers the display opaquely
(its alpha register 0x44 and its vertex colour 0xFFFFFF make it a copy of
the source's colour), before anything is drawn into either. The restores of
the big targets were dropped on 2026-10-21: the status line now says "3
target copies, 0 restores" where it said "3 target copies, 2 restores", and
the frozen frame is the same picture (the only difference is the HUD emblem's
animation phase, 2,685 pixels - the same as between two runs of one build).
Two full-screen transfers a frame are gone.

What is left is the two copies: the scene's (the blit samples the scene's
texture, so it has to be in RAM) and the display's (its content is never
read, but the copy is also what clears the region before the world is drawn
into it - a clear without a copy would need a full-screen quad instead).

**The way to get rid of them is the PS2's own arrangement**: give the two big
targets their own EFB regions (the PS2's EFB is 5 MB and holds both), and
then neither is ever evicted, copied out or restored. **That is not possible
on the Wii**: a test build that put the big target at row 512 (the EFB 1024
lines tall, the strip of small targets at the top) broke the picture
completely - vertical streaks through the whole frame, 286,428 of 307,200
pixels different - which is the depth buffer (or the EFB) wrapping at 512
lines. The Wii's EFB holds one 640x448 region and a strip, like the port's
layout now, so the copies and restores stay.

What is left for this item: the copies themselves (the EFB->texture and the
texture->EFB) are the port's way of emulating the PS2's two regions, and they
cost about 4.6 MB of traffic a frame. The alternatives are to keep the
picture in a texture instead of the EFB (the display's content is only ever
read by the display copy), or to let the blit read the scene's texture
without the intermediate copy - both are changes to the same code and are
worth about as much as the copies cost.

### B12. The HUD and the picture's place in the frame (fixed 2026-10-21)

The picture is 640x448 in a 640x528 EFB, and it was copied to the XFB
*stretched* to the frame's 480 lines. The HUD is at the bottom of the 448
lines, so the stretch put its last row in the frame's last line - where a
television's overscan cuts it. On the PS2 the 448 lines are not stretched:
they are the top of the 480-line frame, and the lines below are black (the
television's overscan eats them).

Fixed: the copy writes the 448 lines at the top of the frame, unscaled, and
the frame buffers are cleared to black when they are taken. `"--tvfit 0"` in
args.txt brings the stretch back.

### B7. Widescreen / VI / TV overscan (high for HUD)

`runtime/patches.c` applies PCSX2's data patches for FOV, aspect, HUD width and
X place. The picture remains 640 wide and is stretched by the TV. `plat_wii.c`
sets `viWidth=704`, and the XFB is 640x480. HUD clipping at the extreme corners
is much more likely to be the 16:9 patch/VI/overscan combination than the 3D
renderer. Test `--widescreen 0` (and the Wii's 4:3 setting) before changing any
render code.

## Tests run on 2026-10-20

All are Dolphin's GX, not the console's Hollywood GPU. `frz.bin` is the
authoritative display buffer; the PNG from `wshot` is only the window.

- `gxflags 128` at tick 650: the frame looked the same as the normal picture,
  but the status line showed `world batches 0 native, 578 emulated`. The flag
  does not select only clipped batches; it sends effectively all world drawing
  to VU1. It is not a usable fix.
- `gxflags 32` (no native cull) at the parked frame at tick 1100: 310 native
  world draws instead of 301. The image still had the same road and buildings;
  the difference was in back-face edges, not a missing bottom strip. That spot
  does not exercise the near-plane case strongly enough.
- `gxflags 32` at tick 500: 245 world draws instead of 567, and a different
  camera frame (the car was approaching a fence). The two buffers are not a
  fair A/B.
- `wbn 0` vs `wbn 1` (recompiled visibility vs native): 1.25 million calls,
  0 differences; the benchmark p50 was 24.87 vs 24.55 ms. B0 is effectively
  ruled out.
- Default tick 650 and tick 1100 freezes: the road, HUD and screen bottom were
  intact in the display buffer. The hardware-only artifacts were not
  reproduced in Dolphin, as expected.
- Near-plane cull guard test: setting `GX_CULL_NONE` for every batch whose
  box reaches behind the eye was wrong. At tick 650, the same frame as the
  old-cull control, 80,367 of 307,200 pixels changed; the difference covered
  buildings and sky, not only a hidden road edge. The guard draws back faces
  that the PS2 culls. It was rejected and the build was reverted to
  `4D2235...`. The correct fix is a per-triangle clipper, not a whole-batch
  cull override.
- `gxflags 8388608` (half depth range) at the parked frame: the same scene and
  the same 311 world draws; the road and buildings were intact. Z range is not
  demonstrated as the cause at this spot.

The parked scene is at 5134.8 1957.8, about 118 dm from the intended hedge
point; the `goto` stopped against the fence. It is useful for a static A/B but
not the near-plane road case. A better test is a low camera or a replay frame
where the road passes under the car; that still has to be selected.

## Tests that separate the cases

Use a **single build** and `frz.sh` at the same game tick. The `frz.bin` is the
authoritative display buffer; the PNG written by `wshot` is a window capture,
not the frozen buffer. At tick 650 one build can differ from another in frame
number and timing, so compare two runs of the **same build**, not two builds.

1. **Near-plane cull**: add `0 gxflags 32` (no native cull) to a `goto` scene
   where the road passes under the car, freeze, and compare the frozen buffer.
   If the missing piece returns, B1 is confirmed. Then try a build with
   `behind_camera()` forcing `GX_CULL_NONE`.
2. **Clamped world**: `0 gxflags 128` is not a fair A/B (it sends the whole
   world to VU1); use it only to compare the same frame visually.
3. **Z**: `0 gxflags 8388608` (half depth range).
4. **Strip edge**: a synthetic road strip that crosses the bottom edge and a
   strip whose ADC bit turns on at the screen edge; compare native and VU1
   outputs in the existing check harness.
5. **Cache**: `0 gxflags2 256` (flush every frame), then a temporary global
   texture invalidation.
6. **HUD**: `0 widescreen 0` at the start grid; if the HUD returns, B7.
7. **Display copy**: freeze at a frame with the HUD at the bottom and compare
   the 640x448 target with the 640x480 XFB (the copy/scaling path).

Do not judge any of these from a window screenshot; use the dumped YUV buffer.

## Performance: what can really move the console

The hardware profile is not a single hot loop. Native work is spread over
memcpy (10%), car shading (7.4%), packet/texture identity (5.7%), audio (4.7%),
draw setup (2%), lit-world shading (2%), car backend (1.7%), VIF/GIF/DMA, and
many smaller pieces. A flag change or a 1% micro-optimization will not reach 30.

### P1. World sections as native display lists (largest exact item)

Instead of walking the game's chain and parsing/drawing each 15..75-vertex
batch every frame, record a section's batches as GX display lists once per
input generation, test the section once, and replay. The handoff's estimate is
3-5 ms. It removes a large part of `pc_cached`, `draw_setup`, VIF packet
parsing, and backend state, not just one function. Exactness requires:

- the same texture, blend, alpha, cull and LOD order;
- the same section visibility result (a conservative "never cull what the VU
  would draw" is safe, because GX clips the extra geometry);
- per-frame lit colours where the light changes (world lights are per-frame).

The existing `0x26AE30`/`0x21C780` walker can be overridden without replacing
the whole game.

### P2. Direct GPU FIFO register writes

`draw_setup()` and libogc's GX shadow state account for several percent. The
same register writes can go into the GX command FIFO directly in the hot draw
paths. This is exact and estimated at 0.5-1 ms.

### P3. Native car path / lighting

The car CPU shading is 7.4% (`nc_shade_ps_pairs`). A GPU lighting approximation
or a normal-to-colour table would remove 0.3-0.5 ms but is not bit-exact. The
user has allowed differences that cannot be seen; this needs an A/B picture at
a fixed tick. The car batches already use display lists; the remaining cost is
the per-vertex quadratic light and sphere-map UVs.

### P4. Audio on the DSP

`mix_out` + `rev_add` + voice work is about 4.7% (1 ms). Moving the mixer to the
Wii DSP is a reimplementation, not an exact float port. It can be made
inaudible, but it needs a reference and a listening test. This is the largest
non-render item.

### P5. Compiler across recompiled game functions

The recompiled code's context traffic and stack spills are significant. Putting
hot caller/callee chains in one translation unit for inlining, and compiling the
hot 1% at `-O2` instead of `-Os`, is exact; the handoff estimates 1-2 ms.

### P6. Console-only memory work

The current 32 MB PS2 RAM is in MEM2; the scratchpad and VU memory are also
MEM2. On Broadway, the 16 KB locked L1 cache as a scratchpad and hot
structures in MEM1 could be large. `memcpy` is 10% of the profile, and
`dma_spr` is a third of that; `ext_source_ident` touches 16 cache lines per
texture; `pc_cached` validates guest words; display-list headers are in MEM2.
This cannot be measured in Dolphin and should be A/B'd behind a switch. It is
the most likely place where hardware can be much better or worse than the
Dolphin numbers.

### P7. Texture/palette path

Placed rasters and the palette cache already saved a lot. The remaining
`ext_source_ident`, `gs_load_clut`, `put_row`, and staged uploads are perhaps
0.5-1 ms together. Prefetch the 16 sampled words, avoid rehashing unchanged
sources, and make small palette transfers direct.

### P8. Post effects, final display, and the 448-to-480 copy

The profile says sync is 1%; the final display blend is not the prize. Do not
repeat the black-screen final-blit experiment. The post chain is already native
and small compared with the world/car/VIF path.

## What this means for the target

- **30 fps** is plausible only after P1 plus P2/P4/P5 and, on the console, P6.
  The exact CPU pieces in the handoff add up to 3-5%; the hardware cache layout
  is the unknown.
- **60 fps** is not demonstrated by any combination of the currently named
  exact items. The world/car/VIF layer has to shrink structurally, not by
  cadence or quality cuts.
- **Forcing 30 fps mode** reduces the game's update work but not the native
  renderer. The profile's native share is too large for that alone to be a
  guaranteed fix. It remains an opt-in test, not a default.

## Recommended order

1. Reproduce the road/HUD case on the same build with the frozen-buffer tests.
2. Fix B1 (use `behind`) and, if it is the edge-clip case, implement the native
   `1013.x` clipper; validate against VU1 and a frozen frame.
3. Test B6 cache hypotheses on hardware with a switch; keep only what changes
   the picture.
4. Start P1 (world section display lists), then P2, then P5; keep P4/P3 behind
   picture/listening comparisons.
5. Only then decide whether the console needs P6 and whether 30 fps mode is an
   acceptable fallback.

No new default flag should be shipped until the frozen-buffer or hardware test
shows a difference.
