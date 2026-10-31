# Native Wii renderer

Burnout 3's PS2 renderer builds VIF/GIF DMA chains for the VU1 microprograms
and the GS. Emulating that path (VIF unpack, VU1 microcode, GIF packets, GS
primitives, per-frame texture streaming) costs far more than the Wii's CPU
has: a race frame took ~200 ms in Dolphin with everything recompiled and
cached. The plan is to replace the game's render functions with native GX
code at the level the game works at (render lists, objects, materials,
cameras), keeping the recompiled game code and the platform-independent
parts of RenderWare. The emulated path stays as the fallback for whatever is
not ported yet.

## Ordering

The game builds a whole frame's DMA chain and kicks it once. A native draw
made when the game calls a render function would run before the chain that
holds the earlier draws of the same frame. Native render functions therefore
record their draws and put a marker into the chain; the draw runs when the
chain reaches the marker. Once a frame's renderers are all native, the chain
goes away.

## Map of the PS2 renderer (SLUS_210.50)

Names are ours unless noted; Burnout 2's DWARF-named decomp
(b3dllc/burnout2, `GsPS2Pipeline`, `PrelitWorld2`, ...) is the guide to
concepts, not to addresses.

| Address | What |
|---|---|
| `0x276300` | in-game frame render: calls everything below |
| `0x275EF0` | frontend frame render (from `0x1312D0`) |
| `0x21C010`..`0x21D770` | render-list walkers ("Gs" framework): call the pipeline functions per object |
| `0x26A1C0`, `0x26A610` | per-section render: transform setup, then `0x26A240` |
| `0x26A240` | per-section DMA: per-frame qword list, VU0 lighting (`vcallms`), MSCNT, CALL of the section's render data |
| `0x26B2F0` | group setup: picks the microprogram by pipeline type (0..7, like Burnout 2's `EGSPS2PipelineID`), binds the texture through `0x221A70`, uploads VU1 constants 1005..1009 |
| `0x26BAA0` | per-section state update |
| `0x221A70` | texture bind (RenderWare texture cache) |
| `0x1F3080` | open a DMA packet (RenderWare DMA queue `0x1F1778`..`0x1F3A10`) |

World render data: one RET-terminated DMA packet per strip batch (15..75
vertices) in heap memory, `UNPACK V4-32` positions (w = strip restart flag
0x8000), `V2-16` UVs (1/4096), `V4-8` RGBA, `ITOP` count. World sections are
in world space; VU1 constants 1014..1017 hold one camera matrix per frame
(x, y and w rows of a world-to-screen transform), 1018/1019 screen size,
centre and Z scale/offset, 1020..1023 fog and guard band, 1013 per-batch clip
flags. One race frame: ~525 batches.

VU1 microprograms by caller: world `46F0D0`/`4745C0`/`47E300` (prelit),
`470650`/`472EB0` (lit, normals), `471FA0`, `476130`, `4803F0`/`481DD0`
(`0x26B2F0`); `46D750` (`0x2546A0`), `46BB80` (`0x220570`), `46A590`
(`0x21DE6C`), `46D090` (`0x2636E4`), `4771B0`/`475910` (`0x2F3xxx`),
`478260` (`0x306E90`), `47A060`/`47C6F0` (`0x3657AC`/`0x3659A0`, garage).
VU0: `4787D0`, `479610`, `47F6D0`, `4839E0`.

## Native pipelines

| Microprogram | What | How |
|---|---|---|
| `46F0D0`, `4745C0`, `47E300` | prelit world (V4-8 colours) | `0x26AF50` override, display list per render data packet |
| `470650`, `472EB0` | lit world (V3-8 colours, lighting data at TOPS+204) | display list with indexed colours; the colours (RGB plus the specular alpha, `nw_lit_rgba`) are made per frame into a two-half array (frame parity: the GPU may still draw the previous frame) |
| `471FA0` | instanced props (V2-32 UVs, colour and matrix per instance at 932..) | same, one display list call per instance; with 932.y set the props are lit: V4-32 normals at TOPS+2 (x, y, z of four vertices in the third qwords of the first three), grey = 934.z + 935.z * (n . 938.xyz) per instance, as floats biased by 2^23 whose low byte the GS takes |
| `46BB80` | cars | VIF level (below): `runtime/nw_car.c` shades on the CPU. Undeformed batches: a display list per batch over the render data (positions; the unsigned 4.12 UV pairs, whose halves GX reads swapped, through `GX_TEXMTX2`) with the pass 1 colours made per frame into the lit colour array (`nc_shade_direct`, no copy of the vertices); pass 2 indexes the positions and sends its sphere-map UVs directly, colour from the material register. Deformed batches (1013.z) are sent immediate. The first 300 display-list batches are checked against `nc_shade` (log "car display lists: 300 batches checked") |

Untextured world batches (no UV unpack: the garage, a few in races) take the
same paths when the microprogram's PRIM has TME clear; the TEX0 array then
points at the positions (read, unused). Render data packets of up to 16384
qwords and 192 batches are taken (car select has 5300-qword lit packets).

Render data reaches the VIF through DMA `CALL`s. For cars the VIF keeps the
call instead of following it (`vif_defer_call`); the next `MSCNT`/`MSCAL`
then draws natively from the render data in guest memory and the constants
already in VU1 memory, without running VU1. When the native draw can not
handle the batch, the VIF sends the render data after all and runs VU1, which
ends in the same state. This needs no game code changes and works for any
caller; the world pipelines could move to it too.

The car microprogram (`46BB80`, ~2000 instructions):
- crash deformation when 1013.z is set: per vertex two matrices from a
  palette at 974.. blended with weights in slot 1 of the odd vertices
  (value / 65535, palette offset = value mod 1024);
- colour = 1010 + (1011 - 1010) * L with L = (n,1)^T M (n,1), M = 966..969;
  alpha from a fresnel term of n and 970.xyz;
- unless 1013.y is set, sphere-map UVs from the eye (971..973 .w) to the
  vertex reflected about n, rotated by 971..973;
- pass 1 in GS context 1 (PRIM `05C`) with the lit colour writes the fresnel
  term to framebuffer alpha; pass 2 in context 2 (PRIM `25C`) blends the
  reflection by destination alpha (ALPHA `54`);
- 1006..1009: a GIF packet of GS state sent before the first batch of an
  object (1006.x cleared after).
`work/nctest.c` checks `nw_car.c` against VU1 runs captured on the PC host
("vucap N 46BB80"): exact colours and UVs, positions within 0.07 px. On the PC
host `B3_CARCMP=1` compares every batch's triangles with what VU1 kicks; the
car's Z is converted without FTOI4's x16 (the world programs scale by 16).

Native draw details found against the PCSX2/software reference:
- world culling (1013.z: 0 one winding, 0x20 the other): through the native
  projection 0 is `GX_CULL_FRONT`; batches with 1013.x set take the
  microprogram's clipping path (per-vertex clip flags decide which triangles
  are drawn) and go to VU1 instead, else the road under the camera had holes;
