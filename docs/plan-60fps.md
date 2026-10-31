# Can the Wii hold 60 fps in a race? An itemized plan (2026-10-17, extended 2026-10-23 with the console's own profile)

All times are Dolphin's: about one cycle an instruction, no cache misses. Nothing is measured on a console yet, so every
number can move either way there.

## Where we are

Race start (the heaviest stretch, `work/fb.sh`): ~24.3 ms of work a frame after today's commits (24.8 before the walker
natives). 60 fps needs <= 16.7 ms, so **-7.6 ms (-31%)**. Lighter stretches already reached 59.8-59.9 frames/s at
interval 1 (2026-10-12/13 notes); the pacer drops a frame that does not fit to 30 fps, so the stutter is in the heavy
stretches. Profile of the race start (samples): game code 22% (5.4 ms), VU code 15% (3.5), gs_gx 14% (3.3), gs 11% (2.7),
vif 9% (2.2), cars 7% (1.75), memfast 4%, sound 4% (1.0), render/world glue 4%.

## The plan, by kind (ms of the race-start frame; confidence H/M/L)

| # | item | basis | gain | exact | effort | conf |
|---|---|---|---|---|---|---|
| done | walker natives `0x26AF50`, `0x26B1C0`, `0x26BAA0` | measured, 3 A/B runs | **-0.48** | yes | done | H |
| E1 | palettes placed as TLUTs like rasters; fewer CLUT loads through the GS | clock reads 2026-10-17: the whole palette path is `gs_load_clut` 0.16-0.26M cycles (180-350 loads), `lazy_finish` 0.09-0.11M (40 uploads), `xfer_run` 0.04M a frame = 0.4-0.55 ms, of which the TLUT loads stay; the "pink surfaces" risk class | 0.15-0.25 | yes | medium | M |
| E2 | collision loops as natives instead of per-triangle VU0 calls (`0x273070`, `0x273BF0`, `0x273400`) | clock reads 2026-10-17: the three callers 0.46M cycles + `0x274C90` 0.16M a frame (0.85 ms) inside a physics step of 0.57M; the arithmetic stays, the dispatch and glue go | 0.25-0.35 | yes, bit for bit (gameplay reads it) | medium | M |
| E3 | box test and `vu0_vis` arithmetic cheaper (paired singles, loop-invariant parts), same results | box test 0.32 ms + `vu0_vis` 0.43 ms | 0.2-0.3 | yes | medium | L-M |
| E4 | `0x1F3080` fast path, camera/projection/state compares skipped when nothing was written | 680 calls (0.2 ms net) + ~0.2 ms compares | 0.2-0.3 | yes | small-medium | M |
| E5 | group set-up and texture bind own code (`0x26B2F0`, `0x221A70`) as natives | 0.6 ms of game code, ~140 + 110 calls | 0.2-0.3 | yes | medium | L-M |
| S1 | world batches handed to GX directly (design: `docs/design-s1.md`): the chain also carries the VIF/VU1/GS state the cars, props and HUD need, so only repeated per-batch work can go (steady-state draw path; parse check by generation if audited) | per batch ~720 parse + 280 VIF + 460 backend + ~300 compares/setup | 0.1-0.5 (S1a alone 0.1-0.15: `docs/design-s1.md` section 7) | yes | medium | L-M |
| S2 | the same for cars | cars 1.75 ms + `gs_backend_car` + VIF share | 0.3-0.5 | yes | large | L |
| S3 | the same for particles, sky, HUD 3D | VU1 programs + GS share | 0.2-0.4 | yes | large | L |
| N1 | sound mixed on the DSP (**built 2026-10-22**: `tools/dsp/dspmix.s`, `platform/wii/dsp_mix.c`, `--dsp 1`; off by default) | `iop_sd` + `hle_audio` 4.1% = 1.0 ms | 0.8-1.0 | no (fixed point, likely inaudible) | large | M |
| N2 | car lighting on GX's lights | weighed 2026-10-16 | 0.3 | no (invisible?) | medium | M |
| N3 | compiler: options on the hot functions (**built 2026-10-23, ships**: `PS2_HOT_VARIANT=1`, `-O3` on HOT_FUNCS; +57 KB text, MEM1 had 4.9 MB free; measured 2026-10-17, `work/hot_opts_run.sh`): `-O3` -0.205 ms (-0.85%, +57 KB), `-Os` +0.16 ms, other register-allocation/scheduling options -0.01 to +0.08; cross-function units not tried | `-O3` same world hashes over the 48 benchmark frames, same fused-op count | 0.2 (O3) | yes | small | H |
| N5 | recompiler's third step (context traffic at call boundaries: sign words, v1 at returns, VU0 lanes), `work/ctxcost.py` 2026-10-17 | context loads/stores are 7.0% of the recompiled code's time = 0.36 ms in all (lane 1: 2.1%, lanes 2-3: 1.0%); the handoff's 1-3% was a static count | 0.1-0.2 | yes | medium-large | M |
| N6 | the recompiled code's stack traffic (spills of values live across calls, callee-saved saves) | 18.4% of the recompiled code's time = 0.94 ms; no cheap handle seen | ? | yes | large | L |
| N4 | console only: locked cache as scratchpad, `dcbt`/`dcbz`, MEM1 placement | cannot be seen in Dolphin | ? | yes | medium | - |

Totals (E1, E2 and S1 re-sized 2026-10-17): E 1.0-1.7, S 0.6-1.4, N (without N4) 1.3-1.6: **3.1-5.6 ms of the 7.6 needed.** Even with every item the race
start ends at about 19-21 ms in Dolphin's count.

**2026-10-23: two of the small ones measured in the build.** The race start (`work/fb1.sh`, the `work/auto_grid.txt` scene, the
300 frames from clock 600, 4:3) was **26.14 ms p50 / 27.14 p75 / 28.43 p90** before and **25.61 and 25.77 / 26.83 and 27.11 /
28.71 and 28.87** after (two runs): `-O3` on the hot functions became the default (N3; its isolated measurement was -0.2 ms)
and the clipper's own work was cut (the depth scan computes W alone, the 24 biggest triangles' test is inlined instead of
called with twenty spills, and a batch's classes are built only when something is actually cut). The sampled status line's
clipper cost went from 380 to 355 µs at the same moment of the same scene; p90 is inside its run-to-run noise. The p50
gain is ~0.45 ms (1.7%).

