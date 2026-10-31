# S1: the world's batches without the PS2 round trip: design (2026-10-17, no code yet)

Aim of item S1 in `docs/plan-60fps.md`: the walker hands the world's batches to GX directly: no chain, no VIF dispatch, no parse
check, no GS state decode. This document says what that would replace, what the chain carries that must still be left as
the original leaves it, how it would be checked, and what it can save. **Result: much less than the plan's 1.0-1.5 ms.**
Numbers are Dolphin's cycles (clock reads, `work/walker_sizing.md` section 9; the handoff's R.0 counters).

## 1. What happens to one world batch today

1. The walker (`f_0021C780`) calls `0x26AF50` (native since today, ~1,000 cycles: box test, then 5 qwords into the staging
   list `[gp-23648]`): a CALL tag to the batch's render data, the state qword (S+0), clip-flag qwords when the mode changed,
   the qword at S+32 or a CALL to the second packet with MSCNT.
2. `0x26B2B0` per section (69 a frame): `0x26B2F0` (VU1 program by section type, texture bind `0x221A70`, an 11-qword GS packet),
   then `0x26B1C0` copies the staged qwords into the real chain (native today).
3. The chain is drawn later, in the flip's interrupt: DMA kick, `dma_run`, `vif_feed_words` per qword, the CALL reaches
   `vif_defer_call_in`, which takes the packet's parse from `nw_packet_ref` (`pc_cached`: the first and last 24 words the
   parse read must be unchanged), then per batch: `gs_backend_world`: VU1 qword 1006 -> PRIM, `gs_draw_state`,
   `draw_setup` (fast path 30 instructions when nothing changed; the slow path ~100 times a frame for texture/TEV changes),
   `native_mode`, `native_projection`, `native_cull`, `dl_get`/`dl_touch`, two `GX_SetArray`, `native_scroll`,
   `native_camera`, `GX_CallDispList`; then `vif_batch_done`.

Per batch, measured (R.0, 2026-10-15, 430 batches a frame): parse check ~720 cycles, `vif_defer_call_in` ~280, `gs_backend_world`
~460 of its own, `draw_setup` fast path ~150, the projection/camera compares ~150 ("each already compare-and-skip"). The world's
backend total in the VIF's call is 0.97-1.09M cycles a frame (parse 0.27-0.30M, draws 0.56-0.64M of which `draw_setup` 0.31M).

## 2. What the chain carries besides the draw (each must still be left as the original leaves it)

| # | effect | who reads it later | how a direct path keeps it |
|---|---|---|---|
| 1 | VIF state: CL/WL/MODE/ROW (`vif_batch_regs`), TOPS/ITOP, the double buffer flip (`vif_batch_done`) per micro start | every later VU1 run and unpack | apply the same two calls per batch from the cached parse (they are plain stores) |
| 2 | VU1 data memory: the view packet's UNPACKs (camera rows 1014-1017, projection 1018/1019, 1020-1023, clip 1013) and the state qwords; `gs_backend_world` writes 1013.w = 1 | props, cars, particles, sky, HUD 3D, the emulated VU1 programs | keep sending the view packet through the chain (17 qwords a (section, view); it is the batch commands that go, not the packet) |
| 3 | GS registers from the 11-qword group packet (TEST, ALPHA, TEX0/1, CLAMP, ZBUF, FRAME) | later draws that do not set them | the group packet stays in the chain |
| 4 | `g_vu1_code` (the program the group set-up selects) | `nw_kind`, the next section | stays (the group packet and its MSCAL stay) |
| 5 | texture side: `hle_tex_send` notes in chain order (a TBP is reused within a frame), CLUT loads, `gs_ver`/`gs_clut_ver` | the texture lookups of every later draw | stays in the chain; order untouched |
| 6 | guest memory written by the recompiled callees (S+155/156, `gp-28060`, `gp-24080`, ENV_CUR, the scratchpad copy) | later guest code | the walker and its callees run unchanged |
| 7 | the backend's shadows: `g_cur` (draw state), `g_nst` camera/projection, the TEV memo, `g_native_mode`, libogc's own GX state | the next draw of any kind | a skipped call must be one that would have changed nothing |
| 8 | order against everything else in the chain (target switches, reflection views, cars, post effects) | the picture | drawing stays in chain order: nothing is drawn at build time |

Effects 1-5 and 8 are why "no chain" is not available: the chain is the PS2's state machine, and the cars, props, sky and
HUD still need it. What can go is only repeated work *inside* the per-batch handling.

## 3. The design

* **S1a, steady-state world draw (backend, small).** For consecutive batches of a section the inputs of `draw_setup`,
  `native_mode`, `native_projection`, `native_camera` and `native_scroll` do not change. Keep a token of them (a GS register
  write counter, a counter of writes to VU1 qwords 1005-1023 bumped by the VIF's UNPACK and by every native writer, `g_native_mode`,
  the target, `gs_ver`/`gs_clut_ver`) and, when it equals the last batch's, go straight to `dl_get`/`dl_touch`/arrays/call.
  Nothing is skipped that would have issued a GX call.