- PS2 FIX blends write the source alpha, GX would write the factor: a second,
  alpha-only pass (`fix2_set`), which the car glass needs for its reflection.

## Native CPU code (performance)

Measured with the Wii sampling profiler (autopilot `prof`, `work/wprof.py`):
- VU0 microprogram entries (`runtime/vu.c`, `k_vu0_native`): races call VU0
  ~3700 times a frame, mostly tiny entries where dispatch dominated. Done:
  `4787D0@0A18` (frustum test of a box: per plane the extremes from the
  signs of n.e), `479610@0000/00C0/0920/0968/0998/09D8/0A18` (box corners,
  record lists), `479610@0138` (an object's visibility for up to three
  cameras and its instance record). Each entry's first 100 calls also run
  the recompiled code and compare every register and VU0 data memory.
- Sky `46A590`: the program still builds its dome (cosine series, four
  mirrored quadrants), but its subroutine `0x0888` (transform, polygon
  clipper, divide, kick) is `vu1_sky_strip` in C (config `VU_NATIVE_SUBS`,
  `tools/vu/vurecomp.py` calls it at each `bal`): each strip is drawn with GX
  hardware transform and clipping, screen = vf2 + (X, Y)/W, GS Z = vf2.z +
  vf31.z/W, after the A+D state packet at 208..216 once a run.
- EE audio DSP loops `0x2FBE20` (state-variable filter, wave folder, drive)
  and `0x2FC820` (stereo mix) are `OVERRIDES` in `runtime/hle_audio.c`,
  bit-exact (self-checked for 24 calls).
- Texture uploads: in races all ~640 uploads a frame (2.9 MB) point at guest
  RAM; `lazy_finish` records each 256-byte block's owner, index and identity
  (source address, stride, format, three words) with the rows stepped, not
  recomputed. A CLUT load that misses the CLUT cache reads its entries
  straight from the pending upload (CT32: one row pointer per 8x8 block)
  instead of writing the blocks to GS memory first.
- GIF: packets of A+D only (every texture upload's BITBLTBUF/TRXPOS/TRXREG/
  TRXDIR header, GS state) are written register by register in
  `gif_path_bulk`; image data of transfers that are not lazy (small
  palettes) is unpacked and written a row at a time (`xfer_words`).
- Indirect calls (`ps2_call`) find their function through a 1024-entry
  direct-mapped cache before the binary search of the function table.
- Native shading: conversions that went through memory on Broadway (int to
  float, float to int with compares) are table lookups (normal bytes, exact
  for the usual ROW of 2^23) and plain fctiwz, which saturates as FTOI0 does.