N1's stage 1 is built and tests clean: `tools/dsp/dspmix_test.c` (a standalone dol, Dolphin's DSP in LLE) sends
random voices through `platform/wii/dsp_mix.c` and gets sums that match a step model of the microcode's integer
arithmetic exactly (0 of every run's words differ) and `runtime/iop_sd.c`'s `voice_stretch` to at most 2.15 LSB
(rms below 0.7), two runs out at once, 78 of 78 answered (`dspmixtest: ALL PASS`). What is not done is the
default: `--dsp 1` is off, the engine's sound is still made on the CPU, and the saving has not been measured in
a race on either side.

**2026-10-23, in a race** (Dolphin's DSP in **LLE**, `--dsp 1`, the `auto_grid` scene, the same build): the mixer
took **12.1 voices a sample** (26,848 jobs over the last 10 s), **no voice found no room, no run was stretched
here after all** — the driver path works end to end. The p50 was **25.96 ms** against the CPU mixer's 25.61 and
25.77 in the two runs beside it; LLE answered the slowest run 25 ms after its kick (the hardware's would be ~2 ms
of a 5.3 ms chunk at 12 voices, by the microcode's ~26 instructions a voice-sample), so 351 chunks waited
(0.4-0.6 ms at the longest) and those waits are the whole of the difference. On the console the wait should be
gone and the dry voices' stretch with it: the mixer is ~4% of the race-start frame (~1.0 ms) and the dry voices
are most of it, so **~0.5-0.8 ms is the theoretical gain**; whether it is heard is the listening test's. The
24-voice-per-run limit would be ~4.8 ms of the 5.3 ms window, so a heavier mix would need the fallback.

## What that says

* On these numbers 60 fps **at the heaviest moment** is not demonstrated: the items we can name and size reach 18-20 ms
  and the best case 17.6. It is reachable for most of a lap (the lighter stretches already are), with 30 fps dips at
  the start grid and in traffic; the pacer already chooses per frame.
* What would change it: (1) the console. Broadway is out of order and dual issue, Dolphin is one cycle an instruction:
  the real frame may be faster or slower, and a data-cache-bound frame would move N4 to the top. (2) S1 to S3 being
  larger than estimated (the only items that remove a whole PS2-shaped layer); the walker study (sizing, section 9)
  suggests the world's share is 3 ms at most, not the 3-5 ms the first handoff estimate had. (3) Differences nobody
  can see (the user allowed those on 2026-10-16) beyond N1-N2; quality cuts are ruled out.
* Order: the console run first (package: `work/console3`); then E1, E2, E4 (exact, medium, ~1-1.5 ms together),
  then S1 (the one big bet) with N1 beside it. (This order is the 2026-10-17 one; "The remaining avenues" below has
  the current one, from the console's own profile.)

## The remaining avenues, with the console's own numbers (2026-10-23)

`work/console7`'s PMC profile of a race frame is the only measurement from the hardware: **30-36 M cycles (41-50 ms),
15-18 M instructions, IPC 0.46-0.49**, 27.5 % of the cycles stalled on loads and 13.9 % on instruction fetches;
L1 I misses 277 k a frame, L2 D misses 90-140 k. The exchange rate for anything measured in Dolphin: instruction-bound
work costs **~2.1x** its Dolphin ms (1/IPC), a load that misses costs 30-100 cycles where Dolphin charges one, and the
GPU's work is hidden behind the CPU (which is the bottleneck). Percentages below are of the ~50 ms frame, so **2 % is
1 ms**.

| avenue | console evidence (share -> ms) | potential | exact? | effort / risk | what blocks it |
|---|---|---|---|---|---|
| **S1/S2** stop rebuilding the PS2 frame: world batches and cars handed to GX directly | `pc_cached` 3.0, `vif_feed_words`+`gif_path_bulk`+`packet_bulk`+`vif_defer_call_in` 3.6, `draw_setup_slow` 2.0, `dl_get` 1.4, `gs_backend_car` 1.7, `nw_lit_ps_quads` 2.0 (13.7 -> 6.9) | **2-4 ms** | yes, with the design's checks (`docs/design-s1.md`) | weeks; the only structural item of that size | the console run first (it re-ranks this above/below the memory work) |
| **Locked cache for the scratchpad** (N4) | `memcpy` 10.1 (5.0), of it `dma_spr` 4.8 (2.4) | **1-1.5 ms** | yes (the same bytes, in L1D) | medium; console-only (Dolphin cannot see it) | the scratchpad's mapping: the guest accesses are `GPTR`-masked today, so a locked region needs a cheaper address path than a branch per access |
| **The car shading on GX** (N2) | `nc_shade_ps_pairs` 7.4 (3.7) under `nc_shade_direct8` | **0.6-2 ms** | **no, visible** (the paint) | medium-large | GX has no reflection texgen, so the sphere-map UVs stay on the CPU; only the light term can move |
| **The DSP ADPCM decode** | `decode_at` 1.5 (0.75) | **0.6-0.75 ms** | no (audio, ~1-2 LSB) | large | the microcode decoder + the voice's `p1`/`p2` history across a switch back to the CPU |
| **E1 palettes placed like rasters** | `lazy_data` 1.4, `tex_rest` 0.9, `put_row` 0.7, `placed_shape` 0.7, `gs_load_clut` 0.5 (4.2 -> 2.1) | **0.6-1 ms** | yes (the `clutchk` check guards it) | medium; the "pink surfaces" risk class | a palette note carrying its CBP, the CLUT cache keyed by content without the CBP |
| **E2 collision loops native** | the segment tests' 0.85 ms a frame at the race start (Dolphin's count); `0x274C90` alone 0.1-0.4 here | **0.5-0.7 ms** | yes, with the `collc90 2` check | medium | the generator's override ABI (below), then the 0x274C90 native; `0x273400`/`0x273BF0` after |
| **E4 `0x1F3080` native** | 680 calls a frame, 0.2 ms in Dolphin | **0.4 ms** | yes, with a chain-bytes check | medium | the same override path (it waits on the VU0 condition, so the VU0 summary) |
| **E5 group set-up / texture bind** | `0x26B2F0`, `0x221A70` ~0.6 ms in Dolphin | **0.4-0.6 ms** | yes | medium | the same override path |
| **E3 box test / `vu0_vis` cheaper** | `box_core` 0.5, `vu0_box_world` 0.6, `vu0_vis` 0.8 (1.8 -> 0.9) | **0.2-0.3 ms** | yes (the MAC flags) | medium; needs paired singles done without fusing differently | runtime only (vu.c), no generator |
| **N5 recompiler context traffic** | 7.0 % of the recompiled code's time in Dolphin | **0.2-0.4 ms** | yes | large (the generator's context model) | the generator |
| **`-O3` vs `-Os` on the hot functions** | 277 k L1 I misses, 13.9 % fetch stalls | **-1 to +1 ms, unknown** | yes | small; console A/B `PS2_HOT_VARIANT=0/1/5` | the console (Dolphin cannot see the I-cache) |
| **Prefetch and cache hints** | shipped: memcpy source, `ext_source_ident`'s 16 samples + full re-reads, the clipper scan; left: `unpack_bulk` 1.5, `lazy_data` 1.4, `put_row` 0.7 | **0.2-0.5 ms more** | yes (hints) | small | the console to say which hints paid |
| **The clipper's own cost** | 0.35-2.5 ms in Dolphin at the race start; the console's misses will make it more | **0.5-1 ms saved by tuning** | yes | small | the console: `--clipevery 4` vs `8`, `--clipband 200` vs `100`; the fix itself stays |
| **EFB copies/restores** | `gs_vsync` 0.8; the GPU does the copies behind the CPU | **<0.4 ms** | yes | medium | not the bottleneck while the CPU is at 50 ms |

**What has already been done for the console this week** (so it is not counted twice): the DSP mixer on by default
there (~0.5-1 ms), the `dcbt` hints above (~0.5-1.5 ms), `dcbz`/the 120-frame re-read/the two-line parse entries
(~0.5-1.5 ms), the clipper (a correctness fix; its cost is the tuning row), `-O3` on the hot functions (the unknown
sign above). None of these changes Dolphin's ~25.6 ms; their shares are the console's.

**The arithmetic, plainly.** 30 fps is 33 ms and needs ~17 ms off the 50; the rows above that do not need a visible
trade or a new architecture sum to **7-12 ms** (S1, the locked cache, E1/E2/E3/E4/E5/N5, the DSP decode, hints,
clipper tuning), which lands at **38-43 ms - 25-27 fps**, with the car shading trade on top at **36-43 ms**. 60 fps is
16.7 ms = **12.1 M cycles**, and at IPC 0.5 that is **~6 M instructions against ~17 M today**: a 2.8x cut, which only
S1-S3 taken all the way plus the memory work can approach. On these numbers 60 fps at the race start is not
demonstrated; 30 fps in most scenes is.

**The order.**
1. **The console sitting** (`docs/console-test.md`): the warping A/B, the pattern, the DSP listening test - and a fresh
   PMC profile, because the 10-20 profile predates the clipper, the DSP, the cache work and the hints. It re-ranks
   everything above.
2. **The generator's override ABI, then E2's `0x274C90`**: the game ELF is now extractable from the disc
   (`work/SLUS_210.50.elf`) and the generator reproduces `gen/` byte for byte (74 s), so an override is a config line
   plus a regeneration. The one change needed first: the override summary (`tools/recomp/emit.py`) treats a native as
   reading only a0-a3/t0-t2/gp/sp/ra and clobbering only the GPR/FPU temps, not the VU0 registers; a collision native
   runs `ps2_vu0_callms`, so it needs a per-address VU0-clobbering summary (a config `OVERRIDE_VU0` set, its call
   sites flushing/reloading the VU0 registers as a `vu0call` does, and `prog.ind_summary` conservative while it is
   non-empty). Then the native + its `collc90 2` check, then `0x273400`/`0x273BF0`.
3. **The locked cache**: the largest single memory win left and the one no Dolphin run can value.
4. **S1** (the big bet), with E1/E3/N5 beside it.
5. The car shading only if the user calls the visible trade; the DSP decode after the DSP's default proves itself.

## 2026-10-24: the override ABI is wired, E2's `0x274C90` is native, and what the rest of E2/E4/E1 would be worth

* **Generator** (`tools/recomp/emit.py`, config `OVERRIDE_VU0`): a native listed there is summarised as clobbering the VU0
  registers (`OVERRIDE_VU0_SUMMARY`: reads none, may change any), so its call sites store what they hold in them and read
  back what is used after, as after a `vcallms`; through a pointer it makes `ind_summary` clobber them too. The generator
  prints, per call site, the VU0 state read afterwards - but only as its own conservative liveness (any later `vcallms`
  or call reads "everything"), so the contract is taken from `work/vu0live.py sites` (program-aware) and then checked.
  With the set empty `gen/` is byte for byte what it was (72 s).
* **`0x274C90`** (`hle_coll_c90` in `runtime/vu.c`, autopilot `collc90`: 0 recompiled, 1 native with its first 5000 calls
  checked, 2 every call checked, 3 native with the registers it does not leave poisoned): the segment-against-triangle
  call keeps its five qwords in locals and runs `seg_tri_in` (the 0x03D8 entry's operations); it leaves vf5.w, vf6, vf7,
  vf19.x, vf24, VI1, VI8 and Q. 100,000 calls of a race start checked against the recompiled function from the same
  state: 0 differ (24,133 hits); the only VU0 registers that differ are vf2, vf4, vf5.xyz, the ones `vu0live` says nothing
  reads; mode 3 (those NaN-poisoned) races as before. The check's first run found a real bug (the answer written to a
  local array, not vf24), so it earns its keep. Gain: the lower-98% mean 25.40 -> 25.32 ms (-0.08 ms, 0.3%; two runs
  each, inside the benchmark's noise, as the 199 calls x ~250 cycles estimate says).
* **Not done, and why.** `0x273BF0` (420 calls, 210k cycles) and `0x273400` (296 calls, 134k) are 900- and 500-instruction
  strip walkers whose hit paths convert u16 positions to floats and run six more VU0 macro operations; a native keeps
  the whole of that bit for bit for ~0.15 ms each in Dolphin's count (~0.3 ms on the console), and can only replace a
  function, not a loop inside it. `0x1F3080` (E4) is the render DMA queue's packet open (DMA kicks with polls, ten paths),
  100 instructions a call on the fast path: ~0.06 ms. E1 (`gs_load_clut`) is already cached by content, by TEX0 fields
  and by the buffer's generation (`g_clut_last`, `g_clut_cache`); what is left is the "pink surfaces" class for
  ~0.15 ms. E3's box test already decides from a sphere with exactness margins. None changes the class of the frame
  (25.5 ms; 16.7 needed); the console's own profile is what re-ranks them.

## 2026-10-24 (later): a pipeline model for the cache-model Dolphin, and the prefetch hints it showed to pay

* **The model** (`CacheSim.cpp`, `work/cachesim/README.md`, "The pipeline model"): the cache model's misses plus Broadway's issue
  width, unit latencies, no register moves between the integer and floating point files, store-to-load delay, mispredicts and
  **one L1 data line fill at a time**. The race start lands at 28-30 M cycles a frame at IPC 0.55 (the console's own: 30-36 M, IPC
  0.47), so the figures are the console's order of size, not its exact cost; changes between two builds are what to read, and the fill
  limit is an assumption (with two fills, `CACHESIM_PIPE=mlp=2`, the baseline is 4.8 % faster, the shipped hints -2.60 % instead of
  -1.62 % and the look-ahead +0.24 % instead of +1.40 % against them: the verdicts hold). The model's interpreter spins in the game's idle waits and counts that as
  work: totals must leave `plat_time_us`, `idle_sleep`, `plat_log_pending` and `plat_vblank` out (`pipeattr.py --exclude`).
* **What it says about the frame**: about 150 k line fills a frame (36.5 k code lines, 71 k data lines touched) are most of the stall;
  recompiled game code is 20 % of the instructions. With one fill at a time a prefetch only overlaps computation, so the lever is
  fewer lines touched, not earlier ones. Top costs a frame (kcycles): `nc_shade_ps_pairs` 2220, `memcpy` 1822, `pc_cached` 1476,
  `draw_setup_slow` 881 (23 k L1I misses), `clip_batch` 703, `gs_backend_car` 668.
* **Shipped** (hints, on only where `g_dcbz_on` is, i.e. not under Dolphin's JIT): touches ahead in `nc_shade_ps_pairs` and
  `nw_lit_ps_quads` (the loops that have work to do between fills), and in `vif_defer_call_in` the display list bucket and sample
  words of the batch two ahead (`gs_world_prefetch`). Modelled: `nc_shade_ps_pairs` -19 %, `nw_lit_ps_quads` -16 %, `dl_get` -49 %,
  the whole **-1.62 %**; the world draw hash is the same on all 300 frames. Plain Dolphin, alternated twice each: mean 25.42 / 25.45 ms
  with the hints in the build against 25.42 / 25.42 without (they are off there): no change.
* **Dropped**: a look-ahead along the render DMA chain (touching each CALL's parse cache entry, pool and packet words a few tags early).
  `pc_cached` -195 M cycles but `dma_run` +247 M, the scanner's own cold tag reads waiting behind its prefetches: +1.4 % against the
  build without it.
* **Not done**: a direct map of the guest's low 32 MB through a spare DBAT (the mask and base add around every guest load and store are
  2.5 % of the cycles; it needs the mirrors and the scratchpad mapped as well); locked-cache streaming (takes half the L1 data cache;
  unmodelled). A hot/cold split of `draw_setup_slow` was looked at and dropped: it executes about 985 instructions on each of its 472
  calls a frame, the 23 k instruction-cache misses are those lines being cold at every call (the per-draw loop's code is far more than
  32 KB), not a layout the compiler chose, so the lever is fewer instructions per call, not their order.
* **Three small exact ones afterwards** (2026-10-24, each by what the model counts for the function itself, as the totals move by less than
  the run to run noise of ~0.2 %: the same code's `draw_setup_slow` varies by 3 % in instructions between two runs; the world draw hash is
  `811C9DC5` on all benchmark frames of all three builds):
  * `tex_lookup`, a texture location of more than 128 blocks hashed block by block: the block numbers come from the walker the upload and
    mark paths use (`gs_bw_*`, one switch a row) instead of a `gs_block_of` call each (1500 a frame): that function's 10.1 M cycles over
    the window are 0.4 M.
  * `plat_time_us` without state or interrupt masking: floor(ticks * 4 / 243) as the high half of ticks * 4 times ceil(2^64 / 243) (exact
    below 2^57; checked against the division on 4 * 10^8 random and 8 * 10^7 edge values), instead of the incremental remainder under
    `IRQ_Disable` (two serializing MSR writes and a divide): 135 to 52 cycles a call in the model (idle-wait calls are in the mix).
  * `ps2_sqrtf`: two corrections `t + (x - t * t) * y / 2` of `x * y` from the 12 bit estimate (written as `fnmsub`: the compiler made
    `fneg` + `fmadd`) instead of two Newton steps and `x * y`: two dependent operations fewer, 83.7 to 71.1 cycles a call; the final
    +-1 check stays, so it is exact for an estimate off by up to 1/1024 (checked with a modelled estimate over 2.4 * 10^8 inputs, half of
    them squares of floats; the 750's is within 1/4096) and the start-up self-test now also runs squares of floats.
* **The CLUT copy waits until the buffer is read** (`gs_load_clut`, `clut_defer`, `gs_clut_sync`): a race frame has some 50 loads of a
  cached palette that is not what the buffer holds, each a 1 KB copy out of a cold cache entry (1700 L2 misses a frame through `memcpy`,
  230 through `memcmp`). What the game does with a palette is nearly always a TLUT the backend has, and `gs_clut_hash` already knows a
  whole load's hash without reading the entries; so the load only notes (slot serials, hash, `gs_clut_ver`) and the copy is made when
  the buffer is read (a new TLUT, the speed blur's ramp, `gs_texel_rgba`, a hash of a part) or before the next write. Whether the
  buffer changes is known by the hashes of whole loads (`clut_holds`), else compared, which ends at the first word that differs.
  Autopilot `clutlazy 0` copies at once as before; `clutlazy 2` runs both and logs a differing decision or buffer (none in the
  menus, the load and the race start of the benchmark; the world draw hash `811C9DC5` on all frames). Modelled over the 300 frames:
  `memcpy` 551 M -> 511 M cycles, `memcmp` 95 M -> 85 M, the total -0.65 % against the build before (the run to run noise is ~0.2 %);
  34 to 57 loads a frame wait, 13 to 18 of them are read later (TLUTs built).
* The PC target links again: `g_dcbz_on` (the hints' switch, `runtime/memfast.c`) is a weak definition in `vif.c`, as the 2026-10-24 hints
  commit had left it undefined there.
* **Looked at, no exact cut** (2026-10-24, same model run): the copies in `unpack_bulk` (a few hundred whole-qword UNPACKs a frame, ~12 k words,
  3600 L2 misses a frame) and `dma_spr` (1500) read guest RAM the game wrote long before (the source is cold by capacity, the destination
  is the scratchpad or VU memory and hot), and the copy is the only reader; skipping or reordering it needs the consumer to read the
  source in place, which is not exact for the VU memory the programs address. `gs_backend_car` (511 calls and ~680 k cycles a frame)
  spends its stalls in the six guest words it hashes into a packet key (5 cold lines a call): the same lines are read by the native
  render right after, so dropping the key words moves the misses, it does not remove them. `draw_setup_slow` is a thousand instructions a
  call, not a layout. What is left is the ~150 k line fills a frame; fewer needs the data or code touched per frame to shrink (a rewrite
  of a stage), or fills to overlap (one console number decides: the `membench` line in the boot log; at two fills the instruction
  side, 78 k L2 misses a frame, would gain from `dcbt` ahead on cold code, which the one-fill model says costs as much as it saves).
* **A stage rewrite that touches fewer lines: none is exact; the lead is the hardware** (2026-10-24). `pc_cached` is 1.48 M cycles a frame,
  944 k of them the validation loop: 17.6 k guest words a frame (21 a call, 822 calls), about 54 cycles a word, because each VIFcode
  word sits in a line of its own between the vertex data; fewer words is the packet check's own sampling made thinner (not exact) and
  there is no write tracking on guest RAM to replace the reads. The car shading reads 16 bytes a vertex of positions (62 % of its bytes;
  the normals are 4 already) and positions change in place (damage), so no copy of them can be trusted without reading them. With the
  lines per frame at their floor, what is left is overlapping them: (1) **HID4**: libogc's `L2Enhance` sets the L2's 64 byte fetch mode,
  dual castout buffers and a two deep miss under miss (the 750CL's own memory parallelism, which the model prices as `mlp=2`: -4.8 %), and
  the Homebrew Channel strips HID4 writes from a dol ("can't be enabled safely"), so it is the user's call and a console test; (2) the
  **locked cache DMA** as a bounce buffer for the cold bulk sources of `unpack_bulk`/`dma_spr`/`memcpy` (5 k L2 misses, ~2.3 % of a frame),
  at the price of half the L1 data cache (to be priced with a 16 KB L1D model run and a console `lcdma` micro-benchmark first).
  **Built: a read-only boot line** `cpu: HID0 .. HID1 .. HID2 .. HID4 .. L2CR ..` (`pmc.c`), which tells which of these the boot leaves
  on (Dolphin's own: HID0 0011C664, HID1 80000000, HID2 A0000000, HID4 83900000, L2CR 80000000).

* **The locked cache priced: the model says it costs 1.75 % of a frame, and the misses it could hide are worth about as much**
  (2026-10-24, race start, 300 frames, one fill at a time, the lazy-CLUT build). `CACHESIM_L1D="ways=4"` gives the model the 16 KB the
  locked cache leaves of the L1 data cache (4 of 8 ways, true LRU); `ways=8,lru=1` is the control (8 ways, true LRU), which lands on the
  8-way pseudo-LRU baseline (CPI 1.779 against 1.779). With 4 ways: +1.75 % cycles (9,602 M against 9,434 M, instructions -0.2 %, CPI
  1.814), L1 data misses 79 k -> 118-125 k a frame, of them loads served by the L2 10 k -> 39-42 k, castouts 45 k -> 66-70 k, L2 data
  misses unchanged. The cost is spread over some forty functions (`draw_setup_slow`, `memcpy`, `vif_feed_words`, ... each 1-5 %), so
  there is nothing to claw back. To break even the DMA has to hide 0.49 M cycles a frame, 3.7 k of the 131-cycle L2 misses; the bulk
  copy sources it could bounce (`unpack_bulk`, `dma_spr`, `memcpy`) are ~5 k misses, 0.66 M cycles, so a perfect overlap nets +0.6 %,
  and the one scattered pool (the 17.6 k `pc_cached` validation words) is already at ~54 cycles a word, less than a miss. Not built as
  a game feature. **Built: `--lcbench 1`** in args.txt (`pmc_lcbench`, `pmc.c`; off by default): the locked cache enabled for a few
  milliseconds at the start, then disabled, logging `lcbench:` lines on a console: that the engine works (a word kept, a block in from
  MEM2 and MEM1, a block out), a dependent load against one block waited for and 15 queued, the overlap with 3000 loop iterations and
  with 12 dependent cold loads, an 8 KB stream against reading the lines in place, and whether the DMA sees a line the core stored and
  did not flush, after `dcbst` and after `dcbf`. Under Dolphin the block in from MEM2 returns zeros (MEM1 and the block out work), so
  only a console can tell; note that libogc's `LCEnable` takes DBAT3 for 0xE0000000 and touches 32 KB from 0x80000000.
* **`--pfbench 1`** (`pmc_pfbench`, `pmc.c`; off by default, interrupts off for a few milliseconds at the start, the last 2 MB of the
  guest RAM, zeros after): the questions the model answers with an assumption, put to a console. (1) 1, 2, 4 and 8 cold lines read in a
  row, and the same with a `dcbt` of all of them first: if the per-line cost falls with the count the fills overlap (the model has one
  at a time, `mlp=1`; the `membench` line asks the same from a dependent chain). (2) a `dcbt`, 0 to 640 loop iterations of work, the
  load, against the work and the load: how much of a fill the work hides (what the prefetch hints, -1.62 %, rely on). (3) 128 lines
  of code (4 KB of nops, in the heap, MEM1 under Dolphin) run cold, cold in the L1 but warm in the L2 (run, `icbi`, run), after a
  `dcbt` of every line and a wait, and warm: what an L2 miss adds to an instruction fetch, and whether a data touch brings code to
  where the fetch finds it, which is the one cheap code prefetch there is (the instruction fetch is 19 % of a frame, 12.7 % of it L2
  misses, 36 k code lines a frame against an L2 of 8 k lines). Under Dolphin the probe runs to its end in 20 ms (its timings are the
  emulated timebase's, so they say nothing).
* **The runtime and platform files at `-Os` instead of `-O2`: no** (2026-10-24; the recompiled game is already `-Os`, its hot functions
  `-O3`). Many small hot functions miss the L1 instruction cache on nearly every call (`pc_cached` 120 times per code line a frame,
  `__GX_SetDirtyState` 107, `native_projection` 121, `memcpy` 124), so a smaller runtime looked worth a run (`work/os_build.sh`,
  `-Os` text 12.57 MB against 12.73 MB). Model, race-start benchmark, 300 frames: L1 instruction misses 284.6 k -> 249.2 k a frame
  (-12.5 %), fetch stall cycles 5.39 M -> 5.10 M (-5.4 %), but instructions 17.7 M -> 19.3 M (+9.0 %) and the pipeline model's cycles
  31.56 M -> 33.66 M (**+6.7 %**): `libgcc.a` +1.04 M cycles a frame (at `-Os` the compiler calls the library for what `-O2` expands in
  line, 64-bit shifts and the like), `iop_sd.o` +0.48 M, `gs_gx.o` +0.22 M, `vu.o` +0.19 M, no object a clear gain. The plain Dolphin
  (JIT, three alternating runs each, `work/jit_ab2.sh`) agrees and is worse: p50 25.33 -> 28.88 ms (+14.0 %), p75 26.79 -> 29.69
  (+10.8 %), p90 28.93 -> 30.60 (+5.8 %). The world draw hash `811C9DC5` was the same on all benchframe lines of both model runs. The
  code is not too big in the way the miss counts suggest: what a smaller footprint saves is a small part of what the instructions cost.
  `work/cachesim/byfile.py A.bin A.elf A.map B.bin B.elf B.map` totals two runs by object file (pipeline cycles and instructions a
  frame, then the stall model's misses).
* **What can be trusted without a console, done and not done** (2026-10-24). Done: `apply_state`'s three divisions by a power of two (a
  render target's `1 / 2^sh`, a texture's `1 / (16 * tw)` and `1 / (16 * th)`, tw and th 1..1024) are table lookups
  (`k_rcp_pow2`, `k_rcp16_pow2`; `gx_init` checks the tables against the divisions and logs only if they differ). The quotients are
  exact, so the draw is the same (world draw hash `811C9DC5` on all 29 frames of the benchmark, no check line); the three `fdivs` were
  8.5 k modelled cycles a frame (17 each, nothing else in the FPU meanwhile), less ~2 k for the `cntlzw` and the table loads: 0.02 % of a
  frame, under the run to run noise of the whole (draw_setup_slow's own total moved +1.5 % with its instruction count +1.6 %, path mix).
  Not done: the **direct DBAT map** of the guest RAM (the address is `base + (a & mask)`, 2 instructions; with the RAM at virtual 0 it would
  be 1) because a BAT block has to be aligned to its size, so the 32 MB would have to sit at a 32 MB boundary of MEM2 (0x12000000 is inside
  the top of MEM2 the IOS keeps), and whether DBAT3 is free on a console, as libogc's `LCEnable` assumes, is a console question; the
  **0x273400 callback record/replay** in `coll_pair_check` had never run with a hit (0 hits in 5000 checked calls, also while driving
  into a wall; 0x273BF0 has 4,500 hits in 20,000), and the first 3000 calls of a game (`collchk`) run through it. Built and run: the
  autopilot `collmut N` (debugging, 0 = off) replaces the segment of the next N checked 0x273400 calls by a ray from its start along
  one of the six axes, 1 to 2048 long, for both passes (the game goes on from what the ray did, so it is for a test run only). In
  600 calls (`coll400 2`, `collchk 100000`, driving into a wall) 24 hit, 38 callbacks were recorded by the variant's pass with the
  real callback and answered in the recompiled function's pass, in the order and with the arguments and memory the record has, and
  no call differed in a GPR, FPU register, VU0 register, the frame or the VU0 state something reads (no `variant differs` line;
  10 of the calls had one callback, 14 had two). The record holds 16 callbacks; a call with more would count as a mismatch, not a
  wrong answer. The audio functions at the top of the compute-bound list (`voice_stretch`, `decode_at`, `mix_f2s16`) run on the DSP on a
  console (the model's Dolphin runs them on the CPU), `vif_feed_words`, `clip_batch` and `vu0_vis` are the compute-bound natives left,
  with no cut found by reading them.
* **`clip_batch`'s depth scan touched one line in four** (2026-10-24). Its `dcbt` of the vertices 8 ahead (written on 2026-10-23, before
  the pipeline model) was issued every 8 vertices, 128 bytes of 16 byte vertices, so a line in four was early and the other three
  were read cold: one instruction, the first `fmuls` after the position loads, was 12.7 % of the function (89 k cycles over the run, 41
  a vertex). Every second vertex now (each 32 byte line once; the hints change no result). Modelled, 300 frames: `clip_batch` 210.3 M
  -> 200.5 M cycles (-4.6 %, instructions +0.8 %), that instruction 89 k -> 42.6 k; the whole moves -0.23 %, inside the ~0.2 % noise;
  the world draw hash `811C9DC5` on all 29 benchmark frames. The rest of its stall is the first eight vertices of a scan (nothing to
  overlap them with) and the fills themselves (one at a time). The other stalls that sit on one instruction are cold reads whose
  address is not known earlier (the DMA chain's tags in `dma_run`, 95 cycles a tag; `dl_get`'s key; `pc_cached`'s validation words):
  all of them wait on fills one after another in the model, so only a console that overlaps fills (`pfbench`, `membench`) makes a
  `dcbt` ahead pay there.
* **What a write barrier on the recompiled stores costs** (2026-10-25; the price only, nothing reads the marks). Part of those cold reads
  only check that guest memory did not change (`pc_cached` 1297 k stall cycles a frame, `ext_source_ident` 378 k, `memcmp` 221 k, `dl_get`
  184 k, `gs_block_ident` 124 k: 2.2 M of a 31.5 M cycle frame, 7 % at most, and only if the draw does not read the same lines right
  after). Write tracking would replace them. `-DPS2_TRACK=1|2` (`runtime/ps2_recomp.h`, off by default: the default build is the same
  binary but for its build time stamp) makes `SW`/`SWF`/`SH`/`SB`, which every guest store in `gen/` goes through (143 k sites), also
  store a byte into `g_wdirty[(a & mask) >> 12]` (8 KB): 1 a constant, 2 the byte `g_wtick`. Model, race start, 300 frames, world draw hash
  `811C9DC5` on all 29 benchmark frames of both:

  | a frame | HEAD | `=1` | `=2` |
  |---|---|---|---|
  | text | 12.73 MB | 14.06 MB (+10.5 %) | 14.17 MB (+11.3 %) |
  | instructions, recompiled game code | 4.17 M | 4.57 M (+402 k, +9.6 %) | 4.60 M (+430 k, +10.3 %) |
  | cycles, recompiled game code | 7.89 M | 8.50 M (+610 k, +7.7 %) | 8.66 M (+774 k, +9.8 %) |
  | L1 instruction misses, recompiled game code | 96.0 k | 108.5 k (+13.1 %) | 109.1 k (+13.7 %) |
  | L2 instruction misses, recompiled game code | 45.8 k | 52.3 k (+14 %) | 54.8 k (+20 %) |
  | instructions, whole | 17.72 M | 18.11 M (+2.18 %) | 18.09 M (+2.08 %) |
  | cycles, whole | 31.49 M | 32.56 M (+3.40 %) | 32.19 M (+2.22 %) |

  About 2.3 / 2.5 instructions a store site. The runtime's and libraries' instructions do not move (-0.03 % / -0.4 %) but their cycles do
  (+339 k / -58 k; `libogc.a`, which is the same code, +119 k / -31 k: code placement against the 8 way cache), so the whole frame's
  figure carries about +-1 % of layout noise and the recompiled code's own +1.9 to +2.5 % of the frame is the cleaner one. Most of it is
  code size (L1 instruction misses +13 % in the game code), not the stores' instructions (+2.1 %). Against up to 7 % to win, so a net
  gain would be 3 to 5 % at best, less if the draw reads those lines anyway. Not built further. If it is: the recompiler could mark
  once per base register and page in a basic block instead of once a store (not tried), and the stack is not to be assumed untracked
  (the game DMAs from small packets it builds on its stack). Before any of it, the ceiling is worth one run: a debug switch that trusts
  the parse cache and the ident checks without reading, in the benchmark (the picture stays the same while the world is static).
* **The ceiling of that: -3.7 % of the frame (about -4.3 % with the checks off all window), so the barrier would net +0.3 to +2 % at
  the very best** (2026-10-25). Autopilot `trust N` (debug; bits: 1 `pc_cached` takes a kept packet without reading its words, 2
  `ext_source_ident` takes a source seen before for the same by the samples it kept and the full re-reads stop, 4 `dl_get` finds the
  block's list without the five sampled words; `gs.h`). Model, race start, 300 frames, `trust 7` from 1 s into the window (the entries
  are those made before: autopilot time 176 landed at boot clock 181.2 s, the window began at 180.15 s; 86 % of the window is under
  it), against the same binary with trust 0 (9,452.7 M cycles, within 0.06 % of the build before it). World draw hash `811C9DC5` on all
  29 frames of both. A frame: cycles 31.51 M -> 30.34 M (-3.72 %), instructions 17.74 M -> 17.52 M (-1.26 %), L1 data loads 93.6 k ->
  85.9 k, L2 data misses 89.1 k -> 82.1 k. By function over the window (M cycles): `pc_cached` 450.9 -> 206.6 (-244), `ext_source_ident`
  74.5 -> 19.4 (-55), `ext_ident` 39.0 -> 9.2 (-30): 329 of the 351 M, 74 % of it the parse cache's validation. Two moved instead of going:
  `dl_get` 81.5 -> 99.8 (+18: the sampled words were what its list walk waited behind; now the cold list entries take the stall, and
  `gs_backend_world` went the other way, -16, so bit 4 nets about nothing), `gs_backend_car` 213.0 -> 226.5 (+13: its key words' lines
  were warmed by the car packets' validation). Against the barrier's +2.2 to +3.4 %, before the reads of the dirty table and with a
  stale-geometry risk: **not built**. What is left of it is cheap and only a console can price it: the validation words' addresses are
  all in the pool before the compare, so a `dcbt` pass over them first lets a machine that overlaps fills (`pfbench`) hide part of the
  ~944 k cycles a frame (the model has one fill at a time and cannot show it).
* **The world path's repeated per-batch work: a ceiling of -0.1 % of the frame, not built** (2026-10-25). Autopilot `wskip N` (debug,
  off; `gs.h`, `gs_backend_world`): bit 1 lets a world batch after the first of a call (`vif_defer_call_in` sets `g_world_rep`) take the
  first's GS state and target and leave out `gs_draw_state`, `draw_setup`, `rt_drawn`, `batch_flush` and, when that batch went the
  native way too, `native_mode`, `native_projection`, `native_scroll` and `native_camera` (the cull, the list, the arrays, `clip_batch`
  and the draw stay). Nothing writes a GS register or a VU1 row between the batches of one call, so these only ever find their
  inputs unchanged. **Exact, checked in the run:** bit 2 does all of that work and counts the runs after which the GX FIFO holds
  anything (`GX_Flush` writes libogc's pending state and pads with 32 zero bytes, the PI write pointer moves in whole bursts, so
  the bytes between the pointers of two flushes are zero if nothing was written). Race start, 300 frames, ~139 such batches of ~620
  a frame: 0 writes in 55.7 k draw setups, 40.1 k mode+projection and 31.7 k scroll+camera runs (cumulative from the switch);
  the control, bit 4 (every batch, the first of a call included): 125 of 595 draw setups a frame wrote, 34 of 394, 31 of 329, so
  the check does see a write. (A digest of the FIFO bytes compared between two runs does not work: the display lists' addresses,
  the `&15` vertex-cache invalidation and `clip_every` all follow `g_frame_no`, so two runs of one build differ.) **The number:**
  model, same binary, `wskip 0` against `wskip 1` from before the window, world draw hash `811C9DC5` on all 29 frames of both:
  instructions 5,323.5 M -> 5,312.7 M (-10.8 M, -0.20 %), the skipped functions' cycles `draw_setup` -2.5 M, `native_camera` -2.2,
  `native_projection` -2.1, `rt_drawn` -1.0, `native_mode` -0.4, `gs_draw_state` -0.4, `native_scroll` -0.4: -9.0 M of 9,456 M
  cycles (-30 k a frame, 0.04 ms, -0.095 %); the whole total moved +13.7 M (+0.15 %), all of it in functions the switch does not
  touch (`dl_get` +9.2, `ext_ident` +5.2, `vif_defer_call_in` +3.8: the frame-number phase and layout noise of ~0.2 %). The
  most any compare-and-skip of this kind could take is those seven functions' whole window, 134.7 M cycles = 1.42 % (cars included,
  and a version counter to know "unchanged" without comparing costs part of it): the first batches of calls change state one time in
  five and need the work. **Not built.** (The same run with `trust 7` from 170 s never reached the benchmark: trust that early
  breaks the race's load; 176 s, after the race has loaded, is where the earlier `trust` runs started.)
* **How many of the L1 instruction misses are capacity: 70 % at 64 KB, 82 % at 128 KB, and nearly all of the backend's** (2026-10-25).
  `CACHESIM_L1I="ways=N"` (128 sets, so 16 ways = 64 KB, 32 = 128 KB; `ways=8,lru=1` the control), race-start benchmark, the HEAD build of
  2026-10-24 (`pfb.dol`), three runs (`run_l1i.ps1`): a frame, L1 instruction misses 294 k -> 90 k (-69.6 %) -> 54 k (-81.8 %), L2
  instruction misses 80.7 k -> 66.2 k -> 52.7 k, instruction-fetch stall (model constants I 6/52) 5.49 M -> 3.59 M -> 2.75 M cycles, the
  whole frame 31.56 M -> 29.80 M (-5.58 %) -> 28.97 M (-8.23 %) with the instruction count unchanged (-0.01 %). By where the code is
  (L1 instruction misses a frame, 32 KB -> 64 KB -> 128 KB): runtime + platform 167.4 k -> 22.0 k -> 10.7 k (-87 %, -94 %), libogc and
  libm 25.7 k -> 2.8 k -> 1.3 k (-89 %, -95 %), the recompiled game 97.3 k -> 61.8 k -> 38.9 k (-37 %, -60 %). Biggest single objects
  (32 KB -> 64 KB): `gs_gx.o` 68.9 k -> 6.2 k, `gs.o` 24.6 k -> 4.6 k, `vif.o` 20.8 k -> 1.3 k, `hle_render.o` 10.6 k -> 0.8 k,
  `nw_car.o` 10.3 k -> 0.3 k, `memfast.o` 6.5 k -> 0.6 k; `gen/recomp_006.o` only 21.2 k -> 9.3 k. So the per-batch draw path (parse,
  state, lists, clip, GX) is what just overflows 32 KB: it is 66 % of all the misses, and 87 to 94 % of those go away when the cache
  is twice or four times the size; the game's own code is first touches mostly (a frame touches ~36 k distinct code lines). Two
  limits: the sets stay 128 as the ways grow, so "capacity" here is capacity plus the conflicts more ways remove (a layout change can
  only win the conflict part), and the figures are the model's one-fill-at-a-time ones (an I miss costs 6 cycles from the L2, 52 from
  memory). The ceiling for any footprint cut of the draw path is therefore the 64 KB row, -5.6 % of the frame, before the code that
  would have to be removed costs anything.
* **The per-batch draw path's code: what it is, how hot, and what a smaller footprint is worth** (2026-10-25; model, race start,
  32 KB L1I run of 2026-10-24 `pfb.dol`; `work/cachesim/hotcold.py`: misses by pc, the sample of 1 in 256 of the instructions executed,
  the code lines the window touched). The backend functions called >= 50 times a frame in `gs_gx.o`, `gs.o`, `vif.o`, `hle_render.o`,
  `nw_car.o`, `nw_world.o`, `memfast.o`, `hle.o`, `vu.o` and libogc: 182 KB of code, 128 KB of it run in the window (58 KB never), 90 % of
  the instructions run in 71 KB of lines and 99 % in 102 KB; they take 171 k of the 294 k L1 instruction misses a frame (58 %). Hot
  lines are dense (85-90 % of a hot line's 8 instruction slots run), so moving cold instructions out of the lines frees at most 10-15 %
  of the hot lines. By misses a frame (m/c = misses a call): `draw_setup_slow` 24.3 k (8 % of all; 16.7 KB, 440 calls, 1,071
  instructions and 55 misses a call, a flat 150-line hot path with no loop), `gs_backend_car` 8.4 k (11.9 KB, 61 % never run),
  `gs_load_clut` 7.6 k (19 m/c), `pc_cached` 6.9 k, `vif_feed_words` 5.1 k, `__GX_SetDirtyState` 4.9 k, `vif_defer_call_in` 4.7 k,
  `clip_batch` 4.4 k (10.6 KB, hot 3.8 KB), `vif_start_micro` 3.6 k, `gs_backend_sync` 3.6 k, `unpack_bulk` 3.4 k, `native_projection`
  3.2 k, `gif_xgkick` 3.0 k, `gs_backend_world` 2.8 k (9.8 KB, 62 % never run, hot core 1.8 KB): the first ten are 25 % of all of them.
  By object (L1I a frame): `gs_gx.o` 67 k (83 KB, hot90 27 KB), `gs.o` 24 k (34 KB, 13 KB), libogc 23 k (9 KB, 7 KB), `vif.o` 19 k (14 KB,
  4 KB), `nw_car.o` 10 k, `hle_render.o` 9.6 k, `memfast.o` 6.5 k, `vu.o` 6.5 k. The exchange rate, the same benchmark with a bigger
  L1I (a frame, L1I misses / cycles): 32 KB 296 k / 31.56 M; 40 KB 185 k (-37 %) / 30.84 M (-2.3 %); 48 KB 128 k (-57 %) / 30.37 M
  (-3.8 %); 64 KB 90 k (-70 %) / 29.80 M (-5.6 %); 128 KB 54 k (-82 %) / 28.97 M (-8.2 %): about 0.29 % of the frame for each KB of
  the hot set that stops competing, at first, if a cut of the hot lines counts like cache (it does not exactly: the cut lines must be
  among those the loop re-fetches). Not built.
* **`draw_setup_slow` read through: what a typical call runs, and what a fast path for the common change would skip** (2026-10-25;
  model, race start, 32 KB L1I run `pfb.dol`; `work/cachesim/fncalls.py`, `fnsections.py`, `fnann.py`, `fnmiss.py`: its calls, its
  instructions by address range from the 1-in-256 sample, its misses by range). It is 17 KB: `rt_get` + `rt_find`, `rt_bind`'s check,
  `same_state`, all of `apply_state`, and `tex_lookup` with `gs_block_ident`, `gs_block_of`, `gs_placed_*` and `tex_decode` inlined; the
  only calls out on a typical call are the GX register setters, `rt_for_texture`, `gs_backend_sync`, `tlut_load`/`tlut_lookup`, `memcpy`
  (`g_cur.d = *d`) and `memcmp` (the texture object). 595 draw setups a frame, 440 take the slow path (the other 155 have the same
  `seq`), and **430 of those 440 apply a state** (memcpy 430, `rt_for_texture` 455): "seq changed, state equal" is 2 %, so a skip for
  that is worth nothing. A call is ~1,071 instructions, 55 L1I misses and ~2,040 cycles itself (899 k cycles a frame, 2.85 %), plus
  ~0.4 M cycles in the GX setters and lookups it calls. By part (instructions a call / L1I misses a call / share of its cycles, the
  cycles being instructions + 10.4 x misses, scaled to the measured total): save and restore of 18 registers 57 / 0.6 / 4 %; target
  lookup, hold, bind check, pending-clear check, dispatch, g_cur copy 109 / 4.8 / 10 %; `same_state` (reached by 337 calls) 38 / 2 / 4 %;
  `apply_state` decode (frame mask, alpha test, blend) 114 / 7 / 11 %; K colours, TEV key, pixel-state keys and compares 124 / 11 / 14 %;
  target placement + scissor + g_cur tail 101 / 2 / 7 %; TEV stage emits (295 of 430 calls) 69 / 7 / 8 %; the texture 460 / 19 / 39 % (43 %
  of the instructions; setup and object load 128, the 4-entry memo scan and its 209 hits a frame, and the 134 misses a frame that run the inlined lookup:
  a page-version hash loop of ~15 iterations x 25 instructions each, 50 k instructions a frame). **Nothing changes by one field**: the
  GX setters' calls a frame say the texture object reloads on 79 % of the applies, the TEV stages on 69 %, blend mode 52 %, K colour 1
  (the FIX alpha) 49 %, colour/alpha update 40 %, Z mode 36 %, alpha compare 14 %, scissor 13 %, position matrix 14 %; and the first
  field `same_state` finds different (337 calls reach it, 103 more have another target) is TEST in ~56 %, ALPHA 9 %, TEX0's CLUT half
  25 %, the rest a few %. FRAME, ZBUF, the scissor, FOGCOL, PRIM's bits and FBA never differ when the target is the same. So the
  "common change" is a new material (test, blend, texture, CLUT) on the same target, and a fast path for it can skip only the target
  group: placement, scissor, frame-mask and Z decode, FOGCOL's K colour, the g_cur tail, ~100 instructions a call on 77 % of the calls:
  ~75 k cycles a frame, **-0.2 % of the frame**, before its own compare. The big masses are elsewhere: the texture memo (miss path
  ~0.18 M cycles a frame with its callees = 0.57 %, a hit an estimated ~330 cycles against ~1,340 for a miss, so at most -0.43 % if every miss
  became a hit; a memo keyed by the texture's own pages instead of the global `gs_ver` could keep entries across unrelated GS writes,
  and how many of the misses are such stale ones is not known) and the TEV re-emit (295 calls x 9 setters = 2.4 k libogc calls and
  ~0.2 M cycles a frame with the inlined side, 0.64 %; the stage set depends on the 8-bit TEV key alone, so a per-key table of the
  commands is the exact shape, but libogc's shadow registers make that a direct-write job, see the direct-writes ceiling). The
  function is cold every call (55 misses on ~134 executed lines), so what is skipped saves its lines' fetches too; the estimate above
  counts them at 10.4 cycles. Not built.
* **The texture memo's misses: 97 % are entries gone stale that would have come to the same texture** (2026-10-25; model run of the
  race-start benchmark, `-DMEMOSTAT` build `build-memo`, debug and compiled out otherwise: the code of `gs_gx.c` without it is
  byte-identical to HEAD's; `work/cachesim/run_memo.ps1`). Over the 300 frames, a frame: 349 lookups, 205 hits (58.7 %), 144 misses
  (41.3 %). The misses by what they are, with the key (TEX0, TEXA, TEX1, CLAMP) kept in a table of the 4,096 last used and a bit set of
  all keys ever seen (about 5,000 keys in the window, each used ~21 times): **new 1.3 a frame (0.9 %), old (back after more than the table)
  0.2, capacity 0, stale 142.7 (99.0 %)**. Capacity 0 means a bigger memo with the same global versions would not turn one miss into a hit.
  A stale entry has the key with other versions: `gs_ver` and `gs_clut_ver` both 85.7 % of them, `gs_clut_ver` alone 14.1 %, `gs_ver`
  alone and the texture generation (g_tmemo_gen) never. **Of the stale ones 97.5 % (139 a frame, 96.5 % of all misses) came out of the
  lookup with exactly the texture, content hash, TLUT and TLUT content the entry held**; 3.6 a frame were really different. The
  versions are what kills the entries: `gs_clut_ver` changes 133 times a frame (each paletted texture loads its own palette), `gs_ver`
  14 times, so a key used in the last frame is stale on both. The cost of a miss (the inlined lookup, its page-version hash loop, `tlut_lookup`
  and the callees) is ~0.18 M cycles a frame for ~134-144 misses = ~1,340 each, a hit ~330 (both from instruction counts), so a memo
  that told the texture's own pages and palette entries unchanged instead of the global counters would save 139 x (1,340 - the
  validated hit's cost): **-0.37 % of the frame if a validated hit costs ~500 cycles, -0.59 % if it were free**. Unproven part: the
  validation. It must hold what `tex_lookup` and `tlut_lookup` depend on: the texture's GS pages (the lookup's own `loc_changed` page-version
  check), the CLUT entries it uses (`gs_clut_hash(base, entries)`), TEXA for the formats it enters, what `gs_placed_for` and `gs_upload_for`
  report, and the generation; and it keeps their side effects (`used` stamps, counters). Not built.
* **What a texture-memo entry must check to be exact, and what each check costs** (2026-10-25; read only, no code changed: `tex_lookup`,
  `tlut_lookup`, `tex_pages_ver`, `loc_changed`, `gs_placed_for`, `gs_upload_for`, `gs_place`, `mark_rect`, `gs_backend_sync` and `apply_state`'s
  memo). "Exact" is measured in pixels: a hit has to give the texture object the miss would give, or one with the same texels and the same TLUT
  entries (the miss may find another `Tex` of equal content, as the memo's hits do today). The miss is a function of eight things; the entry has
  to hold what tells each of them unchanged. Costs are instruction counts from reading the code, not measurements.

  | # | the result depends on | what changes it | the entry holds, the hit checks | cost of the check |
  |---|---|---|---|---|
  | 1 | the state: TEX0 (base, width, format, size, CPSM, CSA), TEXA, TEX1 (filter), CLAMP (wrap) | the game's register writes | the four words, as now | 4 compares, ~12 instructions; a hashed slot instead of the scan of four ~+8 |
  | 2 | no texture or TLUT given up since | `tex_evict`, `tex_cache_clear`, a TLUT slot reused in `tlut_lookup` bump `g_tmemo_gen` | `gen == g_tmemo_gen` | 3, a hot global |
  | 3 | the texels in GS memory: pending upload data, finished uploads, soft draws, transfers, render-target readbacks (`gs_backend_sync` runs first, as now) | all of them call `mark_rect`: `gs_page_ver[page] = ++gs_ver` (a transfer at its start, `mark_rect(..., 0)` in `start_transfer`; a lazy upload's blocks get that same version only when its data is in, so the page version is the stamp that is there at once) | **for an entry whose lookup did not take the placed path** (a placed texture's content is guest RAM, no page of GS memory is it): `loc_changed`'s test on the texture's blocks (pages first, then the blocks of the pages written since) from the `gs_ver` the entry was made at (inserted only if the lookups wrote nothing, as now). It is the predicate the lookup's own "known" path makes, so it is exactly as exact as the lookup | ~15 instructions from the hot `gs_page_ver` (2 KB) and `gs_blk_ver`; the benchmark's non-placed textures cover 1 page and 2-16 blocks (299 a window are over 128 blocks: `tex_pages_ver`'s form) |
  | 4 | what the game *placed* at TBP without an upload | `gs_place` sets a record (entry, ident, `ver = ++gs_ver`) and **marks no page and no block**; it holds while `gs_blk_ver[tbp] < p->ver`; `gs_placed_load` re-versions it. A placement over a texture whose entry is page-valid changes the result and no page version | the record's **ident** the entry was made with (none: not placed); the hit looks at the record now (`gs_placed_for` inline) and needs the same: both placed with that ident, or both not. The lookup's result on this path is a function of the ident alone (`lh = mix(mix(pid, tbw), ...)`), so this is exact. Not the record's `ver`: the game places the same texture again every frame, so `ver` is new each time (it keeps 4,051 of 39,004 placed entries, the ident 37,610) | 2 loads (`g_placed[tbp]`, 12 B of a 192 KB table; `gs_blk_ver[tbp]`, a 16 k-entry table) + ~12 |
  | 5 | a pending upload of exactly this texture (`gs_upload_for`: owner of the first block, same shape, all blocks still its) | its start marks the pages (row 3); `ur_materialize` marks none and changes no texel | covered by 3; `gs_upload_for` is a pure read and is not needed | 0 |
  | 6 | for CI formats the palette: the TLUT slot `tlut_lookup` finds, a function of (base, entries, CPSM, shift 0, TEXA when CPSM) and the **content** `gs_clut_hash(base, entries)` | CLUT loads. **A version cannot stand for it**: `gs_clut_ver` changes 133 times a frame because each paletted texture loads its own palette into the same entries, so palette A is written over by B and back (A, B, A); a stamp says changed where the content is the same, and that is the 97.5 % of the stale misses | the hash the entry was made with; the hit needs the same now | O(1), ~15, when one load put exactly those entries (`clut_holds`/`g_clut_at`); else it folds the n entries after `gs_clut_sync` (16: ~100; 256: ~1,000) and **forces the copy the lazy CLUT change left undone** (1,700 L2 misses a frame): then the hit gives up and the miss runs (how often: to count) |
  | 7 | PSMT4HL/HH are decoded through the CLUT: the key holds `gs_clut_ver`, so each version is another `Tex` | CLUT changes | `clut_ver == gs_clut_ver`, as today, for these two formats | 3 |
  | 8 | every other non-paletted format depends on no CLUT | - | nothing (today's memo requires `clut_ver` for all of them: the 14.1 % "clut_ver alone" class) | 0 |

  A hit must still do what the lookups did that is not the result: `t->used` and `tls->used` (the eviction order), `tlut_load(tl, n)` (it has its
  own `g_gxc.tlut` cache) and restore `to`. It does not repeat the lookup's `TexLoc` writes (`l->tbp = ~0` on a placed or upload path,
  `checked_ver` on the block path): the entry's making did them, and a later real lookup finds an older `checked_ver` and re-checks, which is correct
  and not free. The `g_lk_*` counters are the status line's.

  The checks add ~70-90 instructions (~+120 cycles) to a current hit, so a validated hit is ~450-500 cycles against the miss's ~1,340 and today's
  4-entry hit's ~330, and an entry not used for a frame or two is also three cold lines (about 100 B; 131 cycles each from memory, 12 from the
  L2), 100-400 cycles more. The unknowns were four counts, taken under `MEMOSTAT` before any code (next bullet). **To build it with its check**:
  autopilot `tmemo 0|1|2` (off, on, on with every hit also run through `tex_lookup` + `tlut_lookup` and compared by texel bytes and TLUT bytes, a
  mismatch logged with the row that should have caught it: the pattern of `collc90 2` and `clutlazy 2`), over the menus, the load and the
  benchmark, the world draw hash `811C9DC5`; then a model run at 256 and 512 entries, since the model prices the cold lines.
* **The texture memo's four counters, taken: 96.6 % of the stale misses would have been kept by an exact validation, with no violation**
  (2026-10-25; `-DMEMOSTAT` build, `work/cachesim/run_memo.ps1`, the race-start benchmark, 299 frames, a second run with the refined checks;
  totals, a frame is a 299th; run to run the lookups differ by ~1.5 %: 106,509 in the run before). Lookups 104,196 (348 a frame), hits
  60,503 (202), misses 43,693 (146) = new 404 + old 30 + capacity 0 + **stale 43,259 (145)**. **(a)** The checks in the order cheapest first:
  generation the same 43,192 (67 fail), entry made by lookups that wrote nothing 43,192, placed state the same (a placed entry: the same
  ident; any other: no record now) and, for the entries not placed, the blocks unchanged 41,798, `clut_ver` the same for PSMT4HL/HH 41,798,
  palette hash the same 41,798, `gs_clut_hash` answering from a load's hash 41,798: **41,798 = 139.8 a frame, 96.6 % of the stale ones and
  99.9 % of the 41,838 that came out to the same texture and TLUT** (the other 40 are those whose texture or TLUT was given up in between). The check was exact each time: of the 41,798 the
  lookup came to the entry's own texture and TLUT in all 41,798 (another object with the same texels 0, **different 0**). Placed entries
  (39,004 with a clean entry) have the ident the same in 37,610 (96.4 %) but the record's version the same in only 4,051 (10.4 %: the game
  places the same texture again each frame), the record gone in 0; the other 4,188 entries have their pages unchanged in 4,188 and their blocks
  in 4,188 (100 %). The first run checked the pages of every entry and kept 7,879 (18 %): a placed texture's pages say nothing about it, they
  are written by palette and other transfers. **(b)** The path of the 43,693 misses: placed 39,505 (90.4 %), known 3,883, blocks 305, upload 0,
  hashed 0, and **2 decodes in the 299 frames**: every miss is a lookup that finds an existing texture; convertible: placed 37,610 of 39,071
  stale with an entry (96.3 %), known 3,883 of 3,883, blocks 305 of 305. Formats (lookups / misses / convertible): CT32 14,108 / 4,188 / 4,188,
  T8 60,156 / 17,195 / 16,116, T4 29,633 / 22,011 / 21,195 (it misses 74 % of its lookups), T8H 299 / 299 / 299; no CT24, CT16, PSMT4HL/HH or
  other. **(c)** Reuse of the convertible ones: lookups since the key's last use <4 4,147, <16 2,785, <64 311, <256 376, <1k 26,298, <4k 7,620,
  more 261 (63 % come back after 256-1,024 lookups, 0.7-3 frames of 348); distinct keys used since <4 7,243, <128 2,152, <256 16,984, <512
  15,359, more 60: **a 256-entry table holds 63 %, 512 entries 99.9 %** (~50 KB); frames since (the port's counter, 2 a game frame in a 30
  fps race) 0: 7,274, 1: 2,119, 2: 16,918, 3-4: 12,243, 5-8: 2,945, more 299. The earlier "keys come back after ~14 frames, hundreds to
  thousands of entries" was wrong (it came from 21 uses a key over 300 frames, an average over a skewed set). Pages: the entries that are not
  placed are 1 page (3,889) or over 16 (299), 2-4 blocks (3,491), 9-16 (398), over 128 (299); placed entries have none. PSMT4HL/HH: none in
  this scene (0 lookups; other scenes are unmeasured, so `clut_ver` stays the check for them). **(d)** `gs_clut_hash` answered from a load's
  hash at the check in 90,088 of 90,088 paletted lookups (100 %). **What it changes**: the ceiling is 139.8 a frame x (1,340 - a validated
  hit's 500..900 cycles) = 61-117 k cycles = **-0.19 to -0.37 % of the 31.5 M frame** (-0.59 % if a hit were free); where in that range it
  falls is the cold lines of a ~50 KB table reused after 1-3 frames (a frame fills 150 k lines), which only a model run of a build can price.
  The design follows the measurement: placed entries by ident, the others by their blocks, the palette by content, a 512-entry table. Not
  built. (MEMOSTAT's tables are 480 KB of MEM1 bss; the arena had 536 KB left at the end of the run.)
* **The validated texture memo, built as a prototype and run in the model: exact, -0.35 % of the frame at 1,024 entries, a loss at 256** (2026-10-25;
  autopilot `tmemo 0|1|2` and `tmemon N`, `gs_gx.c` `tm2_*`, `gs.c` `gs_clut_hash_cheap_h`; `build-tm`, `work/cachesim/run_tm.ps1`, the files
  `auto_bench_tm_{c0,chk,t256,t512,t1024}.txt`; one binary, so the control is the same dol with the switch at 0; the race-start benchmark, 300
  frames). **Off by default** (`g_tmemo2 = 0`); with `-DNO_TMEMO2` the code is out and `gs_gx.o` and `gs.o` disassemble the same as HEAD's (checked
  with objdump), and with it in but off the cost is one load and branch per texture draw. The design is the table in "What a texture-memo entry must
  check": a second level behind the four entries, N entries in sets of four hashed by the key (TEX0, TEXA, TEX1, CLAMP), 96 B each (key; gen, page
  version, placed ident, palette hash, `Tex`/`Tlut` pointers, flags; the texture object), probed after `gs_backend_sync`; a hit needs the same
  generation, the same placed ident (or none), for an entry that is not placed the texture's pages unchanged since it was made or last checked
  (`tex_pages_ver`, a superset of `loc_changed`), and for a paletted texture the palette's content hash, taken only when one load put exactly those entries
  there (`gs_clut_hash_cheap_h`; else it is not used and the lookups run), `gs_clut_ver` for PSMT4HL/HH. It is entered only when the lookups wrote nothing
  and took no upload path, and not when the texture has over 16 pages. **Exactness (mode 2)**: every use is also run through `tex_lookup` +
  `tlut_lookup` and compared (texture object, texels, TLUT entries), from boot through the menus and the load and over the benchmark: 31,933 uses in the 300
  frames, **all the same `Tex` and TLUT, 0 mismatches since boot**; the world draw hash `811C9DC5` is on every `benchframe` line (29 of 29) of every run.
  **Cycles** (a 300-frame total; `pipeattr.py --diff` against the control; "memo functions" = the sum of the changes in `draw_setup_slow`,
  `tlut_lookup`, `GX_InitTexObj`/`LOD`/`CI`, `gs_clut_hash`, `gs_clut_hash_cheap_h`, `tex_pages_ver`, `gs_upload_for`, `gs_placed_for`, `GX_LoadTlut`,
  `tlut_load`, `memcmp`; negative = faster; the whole total moves ~0.2 % run to run, the sum is the figure to read):

  | entries | probes (a frame 144) used | no entry | whole total | memo functions |
  |---|---|---|---|---|
  | 256 (24 KB) | 18,871 (63 a frame) | 23,872 | -0.03 % | +7.9 M = **+0.084 %** (a loss) |
  | 512 (48 KB) | 32,335 (108) | 10,191 | -0.22 % | -9.2 M = -0.098 % |
  | 1,024 (96 KB + 4 KB tags) | 38,534 (128) | 3,759 | -0.41 % | -33.0 M = **-0.349 %** (-110 k cycles a frame) |

  The sizes order as the counters said, with the hits fewer than the 99.9 % of a fully associative table: the 4-way sets overflow at ~400 live keys
  (the counters measured LRU distance), so 1,024 is the size that holds ~89 % of the probes, and 512 loses half of what it could win to conflicts. At
  1,024 a used probe saves ~850 cycles net, inside the -0.19..-0.37 % the counters allowed (-0.35 %); the whole-run -0.41 % is at the edge of the noise
  and the same direction. **What the prototype pays that a refined one need not**: `draw_setup_slow` grew from 17 to 20 KB because the compiler
  inlined `tm2_find`, `tm2_insert` and `tm2_compare` into it (`pcattr.py --diff`, 512: +1,368 L1I misses a frame, +161 L1D load, +206 L2D, +34 k
  stall cycles a frame in that function alone, about half of what the other functions save); `g_tm2` and its tags are only 8-byte aligned, so each 96 B
  entry spans four 32 B lines and not three. A model run died once (the first 256-entry run was cut at clock 758 after 355 s, another project's Dolphin
  session; kept as `tmt256_cut_pc.log`, rerun clean). The refinement is the next bullet.
* **The texture memo refined: `find` stays inline, `insert` and `compare` go out of line, the table is 32-byte aligned; -0.352 % at 1,024 entries, exact**
  (2026-10-25; builds `build-tm` = the prototype, `build-tm2` = all three helpers `noinline`, `build-tm3` = the kept form; `run_tm.ps1 -Build B -Prefix P
  -Runs ...`, each build with its own control, the same dol with `tmemo 0`; control -> 1,024 entries, 300 frames, the same function sum as above).

  | build | `draw_setup_slow` | memo functions | whole total |
  |---|---|---|---|
  | prototype (all inlined, 8-byte aligned) | 20.0 KB | -0.349 % (-110 k a frame) | -0.41 % |
  | `find`, `insert`, `compare` all `noinline`, aligned | 18.0 KB | **-0.154 %** (-48 k) | -0.08 % |
  | **`find` inline, `insert` + `compare` `noinline`, aligned (kept)** | 18.6 KB | **-0.352 %** (-111 k) | -0.50 % |

  Out of line, `tm2_find` is a 752-byte function that misses ~16 of its ~24 code lines on each of the 144 probes a frame (L1I +2,282 a frame, +91.8 k
  stall cycles a frame in the function, +13.6 k in `insert`): the draw path is already past the 32 KB L1I and the inlined hit path shares lines with the lookup
  it replaces (`draw_setup_slow` + `find` + `insert` +1,614 L1I misses a frame out of line against +432 inlined). So the loss was `find` out of line;
  `insert` (36 calls a frame) and `compare` (mode 2 only) out of line cost nothing, and the alignment is not visible above the noise (kept: it is
  free and the entry then takes three lines, not four). The kept build at 1,024 entries: 38,949 of 43,248 probes used (no entry 3,328, placed record
  957), `draw_setup_slow` -19.6 k stall cycles a frame with its L1I misses flat (+6), `tlut_lookup` -28.1 k, `GX_InitTexObjLOD` -8.2 k, `tm2_insert`
  +13.9 k, `gs_clut_hash_cheap_h` +4.2 k, `tex_pages_ver` +2.7 k. **Exact again on the kept binary**: check mode (at the default size then, 512 entries; the
  size changes only the set index, every use still compares the whole key) 32,366 uses, all the same `Tex` and TLUT, 0 mismatches since boot, hash
  `811C9DC5` on 29 of 29 frames; the 1,024 run has the same hash on 29 of 29. The default size is now 1,024 (`g_tmemo2_n`); the switch itself is still
  **off by default** (`g_tmemo2 = 0`, one load and a branch per texture draw; `-DNO_TMEMO2` still disassembles the same as HEAD). Cost of turning it on: 96 KB +
  4 KB of MEM1 bss, ~3.5 KB of text, ~128 uses a frame touching 3-4 lines of the table each (~16 KB of lines a frame, inside the measured figure). Three
  more model runs of the new builds were cut short by the same outside Dolphin kills (a control after 104 s, a check run after 429 s): rerun each time, the
  cut logs are `*_cut_pc.log`.
* **The texture memo is on by default** (2026-10-25, the user: "how would it work on console ... if beneficial then may turn it on if you think"; `g_tmemo2 = 1`,
  1,024 entries; args.txt **`--tmemo 0`** turns it off and `--tmemo 2` is check mode, the autopilot's `tmemo 0|1|2` as before). The basis: check mode at 1,024
  entries over scenes other than the benchmark, plain Dolphin (`work/cachesim/run_tms.ps1`, `auto_tms_*.txt`, the status block's line "gx: texture memo checks so
  far", totals since boot): a longer 1P drive 4,656,229 uses (60 fps, the car stuck against a wall after the first minute: the counts of draws and textures stop
  changing), the 2P split-screen race 4,364,682 (30 fps, 13.7 k draws a frame), the car select screen 1,006,504, and three more race starts with the track row
  pressed once, twice, three times (7,139 draws / 237 textures, 7,169 / 251 twice: three different scenes, the fourth press landed on the third's) 2,761,433 +
  2,447,514 + 2,848,609: **every use the same `Tex` and TLUT as the full lookup, 0 mismatches, ~18 million uses**, no out-of-memory or fatal line. Not checked: the menus
  after a race, other cars, a console. The default-on binary against its own control (`tmemo 0`), the standard benchmark file with no memo line: hash `811C9DC5` on
  29 of 29 frames, 38,520 of 43,245 probes used, memo functions **-0.266 %** (-84 k cycles a frame; the same code measured -0.352 % in the run before, the
  prototype -0.349 %: the figure is **-0.27..-0.35 %**, about 0.1-0.15 ms of the console's ~40 ms race-start frame), whole total -0.13 % (inside the noise). The console
  view: the saving is mostly instructions (~59 k a frame, ~460 per use), which does not depend on cache luck; the table's lines are priced in the model, whose miss
  costs were calibrated to the console's self-test; if the console overlaps line fills both the table's and the skipped lookup's cold lines get cheaper, the net
  stays close; 100 KB of bss and 3.5 KB of text against ~4.4 MB free (the sound memo backed out earlier was 1.1 MB for 0.15 %). A console A/B (`--tmemo 0` against
  the default) will not show 0.3 % through the PMC lines' noise: the decision rests on exactness and the model.

## Handover (2026-10-25): where things stand, the design to build next, every open task

This file is the log and its sections above hold the measurements; this section is the map. The assistant's own notes on workflow pitfalls are in
`C:\Users\sherv\.claude\projects\C--Users-sherv-Desktop-Projects-burnout3-Wii\memory\` (`MEMORY.md` is the index; they are not in the repository).

### State

* **Repository**: branch `wii-port`, private GitHub `shervinemp/burnout3-Wii`, tree clean at the last commit of this file. In git: `runtime/`,
  `platform/`, `tools/`, `config/`, `docs/`, `external/`, the two makefiles. **Not in git** (`.gitignore`: `work/`, `build*/`, `gen/`): every
  benchmark script and scene (`work/fb*.sh`, `work/auto_grid.txt`), the game's ELF (`work/SLUS_210.50.elf`), the cache-model's tools and every run
  (`work/cachesim/`), the Dolphin patch (`work/cachesim/dolphin-cachesim.patch`) and the generated sources. They exist on this machine only: copy
  them if the work moves. The patched cache-model Dolphin: source `F:\dolphin-src`, build `C:\dolphin-build`, rebuild notes in
  `work/cachesim/README.md` ("Build it again"); the test SD image is `D:/b3wii-dolphin/user/Load/WiiSD.raw` (`tools/wii/fatcat.py --put` writes
  files into it).
* **What the port is**: Burnout 3 (PS2) statically recompiled to C (`tools/recomp/` -> `gen/`, 143 k guest store sites), with the PS2 hardware in
  `runtime/` (VIF/VU/GS/IOP, `gs.c`, `vif.c`, `vu.c`, `iop_sd.c`) and the Wii side in `platform/wii/` (the GX backend `gs_gx.c`, native renderers
  `nw_*`/`nc_*`, the DSP mixer, `pmc.c`). The target is a **real Wii** (Broadway 729 MHz, 32 KB L1 I and D, 256 KB L2); Dolphin is for debugging.
* **Where it stands**: the heaviest stretch (race start) is 25.4-25.6 ms in plain Dolphin (60 fps needs 16.7) and 31.5 M cycles / 17.7 M
  instructions a frame in the cache + pipeline model; the console's own PMC profile (`work/console7`, older code) was 30-36 M cycles, IPC 0.47. Lighter
  stretches already hold 60. Shipped: the walker natives, `-O3` on the hot functions, the world clipper, the DSP mixer (default on a console), the
  `dcbt` hints, the lazy CLUT copy, `0x274C90` native, reciprocal tables, three small exact ones, widescreen, the race-load re-step (6.3 -> 3.4 s).
  Built and **off by default / debug only**: `--lcbench 1`, `--pfbench 1`, `-DMEMOSTAT`, `-DPS2_TRACK`, autopilot `trust`, `wskip`, `collmut`,
  `clutlazy 2`, `collc90 1..3`, `gxflags`. The validated texture memo is **on by default** since 2026-10-25 (`--tmemo 0` / autopilot `tmemo 0` off, `tmemo 2` check mode, `tmemon N` entries).
* **No console until ~2026-10-27.** Its boot log has five things this file is waiting for: the `cpu:` line (HID0/1/2/4, L2CR), the two `bat:` lines (IBAT0-3 and
  DBAT0-3, read only, added 2026-10-25: is a data BAT free for a direct map of the guest RAM; Dolphin shows DBAT0 `80001FFF 00000002`, DBAT1 `C0001FFF 0000002A`,
  DBAT2-3 empty, which says nothing about a console), `membench`, `lcbench:` and `pfbench:` (args.txt `--lcbench 1`, `--pfbench 1`; package `build-wii/hbc`,
  staged with the `bat:` lines 2026-10-25). The user sends `run.log`.

### The rules the user has set (keep them)

* Exact or nothing: no quality cuts, no lowered refresh, no reused shading; "if accurate 3 cycles is still 3". Verify **inside one binary** with a switch
  (the build changes timing; the AI is randomised by it, so cross-build pixel compares are not meaningful); the world draw hash `811C9DC5` over the
  benchmark's frames is the check that the picture is the same.
* The target is the console: weigh the code and data a frame touches (cache fills) above Dolphin milliseconds, and read the model's **per-function**
  numbers (the whole frame moves ~0.2 % run to run). The cache-model Dolphin's "console" profiles are the model's, not the hardware's.
* **Numbers before builds**: the user asks "tell me the numbers before building anything" and answers "yes" to each next step by name. Keep percentiles in
  benchmarks. Do not touch `HID4` (libogc's `L2Enhance`; the Homebrew Channel strips it) without an explicit go-ahead.
* No debug aids in the picture by default (the frame meter is off); announce the debug switches that change anything the user will see.

### Recipes

* **Build** (the msys2 shell is needed): `C:\devkitPro\msys2\usr\bin\bash.exe -lc 'cd /c/Users/sherv/Desktop/Projects/burnout3-Wii && export DEVKITPRO=/opt/devkitpro && make -f Makefile.wii -j24 PYTHON=/c/Users/sherv/AppData/Local/Programs/Python/Python313/python.exe'`
  (a second output directory: `OUT=build-memo XDEFS=-DMEMOSTAT`). devkitPPC / libogc 3.1.0. `build-wii/burnout3.dol` and `.elf`; copy the dol before a
  run, other sessions rebuild it.
* **The model run** (the race start, 300 frames, ~8 min): `work/cachesim/run_bench.ps1 TAG AUTOFILE DOL` (autopilot `work/cachesim/auto_bench_inl_on.txt`
  for the usual one), run in the background and wait for the notification; it writes `TAG_pc.log`, `TAG_pc.bin`, `TAG_pc.bin.pipe`. **Never overlap two
  Dolphin runs** (they share the profile and the SD image). `run_memo.ps1` is the MEMOSTAT one; `run_l1i.ps1` the bigger-L1I ones (`CACHESIM_L1I="ways=N"`),
  `run_l1.ps1`/`CACHESIM_L1D="ways=4"` the half L1D, `CACHESIM_PIPE=mlp=2` two fills.
* **Reading a run** (all `python -I`, all in `work/cachesim/`): `pipeattr.py` (cycles by function, `--exclude` the idle waits `plat_time_us`, `idle_sleep`,
  `plat_log_pending`, `plat_vblank`), `pcattr.py` (misses by pc), `byfile.py A B` (two runs by object), `hotcold.py` (hot/cold lines), `fncalls.py`
  (callees with calls a frame), `fnsections.py` (a function cut at its calls), `fnann.py RUN ELF LO HI --min N` (disassembly with executed a frame),
  `fnmiss.py RUN NAME:LO-HI ...` (instructions and L1I/L2I by range). The `.bin` has a record per pc and a 1-in-256 instruction sample.
* **Plain Dolphin timing** (JIT, p50/p75/p90 of the same 300 frames): `work/fb.sh` / `work/fb1.sh`; the scene is `work/auto_grid.txt`.
* **Traps that cost hours**: the MEM1 `.bss` of debug tables must stay under ~0.4 MB (a 1.4 MB table left a 52 KB arena and Dolphin exited at clock ~880
  with no message; the `mem:` line's "arenas MEM1" is the margin); a model run's totals include the game's idle waits unless excluded; a build's code
  placement moves other functions by up to +-1 % (judge by the function you changed); `dcbz`/hints are off under Dolphin's JIT (`g_dcbz_on`); nested
  heredocs hang and backslash escapes in heredocs get mangled (write files with the editor tool); `ps | grep | kill` kills its own shell; the PC
  `make` needs the msys2 shell; the test SD's `args.txt` decides the benchmark's aspect (4:3) and the autopilot file is copied in by `run_bench.ps1`.

### The validated texture memo: built, refined, on by default (2026-10-25)

**Status**: **shipped, on by default** (`g_tmemo2 = 1`, 1,024 entries = 96 KB + 4 KB tags of MEM1 bss; `--tmemo 0` in args.txt is the kill switch). Exact in check
mode (0 mismatches over ~18 million uses in six scenes besides the benchmark's 32 k, hash `811C9DC5` in every run); in the model -0.27..-0.35 % of the frame
(-84..-111 k cycles a frame) at 1,024 entries, -0.10 % at 512, +0.08 % (a loss) at 256. The bullets "...built as a prototype", "...refined" and "...is on by
default" in the log above have the numbers; `find` must stay inline (out of line it halves the gain). **Left**: a console look at the pictures (`--tmemo 0` is the
comparison), check mode over the menus after a race and other cars. The text below is the design as it was written before the build; the code follows it.

The counters (the bullet before this section) say the checks are exact on the benchmark and the ceiling is -0.19 to -0.37 %. The design is the
table in "What a texture-memo entry must check": a hashed table of N entries (4-way; 256 and 512 to try: 512 holds 99.9 % of the reuse, 256
63 %), each entry = key (TEX0, the used bits of TEXA, TEX1, CLAMP), `gen`, `ver` and the block list (entries that are not placed only: one page and
2-16 blocks in the benchmark; a texture of more than 128 blocks uses `tex_pages_ver`'s form), the placed record's ident (0: not placed), the
palette's content hash with its base and entry count (CI), `clut_ver` (PSMT4HL/HH only), `t`, `tls`, `n`, `to`. `apply_state` (`gs_gx.c`, the
`g_tmemo` scan before `tex_lookup`) probes it after `gs_backend_sync`, checks rows 2-7 in the order cheapest first (gen, placed ident, palette
hash, blocks), and on a pass does the hit's side effects. On a miss it runs the lookups as now and inserts when `gs_ver` and the palette's hash did
not change during them. Switch `tmemo 0|1|2` in the autopilot (2 = check mode), the 0 path byte-identical to HEAD (compare disassembly, as
`MEMOSTAT` was). The edge to check by reading again before writing: every writer of `gs_clut[]` and of `gs_page_ver`/`gs_blk_ver` (the audit that
makes rows 3 and 6 true), and the `TexLoc` invalidations on the placed and upload paths. Decide by a model run (per-function numbers for
`draw_setup_slow` and `tex_lookup`; the whole frame is inside the noise for anything this size). The code to read first: `ms_note` in `gs_gx.c`
under `MEMOSTAT` already computes every check on the stale misses and compares the result.

### Every remaining task and possibility

**Waiting for the console** (`docs/console-test.md` is the sitting's list):

| item | what decides it | state |
|---|---|---|
| the four boot-log lines | `membench` and `pfbench` say whether line fills overlap (`mlp`, the model's one-at-a-time assumption), whether a `dcbt` of code lines helps; `lcbench` whether the locked-cache DMA works at all | built, staged in `build-wii/hbc` |
| a fresh PMC profile | the one profile is older than the clipper, DSP, hints and lazy CLUT: it re-ranks everything | build has `pmc.c`; run a race |
| `HID4` (L2 fetch mode, dual castout buffers, miss under miss) | the model prices it at -4.8 % (`mlp=2`); the `cpu:` line says what the boot already leaves on | **the user's call**; libogc `L2Enhance`; the HBC strips HID4 writes from a dol |
| the clipper A/B, `--clipevery 4` vs `8`, `--clipband 200` vs `100` | the warping fix and its cost (0.35-2.5 ms in Dolphin) | built; the correctness is the console's to see |
| the DSP mixer listening test (`--dsp 1/0`), MEM2 DMA | ~0.5-0.8 ms theoretical; fixed-point mix likely inaudible; stage 2 reads SPU RAM | MEM2 DMA unverified on the console |
| `PS2_HOT_VARIANT=0/1/5` | `-O3` vs `-Os` on the hot functions is -1..+1 ms, unknowable in Dolphin | `-O3` ships |
| the hints' A/B on a console | which `dcbt` hints paid (-1.62 % modelled with one fill) | no on-console A/B harness for the hints is built (a run-time switch would do) |

**Code candidates, all exact** (ceilings are of the 31.5 M-cycle model frame unless in ms; none is built unless it says so):

| item | evidence / ceiling | notes |
|---|---|---|
| validated texture memo | **shipped, on by default** (`--tmemo 0` off): -0.27..-0.35 % at 1,024 entries (-0.10 % at 512, +0.08 % at 256), 0 mismatches in ~18 M checked uses | `find` must stay inline (out of line it halves the gain); the console's pictures are the one thing unchecked |
| TEV stage re-emit as a per-key table | 295 calls x 9 setters a frame, ~0.2 M cycles = 0.64 % with its inlined side | libogc's shadow registers make it a direct-FIFO-write job; `gx-direct-writes` ceiling: GX is 2.0 % of a frame, direct writes cap ~0.5 ms |
| target-group fast path in `draw_setup_slow` | -0.2 % (~100 instructions a call on 77 % of 440 calls) | "seq changed, state equal" is only 2 %: no skip for it; "nothing changes by one field" |
| the draw path's code footprint (`gs_gx.o` 83 KB, `gs.o`, `vif.o`, `hle_render.o`, `nw_car.o`: 182 KB, 128 KB executed) | a 64 KB-equivalent L1I is -5.6 % (70 % of the misses are capacity); dense hot lines free at most 10-15 % | no code found to remove; the lever is fewer instructions per call |
| `trust` (stop re-reading to validate packets, idents, list words) | -3.72 % with `trust 7` from 1 s into the window; 74 % is `pc_cached`'s validation | stale-geometry risk; the barrier below would only net +0.3..+2 %; a `dcbt` pass over the validation words is the cheap part and a console must price it |
| write barrier on recompiled stores (`-DPS2_TRACK=1|2`) | price +2.2..+3.4 % (+11 % text) against up to 7 % to win | built as a price only; not built further |
| `0x273BF0` (420 calls) and `0x273400` (296) natives | ~0.15 ms each in Dolphin (~0.3 on the console) | strip walkers, a native replaces a function and keeps the arithmetic; `collmut` and the callback record exist |
| E1 palettes placed like rasters | 0.15-1 ms | the "pink surfaces" risk class; `clutchk` guards |
| E3 box test / `vu0_vis`; E4 `0x1F3080` (~0.06 ms); E5 group set-up / bind (~0.6 ms of game code); N5 recompiler context traffic (0.1-0.4 ms); N6 stack traffic | small each; N5/N6 need the generator | override ABI exists (`OVERRIDE_VU0`) |
| S1-S3: world batches, cars, particles handed to GX without rebuilding the PS2 frame | 2-4 ms console estimate; the only structural item that size | `docs/design-s1.md`; weeks; S1a alone 0.1-0.15 ms |
| DSP ADPCM decode (`decode_at` 0.75 ms) | not exact (1-2 LSB) | needs the history across a switch back to the CPU |
| locked cache as a bounce buffer | costs +1.75 % (half the L1D); a perfect overlap nets +0.6 % | not built as a feature; `lcbench` decides |
| a direct DBAT map of the guest RAM | the mask + base add around guest loads/stores is 2.5 % of the cycles | needs 32 MB at a 32 MB boundary of MEM2 and DBAT3 free: a console question |
| car shading on GX's lights (N2) | 0.6-2 ms | **visible** (the paint): only if the user calls the trade |
| EFB copies/restores (`gs_vsync` 0.8) | <0.4 ms | the GPU does them behind the CPU |

**Other open threads**: 60 fps split-screen (the user hopes for it; the 2P race needs port 2 and a second DS2O copy, `auto_mp2.txt`; per-view post and
reflection fixes are in, what is left is in the memory note `split-screen-setup`); the speed blur is the PS2's three zooms as an 8-tap pass (a half-size
sum is built: `gxflags2 268435456` is the old); the 16:9 follow-the-Wii-setting path (`viWidth 704`) is ported and needs a console look; the load time
(`loadstep`/`loadnap`/`loadfe`) could still shrink; the PC port (`burnout3-pc`) and the Xbox port were evaluated and give little (a load-time idea,
latency notes, the VU replay-bench method).

### Looked at and left (do not redo without a new reason)

A hot/cold split of `draw_setup_slow` (the misses are the lines cold at each call, not layout); runtime and platform at `-Os` (+6.7 % cycles, +14 % in
Dolphin); a look-ahead along the render DMA chain (+1.4 %); a bigger texture memo with the global versions (capacity 0: it does not help); a skip for
"seq changed, state equal" (2 % of the calls); `wskip` (-0.095 %, proven exact, not built); copies in `unpack_bulk`/`dma_spr` (the copy is the only
reader of a cold source); dropping `gs_backend_car`'s key words (moves the misses); the segment-test natives beyond `0x274C90` (small); trusting `gs_clut_ver` as the
palette's stamp in a memo (A, B, A).

### Cleanup when convenient

`build-memo/` and `build-os/` (48 MB) in the repository folder, the `.dol`/`.elf`/`_pc.bin` copies and `dss_*`/`pa_*` text in `work/cachesim/` (gitignored,
disk only); `D:` is exFAT with 128 KB clusters, so many small files cost real space.