* **S1b, one list command per section (VIF).** Tried on 2026-10-14 as `VIF_NATIVE_LIST`: the VIF expanded the same steps and
  saved ~80 cycles a batch (9,000 a frame). Not worth doing again.
* **S1c, the parse check by generation (parse).** The check (~720 cycles a packet, 0.27-0.30M a frame) could be a streaming
  generation if every write into the track's heap went through a place we control (the disc/SIF paths, the memcpy and DMA
  overrides). Recompiled EE stores into that memory are not tracked, so it is exact only if an audit shows the structure of
  render data (VIFcodes) is written once by the loader and never by game code. A debugging mode would compare every packet's
  full read set each frame for a long run; zero changes is evidence, not proof. Do only if wanted.

## 4. How it is checked (S1a)

Switch `s1`: 0 the current path, 1 the steady-state path, 2 both. In mode 2 every skipped call is made anyway and the shadow
it would have changed is compared (a "miss" = a call that would have emitted a GX write: counted and logged with the batch). Then
a digest of the GX side in one binary on the deterministic benchmark window (ticks 600-900; `wstat 3`'s world hashes already
repeat run to run): a rolling hash over every world `GX_CallDispList` (list pointer, array bases, PNMTX1, TEV/pixel keys, texture
object) and, at each chain's end, the VIF registers, VU1 qwords 1005-1023 and the GS registers; `s1 0` against `s1 1` must give
equal digests frame for frame. The first miss or unequal frame stops the run with the frame and batch.

## 5. What it can save

| piece | cycles a frame | ms | exact | effort |
|---|---|---|---|---|
| S1a: `native_projection` 0.69%, `draw_setup` fast path 0.44%, camera 0.5%, scroll/mode 0.2% of a frame, ~60% skippable | 40k-70k per 0.1 ms | 0.2-0.3 | yes | small-medium |
| S1b: the list command | 9,000 | 0.01 | yes | not worth it |
| S1c: the parse check by generation | 0.27-0.30M of it | 0.2-0.4 | only if the audit passes | medium + audit |

**S1 as a whole: 0.2-0.7 ms, not 1.0-1.5.** The dispatch the plan counted (~0.9% of the frame) is mostly the VIF reading the
chain, which the cars and the rest still need. The same shape of argument cuts S2 (cars: the GS packet per object is 252 a
frame at ~1,000 cycles, but the cars' own shading and batches are native already) and S3.

## 6. Consequence for the plan

With E1 and E2 re-measured (0.15-0.25, 0.25-0.35) and S1 at 0.2-0.7, the named items total about 3-5.5 ms of the 7.6 needed
at the race start. In Dolphin's count the heaviest moment ends near 19-21 ms: 60 fps is out of reach there with these
items, and reachable for most of a lap. The console run decides whether that picture is right. Recommended: S1a (small,
exact, checkable), E4, then the DSP (N1) if the console says the processor, not the GPU, is the limit.

## 7. Addendum, same day: why S1a was not built

Reading the per-batch code (`gs_backend_world`, `native_projection`, `native_camera`, `native_scroll`, `native_cull`,
`draw_setup`, `gs_draw_state`) to build the steady-state path showed two things.

* **The token of section 3 cannot be exact.** It needs a counter bumped by every writer of VU1 data memory qwords
  1005-1023. The writers are the UNPACK paths of the VIF (`unpack_bulk` and the others through `v->mem`), the emulated VU1
  programs (`gen/vu1_*.c`, `sq` through `vu->mem`), the EE's own stores into VU memory (`hw.c`), and natives in `gs_gx.c`
  (`vm[1013 * 4 + 3] = 1` and more). A bump missed in one of them leaves a stale camera or projection: a visibly wrong world.
  The shadows the four calls compare against (`g_nst.cam`, `.proj`, `.pin`, `g_uv_scroll`, the cull) are also rewritten by cars, props,
  the sky and `ortho_mode`, so "nothing else touched them since the last batch" is a second invariant to prove.
* **What is exact is what the code already does:** compare the inputs (12 words for the projection, 12 for the camera) and skip.
  `gs_draw_state` is two compares and `draw_setup`'s fast path ~30 instructions. The ceiling if all four were free is
  `native_projection` 0.69% (0.17 ms) + the camera ~0.12 ms + scroll/cull/mode ~0.05 ms = ~0.3 ms; a single fingerprint compare
  of the ~25 words they read would still cost ~100 cycles a batch (0.06 ms). Net 0.1-0.15 ms.

So S1a is 0.1-0.15 ms, not 0.2-0.3, and the way to more is the risky one. Not built; S1 in the plan is 0.1-0.5.