- Display: three XFBs. With two, the copy of a new frame went into the
  buffer still on screen until the next retrace (the flip only happens
  then): a quick copy showed the new frame and then the older one, and time
  rubberbanded. The status line counts how many retraces each frame stayed
  and how many vblanks the game took for it ("frames shown for ...").
- Small render targets are rasterised into GS memory only while something
  reads GS memory inside them (`soft_draw`): the sky gradient 0x2D40 is never
  read and cost ~15% of a frame.
- I/O: the memory card file is cached in 8 KB blocks (`iop_sio2.c`): the game
  polls the card ~3000 times a second at the "load successful" prompt; the
  disc image keeps its last 32 KB blocks and several handles (`plat_wii.c`):
  looping menu audio made every jump back a FatFs walk over the whole
  cluster chain. Boot to the main menu: ~60 s in Dolphin (was 70..150 s).

## Render targets and post effects

The game renders tables and textures that it reads back from GS memory:
- the sky: a 32x32 source inside the 64x32 target at 0x2C00 (drawn at load),
  a gradient target 0x2D40 drawn from it every frame, the cloud CLUT inside
  the target 0x2CC0, the haze palette as 256 points into 0x2D20;
- targets whose layout changes (0x2D60: cleared as 64 wide, the 256x256
  reflection map, then a 320x224 blur buffer) are reshaped on GX.
Targets of 64x64 or less are also drawn into GS memory by the rasteriser
(`runtime/gs_raster.c`, the PC renderer's core), untextured points always;
CLUTs and textures inside targets then come from GS memory. A target drawn
in many frames that nothing has read from GS memory lately is left to the
GPU (a later read falls back to `rt_readback`). CPU readbacks of
EFB copies (`rt_readback`) are a last resort: Dolphin keeps EFB copies on the
GPU by default, so they read zeros there. `B3_RTDEP=1` on the PC host lists
every read of rendered GS memory.

End of frame (after the HUD's 3D parts):
- depth haze: 16-bit views of the colour and Z buffers put Z bits 8..15 into
  the alpha bytes, then the alpha is read as T8H through the haze palette.
  GX: the T8H read takes an EFB depth copy (`GX_CTF_Z8M`) instead;
- bloom: bright pass (scene - 0xBF) x 2 at half size, ~20 blur passes, a
  running sum with last frame's (x 0x3B/128), added to the scene. It is built
  from 16-bit aliasing and pages that interleave between two buffers; GX
  skips those draws and does it directly (`post_bloom`): one box-filtered
  half-size copy, bright pass and blur in an 80x56 strip target, the running
  sum, one additive quad;
- radial speed blur through 0x2D60 (at speed): the scene at half size into
  the 320x224 buffer, a 32x32 radial alpha ramp drawn into its alpha as 22
  mirrored sprites (its UV range grows with speed), three 50% passes of the
  buffer onto itself at a slight zoom (UV rectangles), then the buffer over
  the scene through that alpha. GX (`post_speed_blur`, noted by
  `speed_blur_note`): the eight equally weighted zoomed copies those passes
  amount to, in one pass (eight texture coordinates from position, TEV sums of
  1/8), over a 16x14 grid whose vertex alpha is the mask; empty cells are not
  drawn. gxflags 65536 turns it off; 16384+32768 logs the game's blur draws.

Split-screen (2 players): the scene is still one 640x448 target, P1 in rows
0-223, P2 in rows 224-447, and each player's view is drawn in full before the
next: reflection views, sphere map, world, haze, bloom, speed blur. So:
- a post chain is a view's: the rectangle its bright pass reads (640x224),
  one bloom accumulation per view, the speed blur over the rectangle its
  composite covers; a chain ends at its first draw that is not the chain's
  (the next view's reflection clears, which it used to swallow);
- P2's haze is drawn through FBP 01180 (row 224 of the scene): a draw into an
  FBP with no target, whole page rows inside a screen-sized target of the same
  width, goes to that target moved down (`view_alias`);
- both players' reflection views are the same three targets: views held in
  turn keep a picture per player (`GS_NOTE_VIEW`, `rt_view_owner`).

A speed blur can also come with no bloom before it: after a crash's replay
(from the first crash on, every frame) a draw into the scene ends the bloom
chain, then the blur runs. Its half-size copy of the view into the blur
buffer starts a chain of its own, done by `post_speed_blur` alone
(`post_end`); drawn as the game's it gave a bright frame of blocky vertical
strips and a blocky blur after it.

## Steps

1. World: native section draws from GX display lists built once per render
   data packet, the camera from the game's constants, materials from the GS
   state the group setup still emits.
2. Textures: rasters converted to GX once, keyed by the raster, instead of
   the per-frame VRAM streaming.
3. Cars, traffic, props, sky, particles, post effects, HUD.
