# Handoff: Burnout 3 PS2 → Wii port

Written 2026-10-12 for an agent with no prior context, last brought up to
date on 2026-10-23. Read section 0 first, then the rest of this
file as you need it, then `docs/native-renderer.md`, which covers how the
renderer works.

## 0. Pick up here (state on 2026-10-23)

Sections 1 to 4 are the standing rules, the environment, the architecture
and how to build and test. Section 5 is the history, newest work last.
Section 6 is the task list with the details behind every item named here;
**its first item, "G", is the 60 fps gap and the ideas big enough for it
(2026-10-16); then "R", the smaller pieces left after 2026-10-15.**

### Where things stand

- **2026-10-15 (commit e15f1a9):** a round of exact speedups, all behind one
  autopilot switch (`newopt 0` = the old ways). The race-start benchmark
  (`work/fb.sh`) went from 26.87 ms of work a frame to 25.73 (p90 30.5 to
  28.7 ms, p99 33.6 to 30.8), after the benchmark's own counters were
  taken out of its mode (27.84 before that: they were not the game's
  cost). Details in section 5, "Done on 2026-10-15"; what is left in
  section 6, item R. The user's rules this time: exact only ("make the
  optimizations as best as you can"); pixel comparisons between builds do
  not work past the race's first seconds because the AI is randomized by
  timing (the user: "I'm certain ai is different"), so check in one binary.
- **2026-10-15, later (same commit):** the collision code's VU0 tests (item R.1): 0x273070's sphere tests
  decided as misses by the routine's own logic, natives for the segment tests; -1.1% of the mean frame, -1.6% at
  the median, behind `vu0coll 0`. New recompiler option `VCALLMS_SITES`. Section 5, "Done later on 2026-10-15".
- **2026-10-16 (night):** the parse cache answers in place, the VIF debug ring only with `nlog 1`, the lit instanced
  props' colours kept across frames: -1.1% of the mean frame, -1.3% at the median (24.99-25.03 ms), plus -0.38% from
  the props, behind `opt3 0`. A sound decode memo was exact but backed out (1.1 MB of MEM2 for ~0.15%). Section 5,
  "Done on 2026-10-16".
- **2026-10-16, later:** the lit world's colour sums in paired singles (bit for bit the scalar loop, checked in
  every run), `lit_floor`'s last table remembered: `opt3` as a whole is now -1.9% (24.81 ms against 25.28).
- **2026-10-16, then:** a console test package (section 4, "On a console"), for the first run on a real Wii:
  the benchmark and a profile on the card, no timed presses. Waiting for the user to run it.
- **2026-10-16, last:** DMA tags with nothing for the VIF skip it; the three structural items measured (section 5,
  "Done on 2026-10-16": none is large; the recompiler's upper halves well under 1%, the DSP not bit-exact).
  `opt3` as a whole: 25.30 -> 24.77 ms (-2.1%).
- **2026-10-16, after that:** the texture lookups, `apply_state` and the GS path measured line by line: nothing left
  there over ~0.3% that is cheap and exact (section 5). `draw_setup`'s slow part split off (for the console). The
  next real information is the console run.
- **2026-10-16, then (the user allowed differences that cannot be seen):** car lighting on GX's lights weighed and not
  built; the sphere map's scale from frsqrte's estimate (UV bytes one step off in 0.39%, -0.17%); the car loop two
  vertices a register, exact (-1.0%). The race-start benchmark is ~24.48 ms now (25.30 at the session's start).
- **The 60 fps gap and the ideas big enough for it: section 6, item G (read it first).** The known pieces add up to
  ~2 ms of the ~8 needed; the ideas of that size are the static world as replayed display lists, the DSP for the
  sound, GPU registers written directly, the compiler inlining across game functions, and the locked cache on the
  console. Waiting for the user's console run (section 4, "On a console").
- **2026-10-22/23: the hardware-only warping's fix, the console profile's cache work, and the DSP mixer.**
  - The warping: the port never reproduced the PS2 GS's drawing-area clip, so the Wii's GPU interpolated the world's
    texture coordinates over triangles that reach far outside the screen (and over triangles cut at the near plane,
    whose cut vertices land thousands of screens away). `platform/wii/clip.h` and `clip_batch()` in `gs_gx.c` cut
    those triangles on the CPU against the five planes through the eye, in the world space, where every attribute is
    affine: the picture is mathematically unchanged and only the triangle the GPU interpolates over gets smaller. On
    by default (`--clip 0` off; `--clipband N` how far outside the screen a triangle may reach; `--clipevery N`).
    `tools/clip/cliptest.c` proves the exactness on 40000 triangles; in Dolphin the race-start frame cut against not
    cut is the same scene (no hole at the sides, the bottom or the near plane); the console run is the judge. Cost:
    1-1.5 ms a frame at the race start, 5 ms in the intro cameras (Dolphin's count). Details:
    `docs/next-steps.md` "The clipper, built", `docs/bug-and-performance-analysis.md` B2 (B1 is subsumed).
  - The console profile (`work/console7`) says the frame waits on memory (41% of the cycles). Built from it: dcbz
    for whole lines in memcpy/memset and in the lit colour arrays (off under Dolphin, whose JIT has no cache model),
    the parse cache's entries two cache lines, unchanged texture sources read every `--reread` frames (120), and the
    phase timer accounted by part (`ptstat 1`'s "pmc by part").
  - The DSP mixer, stage 1 (`tools/dsp/dspmix.s`, `platform/wii/dsp_mix.c`, `--dsp 1`, off by default): the voices
    heard dry only are stretched and summed on the DSP. The test DOL (`tools/dsp/build.sh dspmix dspmix_test`) is
    ALL PASS under Dolphin's DSP in LLE: 78 runs, 78 answered, every sum equal to the microcode's model, within
    2.15 LSB of the CPU mixer. In a race (LLE, `auto_grid`, 2026-10-23) it took 12.1 voices a sample with no
    fallbacks; its p50 was 25.96 ms against the CPU's 25.61/25.77 because LLE answers a run in up to 25 ms and 351
    chunks waited (0.4-0.6 ms at the longest) - the console has no such wait (the hardware's run is ~2 ms of the
    5.3 ms chunk), so the estimate there is ~0.5-0.8 ms, to be heard.
  - The calibration pattern (`gxflags2 536870912`) draws now (the port's untextured Tev recipe; its `calib.rgba`
    EFB readback is broken). It is the console's own measurement of the BP's fixed-point interpolation: draw it,
    screenshot the frame, compare the two halves' ramps (`work/ramp.py`). Dolphin's Software Renderer is not that
    reference: it dies at frame ~140 in this build, and its rasteriser interpolates in float like the HLE.
  - The package: `TEST=1` is what puts `auto.txt` on the card now (a normal package must not pause at the race
    start); a formatted memory card is shipped when `work/saves` has one; the card's writes are flushed when an
    operation ends, and the card says why it could not be opened.
  - 2026-10-23, the small optimizations: `-O3` on the hot recompiled functions is the default (`PS2_HOT_VARIANT=1`,
    +57 KB of text; MEM1 has 4.9 MB free), and the clipper's look was made cheaper with the same cuts (the depth
    scan computes W alone, `cl_tri_bad` is inlined, the class pass is skipped unless a vertex is behind the eye).
    The race start measured 26.14 -> 25.61 and 25.77 ms p50 and 27.14 -> 26.83 and 27.11 p75 on the same scene
    (`work/fb1.sh`, two runs; the numbers in `docs/plan-60fps.md`). The look's bookkeeping was then checked
    against the old one (a temporary `CLIP_CHECK` build, every batch looked at): it found that a triangle the look
    cut in a batch that also crosses the near plane stayed in the strips as well as its clipped fan (drawn twice,
    the fan covering it); fixed, and 0 of 46,008 batches differed after.
  - 2026-10-23, console-directed (after `work/console7`'s PMC profile: 30-36 M cycles a frame, IPC 0.49, 27.5 %
    load stalls, `memcpy` 10.1 %): the **DSP mixer is on by default on a console** (off under Dolphin's
    `/dev/dolphin`; `--dsp 0` off, `--dsp 1` on there), and **`dcbt` prefetches** were added where the profile's
    misses are - the texture source check's 16 scattered samples (`ext_source_ident`, 2.7 %) and the copy loop's
    source two lines ahead (`memcpy`, 10.1 %, `dma_spr` half of it). The wet voices and the ADPCM decode are
    *not* free: the DSP's data memory 0x800-0xFFF is exactly full (a second sum per core has nowhere to go), and
    the decode would have to carry the voice's decoder history across a switch back to the CPU. The reasoning and
    the exchange rates are in `docs/next-steps.md`, "The console's own profile".

- The game boots, goes through its menus and races, with sound. In the
  scripted race (`work/auto_fh.txt`) it holds 30 fps with 38 to 48% of the
  processor idle; to hold 60 in traffic it lacks 1 to 1.5 ms a frame. The
  game's speed is right at 30 and at 60 (`pace_passes`).
- Sound was built on 2026-10-13: the chip's mixer (`runtime/iop_sd.c`,
  floats and paired singles), the engine's filter made where the chip is
  mixed (`runtime/hle_audio.c`), output through the audio interface's DMA
  (`platform/wii/plat_wii.c`). It costs about 4.5% of the processor in a
  race.
- No debug aid is drawn into the picture (the frame meter is off: the user
  asked). The log still gets a status block every 10 s.
- 2026-10-14 (night): committed 316e387 (faster memcpy/memcmp/memset: 51.6 ->
  53.1 frames/s at the game's own 60 fps interval; section 5, "Done on 2026-10-14";
  the user wants exact rewrites only, no quality cuts). Benchmark:
  `sh work/bench.sh work/auto_grid.txt TAG`.
- The newest build the user played is kept as
  `work/burnout3_sound5.dol/.elf`. The tree is that build plus the fix for
  the car select's choppy sound (below) and some logging under `spudump 1`;
  the user has not played it yet. That tree is kept as
  `work/burnout3_sound6.dol/.elf`.
- 2026-10-14 (later night): efd0b70 pushed (VU0 call path, gs_place records, plat_time_us; the benchmark with
  percentiles), then 726d9d3: the debug time split switched off by default (-2.8% of a heavy frame), a cached
  `GX_SetCurrentMtx`, the benchmark window moved to ticks 600 to 900, `wstat 7` counters (section 5, "Done in the night
  of 2026-10-14"). The mean work of a frame in the race's first seconds is 27.84 ms (28.78 before); the exact pieces left
  add up to 3 to 5%. Wii hardware tricks (prefetch, `dcbz`, locked cache, MEM1, `-Os`), none built and none measurable
  in Dolphin: section 5, "Wii hardware tricks".
- Nothing has ever run on a console. Everything measured is Dolphin's.

### Choppy sound in the car select: found and fixed (2026-10-13, evening); the user has not heard the fix yet

The user, 2026-10-13 about 15:21, playing `burnout3_sound5`: "the sound
started getting choppy at some point randomly on the car selection menu
(going inside the race reset it)". Their log is `work/last_play6.txt`.

**What it was.** Reproduced at normal speed (the earlier tries ran at
unlimited speed, which hides it) with `work/auto_wt3.txt` (World Tour path,
muted, `DumpAudio` on): in the car select the music's volume went from nothing
to full and back about thirty times a second. In the recording that is 50
stretches of exact silence of 10 to 12 ms in 7 s, each with a smooth fade
down and up around it (`work/snd_wt3.wav`; the scan tools are in the
scratchpad of that session: a zero-run and click scan of the dump).
The cause is a control loop between the game's sound driver and our model of
the chip:
- RWA.IRX keeps the volume of each voice by sweeps. Its ramp thread
  (file 0x1720, runtime 0x41720; the per-voice step is file 0x3358) reads the
  voice's level (`sceSdGetParam` VOLX), freezes it, and starts a new sweep
  toward the target. The rate is taken from a table (file 0xAB44, indexed by
  the remaining distance >> 7): every entry is made so the sweep covers its
  distance in 512 samples (decode it with the Irx class of tools/iop/irxdis.py).
  So the loop is built for one turn per 512 samples = 93.75 a second.
- The SPU2 interrupt handler (file 0x5E20) only wakes a thread. That thread
  steps IRQA through a table (IRQA moves on 256 samples each time: 187.5
  interrupts a second per core, core 0 used here) and runs the ramp's
  callback on every second one (a bit pattern 0xAA of a rotating mask:
  file 0x6274-0x6400). That is where the 93.75 comes from.
- Here the interrupts came at 87 a second (74 later), and the ramp at 43: the
  sweep ran for 1024 samples where it was made for 512, overshot by the
  distance it had to go, and the driver then turned it back: a limit cycle
  between nothing and full. On the PS2 the IOP takes the interrupt on time.
- Why late: the kernel's idle wait sleeps to the next event it knows
  (vblank, DelayThread, timers, alarms) and the chip's interrupt was not one.
  In the car select the processor is 60 to 70% idle, so the sleeps ran on to
  the vblank.

**The fix.** `iop_sd_next_us()` (runtime/iop_sd.c) says when the next interrupt
of the chip is due (from the IRQA address and the sample clock); `iop_next_deadline()`
(runtime/iop_kernel.c) returns the earlier of that and the IOP's delays, so
the idle waits (`next_event_us`, runtime/kernel.c) wake for it. On by default;
autopilot `spudl 0` turns it off (the old behaviour, for A/B).
- The first version made Dolphin 16 to 27% slower than real time from the
  first menu seconds. Not a bug in the logic: under Dolphin on Windows a host
  sleep of a few ms ends about a millisecond late, and with events every 5 ms
  the guest cannot make that time up (it has nothing to run in it). Waiting
  on the clock instead gives real time back, so `idle_sleep` (runtime/kernel.c)
  spins for waits of up to `g_idle_spin_ms` ms (autopilot `idlespin N`); the
  platform sets 20 under Dolphin only (`IOS_Open("/dev/dolphin")` succeeds
  there, platform/wii/plat_wii.c) and leaves a console sleeping.
- The spin starves the log thread (a lower priority than the game on
  purpose: the UART is slow); the log then came out in bursts, a minute late.
  So the spin sleeps in 1 ms pieces while `plat_log_pending()` says the queue
  has lines. (Keep that when touching either.)

**Measured, same path (World Tour, muted, normal speed), default build vs
`spudl 0`:**
- speed: wall time per 10.01 s of game time: 10.0 in both through the car
  select and the race (10.88 against 10.79 in the race's load block, 9.94 to
  10.04 in the race). `work/dumprate.sh` is the external check (audio
  recorded per wall second of the newest dump).
- the car select (recording 86 to 100 s): no stretch of silence (50 before),
  loudness steady (rms 4100 to 4300); music-volume writes 22 to 24 events in
  10 s (322 before). Driver interrupts 133 to 158 a second (87 before),
  still short of 187.5: the EE's long stretches without a poll delay them, so
  the ramp runs at 66 to 79 Hz; the loop gain is then 1.2 to 1.4, damped, not
  a limit cycle. Logs `work/last_wt11.txt` (default), `last_wt10.txt` (`spudl 0`),
  recordings `snd_wt11.wav`, `snd_wt3.wav` (before).
- The race's load has a silence of 0.8 s and then 6.5 s in the recording; the
  build before has 0.9 s and 8.8 s there. Not new; not looked into.

**Debugging added** (all under `spudump 1`, cheap): events 'L'/'R' in the
voice event log for the volume registers of core 1 voices 16 to 19 (the
register's new value as the place, the level it stood at after the slash), the
first volume writes/reads with the caller's return address, and a line
"the tick ran N times ... the driver's interrupt was called N times".

**Not done / unknown:** nothing is verified on a console (there the sleeps
are exact, and the spin is off); the interrupt rate is still below 187.5
when the EE is busy (a thread cannot be run in the middle of recompiled EE
code; an inline run of the IRQ thread from the tick would need a nested
scheduler); other loops of the driver that read VOLX/ENVX may have the same
sensitivity. Ask the user to listen to the car select again.

### Git

- Committed and pushed on 2026-10-13, at the user's word: commit 3877cf2 on
  the branch `wii-port` (master holds the first commit alone, ee9b46d), to
  the private repository https://github.com/shervinemp/burnout3-Wii
  (remote `origin`, made that day; `wii-port` is its default branch). It
  holds the runtime, the platform layers, the tools, the configs and the
  makefiles.
- Not in git, as the user asked ("excluding the doc and assets"): `docs/`
  (this file and native-renderer.md, left untracked) and everything of the
  game's (`work/`, `gen/`, `gen_iop/`, `gen.abi/`, `gen.old/`, `gen.vu/`,
  the build directories: all in .gitignore). `gen/` and `gen_iop/` are
  made from the disc by the recompilers (section 4).
- Commit and push again only when the user asks.

### Everything that is still open

**Reported by the user, not solved**
- The choppy sound above: fixed in the scripted run; waits for the user's ear.
- "Almost the whole frame" glitch: never reproduced.
- The ground drawn over the lower part of the car (rare).
- Not yet judged by the user after the fixes made for them: the hitch
  just before a collision (the log thread and sector reads were fixed; one
  spike frame was logged in a session since: frame 9600, game code 17.6 ms
  against 11.1, 4866 VU0 calls; and one at a scripted race's first frame,
  17.2 ms against 5.7 with 16 lists made), the lighting's "sudden shifts"
  at a race's start, the corrected game speed, 30/60 switching, spark
  density, collision feel, textures, the engine's loudness against the
  music.

**Sound** (section 6, item 0)
- Reverb is not modelled (core 1 has it on in a race: mode 5, EVOL 0x101D);
  nor noise, pitch modulation, the chip's four-point interpolation, the
  capture rings of voices 1 and 3.
- The mix reaches full scale in crashes (thousands of clipped samples in
  10 s); whether the PS2 clips there too is unknown.
- The engine's balance against the music has no reference (PCSX2 may not
  be driven without the user's word, which was never given).
- Only filter mode 1 has been met; mode 2 is written and untested, modes 0
  and 3 give silence as in the game.
- Cost, 4.5%: a decoded copy of the engine's eight looping samples (about
  0.5% for 700 KB), a higher `GAIN_HEARD`, the DSP (not started).
- The picture drawn by the software rasteriser at a track's load (a third
  of a second; the pump now mixes inside it) has not been looked at.
- The PC host's `B3_PCM` dump was never run.

**Speed** (section 6, item 0a and items 2 to 7)
- 60 fps in traffic lacks 1 to 1.5 ms a frame; split screen at 60 is far
  off.
- Not built: the native world loop spliced into 0x21C780 (about 1%; the
  design is in 0a), native batch commands in the chain
  (`hle_world_batch_ee`, `VIF_NATIVE_CODE` exist, unused), box tests left
  out for native batches, textures by identity for palettes and CLUT
  loads (the GS path is about 5%), shading reused for distant cars,
  `draw_setup`'s state application (2000 cycles each, 170 to 190 a frame),
  the VU0 dispatcher (1.4%), `vif_feed_words` (2%), `nw_lit_rgba` (0.9%),
  `plat_time_us`'s callers (0.5%, not found), the parse cache's copy
  (0.7%).
- Older candidates: GS leftovers, skid ribbons (VU1 4771B0, 0.67%), the
  crash blur, the final blit, particle kinds 0 and 3, the recompiler's
  third step.
- Memory: MEM1's arena had 4472 KB free before the parse cache's 750 KB.

**Real hardware**
- Nothing verified: SD remount, sound out (the DMA, the right-word-first
  order), the paired-single mixer and its use of GQR 4, sector reads, what
  the picture costs the GPU, how the data behaves in Broadway's caches.

**Debugging left in** (all log only)
- Frame history and spike capture, the output-ran-dry probe
  (`aq_dry_note`), kernel.c's wait log and counters, `WII_UART_LOG`, the
  status block every 10 s.
- "VU0 4787D0@0A18 native differs" (34 of the first 100 calls): solved on
  2026-10-15 (the check ran the lean routine; it runs the full one now, 0
  differences). The car select also logs "world render data not native:
  reason 7 (500000C7)".

**Older, from section 5 and 6**
- Lake reflections; the car select's floor reflection; the instanced
  batch fallback; hedge triangles in the VU1 fallback; the spoiler's
  flicker at the start line; "render target read as psm 2C"; the "Wii
  Remote 1 disconnected by emulated software" notice; the crashed car's
  underside; thinned spark trails on a heavy crash.

## 1. What the project is and what the user wants

- **Burnout 3: Takedown** (PS2, NTSC-U, SLUS_210.50) is statically recompiled
  to C and runs as a Wii homebrew (`boot.dol`).
- **The target is real Wii hardware:** Broadway 729 MHz, 24 MB MEM1 + 64 MB
  MEM2, Hollywood GX. Dolphin is only a debugger, and the PC build is only a
  reference harness.
- **The user wants native rewrites of hot paths, not small emulation tweaks.**
  - Replace whole PS2 pipelines with native code: VU1 render programs become
    GX hardware transform, and hot EE routines become C.
  - **60 fps in races is the goal.** Races run the game's own 60 fps mode
    (a fixed 1/60 s step an update pass); menus run its 30 fps mode. The
    game makes one pass a frame in ordinary racing, so a race drawn at 30
    fps ran at half speed: the port counts the passes itself (as many as
    the vblanks a frame is shown; `pace_passes`, `runtime/hle.c`) and picks
    the flip interval (`pace_frame`). See "The game ran at half speed" in
    section 5.
  - The real Wii is far slower than Dolphin on a PC (no cache-miss model in
    Dolphin): aim for structural rewrites, not tuning.
  - Display buffering is not a lever: waiting in present is 1-2% of a frame.
  - Open user reports (2026-10-12): spoiler/kit strip flickers dark at the
    start line (~20% of frames, two-valued; not palettes, not the car batch
    state, not the reflection map source), the car "sinks into the road" at
    times with sharp-edged parts drawn over, the road was "cut" once, crashes
    are slow (sparks: VU1 46D090; ~1300 game-built GIF sprites a frame).
- **The user tests in Dolphin themselves and dislikes long blocking tool
  calls.**
  - Run every Dolphin test with `run_in_background`, and never sit in a
    blocking wait loop. The user rejected exactly that twice with "STOP".
  - Give short status lines with numbers.
- **Commit only when asked.** The user asked once, on 2026-10-13 (section
  0, "Git"); `docs/` and the game's own files stay out of git. Work since
  then is uncommitted again. Don't stash, reset or clean.
- **Use the separate Dolphin test profile** (`-u D:\b3wii-dolphin\user`), never
  the user's own Dolphin config.

## 2. Environment

| What | Where |
|---|---|
| Repo | `C:\Users\sherv\Desktop\Projects\burnout3-Wii` (Windows 11; Git Bash for the Bash tool) |
| devkitPro | `C:\devkitPro`: devkitPPC gcc 15.2, libogc **3.1.0** (older versions freeze in the GX FIFO IRQ). Build in its msys2 bash: `C:/devkitPro/msys2/usr/bin/bash.exe -lc '...'` with `DEVKITPRO=/opt/devkitpro` |
| Python | `C:/Users/sherv/AppData/Local/Programs/Python/Python313/python.exe`. Always run scripts with `python -I` |
| Dolphin | `D:\Emulators\dolphin-master-2503a-603-x64\Dolphin-x64\Dolphin.exe`. Test profile `D:\b3wii-dolphin\user`, log `D:/b3wii-dolphin/user/Logs/dolphin.log` (grep `OSREPORT`) |
| SD image | `D:/b3wii-dolphin/user/Load/WiiSD.raw` (FAT32). Holds `burnout3/burnout3.iso`, `burnout3/auto.txt` (scripted input), `burnout3/args.txt`, and `prof.bin` written by the profiler. Read/write it with `tools/wii/fatcat.py`, only while Dolphin is closed |
| Game ELF (input to the recompiler) | `work/disc/SLUS_210.50` |
| Reference port | `C:\Users\sherv\Desktop\Projects\reVC-Wii`: the user's GTA VC Wii port. Reuse its GX backend, HBC packaging and deploy script as references |

`work/` is gitignored and holds all helper scripts, RAM dumps and screenshots
(`work/frames/`).

## 3. Architecture (one paragraph per layer)

- **EE code.**
  - `tools/recomp` (decoder, function analysis, C emitter) turns the PS2 ELF
    into `gen/recomp_*.c` and `gen/funcs.h`.
  - Title-specific settings live in `config/burnout3.py`:
    - `OVERRIDES`: guest address → native C function;
    - `VU_ENTRIES`;
    - `VU_NATIVE_SUBS`.
  - Guest RAM is a fixed MEM2 block at `0x90100000`.
    - Words are host-order; byte addresses use `^3` and halfwords `^2`.
    - `GPTR` masks addresses with `0x01FFFFFF`, so the scratchpad is RAM
      `0x0000–0x3FFF`.
- **VU code.**
  - `tools/vu/vurecomp.py` statically recompiles VU0/VU1 microprograms to
    `gen/vu0_*.c` and `gen/vu1_*.c`. Pipeline timing is modelled per block
    variant.
  - `runtime/vu.c` dispatches them. VU0 calls from EE code go
    `ps2_vu0_callms` → `vu0_fast`, a 512-slot cache, → native C or the
    recompiled program.
- **IOP.**
  - IOP modules are recompiled to `gen_iop/` by `tools/iop/regen.py`.
  - The kernel and services are emulated in `runtime/iop_*.c`. Memory card
    I/O is in `iop_sio2.c`.
- **DMA/VIF/GIF/GS.**
  - These are emulated in `runtime/vif.c`, `hw.c` and `gs.c`.
  - Texture uploads are kept "lazy": data stays linear and is identified by a
    hash per 256-byte block (`g_blk_owner`, `g_blk_idx`, `g_blk_hash`,
    `gs_blk_ver`, `g_blk_hver`). It is written to GS memory only when read.
- **GX backend.** `platform/wii/gs_gx.c`, about 10k lines, translates GS
  primitives to GX. It includes:
  - render targets, CLUT/texture caches and display lists;
  - a small CPU rasteriser (`soft_draw`) for tiny targets;
  - the native pipelines.
- **Native render pipelines.** These replace VU1 programs with GX hardware
  transform. `vif_defer_call` intercepts the DMA CALLs of render data, and
  `hle_render.c` parses the packets. The pipelines are:
  - `NW_PRELIT`, `NW_INST`, `NW_LIT` (+ display lists), `NW_INST_LIT` for the
    world (`runtime/nw_world.c`);
  - `NW_CAR` for cars (`runtime/nw_car.c`);
  - the sky (`vu1_sky_strip` in gs_gx.c, hooked into the recompiled VU1
    program 46A590 through `VU_NATIVE_SUBS`).

  `docs/native-renderer.md` maps the PS2 renderer and lists every pipeline.
- **Platform.** `platform/wii/plat_wii.c` provides:
  - time, files (disc-image block cache), memory and the autopilot (scripted
    input);
  - the sampling profiler and the status line.

  Host threads are cooperative green contexts on one LWP thread
  (`platform/wii/green.S`).

## 4. Build and run

```bash
# EE recompile (only after recomp/config/VU changes; ~1 min of gen rebuilds after):
#   work/regen.sh regenerates gen/ (copies only changed files) and rebuilds the PC host.
# Wii build:
C:/devkitPro/msys2/usr/bin/bash.exe -lc 'cd /c/Users/sherv/Desktop/Projects/burnout3-Wii && export DEVKITPRO=/opt/devkitpro && make -f Makefile.wii -j8 PYTHON=/c/Users/sherv/AppData/Local/Programs/Python/Python313/python.exe 2>&1 | grep -E "error|warning: impl" | head'
```

- **Output:** `build-wii/burnout3.dol` and `.elf`.
- **Build cost:** changing `OVERRIDES` changes `gen/funcs.h`, so every gen
  object rebuilds.
- **Job count:** the PC build uses `make -f Makefile.pc -j6`; `-j32` makes
  gcc run out of memory.

### Test run in Dolphin (always in the background)

```bash
cd /c/Users/sherv/Desktop/Projects/burnout3-Wii && rm -f work/prof.bin && AUTO=work/auto_prof.txt SHOTS="16:m16 30:race" SHOW=4 sh work/wiishots.sh > /dev/null 2>&1; L=/d/b3wii-dolphin/user/Logs/dolphin.log; grep -a "fatal\|cannot\|differ\|autopilot" $L | sed 's/.*OSREPORT\]: //' | cut -c1-200; grep -a "frames/s" $L | tail -5 | sed 's/.*OSREPORT\]: //' | cut -c1-200
```

- **What `work/wiishots.sh` does:**
  - kills Dolphin, puts `$AUTO` on the SD image as `burnout3/auto.txt` and
    starts Dolphin;
  - for each `N:name` in `SHOTS`, waits until the log has N status lines
    (one per 600 vblanks, about 10 s), then saves a screenshot to
    `work/frames/wii_<name>.png`;
  - kills Dolphin at the end;
  - stops early on `fatal|cannot allocate|out of MEM`.
- **Overlapping runs:** the script now writes its PID to `work/.wiirun` and
  exits if a newer run took over. Before that fix, leftover scripts killed
  later runs. Check `ps -ef | grep wiishots` before starting a run.
- **Run length:** a race profile run takes about 6 minutes at normal speed, under 2 with `SPEED=0` (unlimited; then
  with `DOLOPT="-C Dolphin.DSP.Volume=0"`, or the speakers get the sound at that speed).
- **Autopilot files:**
  - `work/auto.txt` drives boot → main menu → race;
  - `auto_prof.txt` adds `243 prof 20`, a 20 s profile during the race;
  - `auto_stat.txt` gives GS stats, and `auto_vu0.txt` the VU0 call stats.
- **Autopilot script lines** have the form `<sec> <button> <fields>`. Special
  commands:
  - `waitdraws N`: pause the script clock until frames have about N draws.
    Use it for screens: profile list 1801, "Load successful" 1141, main menu
    2645.
  - `waitfps`, `waitcard`;
  - `prof S`, `gsstat N`, `vu0stat 1`, `gxflags BITS` (turn native paths off
    for A/B comparison, e.g. 512 = sky, 256 = lit display lists), `native
    MASK`, `maxdraw N`, `rtdump 1`.
- **Before trusting a run, check the car-select screenshot**
  (`work/frames/wii_m16.png` should show COMPACT SERIES). Menu presses
  sometimes get lost.

### On a console (the first one: 2026-10-16, not run yet)

- `sh tools/wii/package.sh TARGET [ISO]` puts the Homebrew Channel folder in `TARGET/apps/burnout3`: boot.dol,
  `platform/wii/hbc/meta.xml`, `auto.txt` and `README.txt` (the user's instructions), the game image as
  burnout3.iso when given (`F:/Roms/Xbox/Burnout 3 - Takedown (USA).iso`, 2.9 GB: under FAT32's 4 GB), and the
  build's ELF as `TARGET/burnout3.elf`. The data folder may be `sd:/burnout3` or `sd:/apps/burnout3` (k_roots).
- The console test's `auto.txt` has no presses (a console's loading times are not Dolphin's): `pace 0`, `wstat 5`
  (the benchmark starts at the race clock's tick 600 of whatever race the user drives) and `benchprof 5` (new: the
  profiler for 5 s right after the benchmark has its frames, so its interrupts do not count in it).
- What comes back: `run.log` (it is fsynced every 10 s and after the benchmark and the profile: the FAT library
  writes a file's length at a close or a sync only, so a console switched off after a hang left an empty log
  before), `prof.bin`, `profstack.bin`. The build first packaged is kept as `work/console1.dol`/`.elf`: read its
  profile with `python -I work/wprof.py PROF.BIN -e work/console1.elf` (on a console the samples are exact: no JIT
  blocks).

### Profiling

- `python -I work/wprof.py -n 30` reads `prof.bin` from the SD image (Dolphin
  must be closed) and maps it with nm. Add `-c FUNC` for callers.
- `python -I work/wfn.py FUNC --dis` shows the buckets inside one function.
- `python -I work/vuprof.py vu1_XXXXXX OBJ_WITH_-g` gives samples by VU micro
  address.
- **Caveat:** Dolphin delivers the profiler interrupt at JIT block starts, so
  samples can land in the next block or function. Function totals are fine.
- **Always profile the current build:** an old `prof.bin` against a rebuilt
  ELF gives wrong attributions.

### Status line (every 600 vblanks in the log)

It shows:

- frames/s;
- draws per frame;
- uploads and texture cache;
- a time split (EE/VU0/VU1/VIF/GIF/upload/tex/gx/idle);
- "gx: per frame …" counters: native draws, display lists, and car/world
  batches native vs emulated.

## 5. Current state (2026-10-12)

- **Boot to main menu:** about 60 s in Dolphin.
- **Race frame rate (Dolphin):** a solid 30 fps at the start line: every
  frame takes 2 vblanks, and about 2.4% of the time the CPU is idle. It was
  about 17 fps two sessions ago.
- **Car select:** at the 30 fps cap (was 20). Untextured world batches,
  packets of up to 16384 qwords and car display lists made the difference.
  - Gains came from the native sky, native lit world display lists, lit
    instanced props, native VU0 entries, audio DSP overrides, and memory-card
    and disc caches.
  - The "Load successful" dialog runs at 21.7 fps, was 11.5.
- **Verified exact against the recompiled code:**
  - all native VU0 entries (100/100 self-checks each, log line
    `VU0 prog@entry native: 100 calls checked (0 differences)`);
  - the audio overrides (24/24).
- **Checked visually:** sky and lit display lists.
- **Race profile before the in-flight change**, % of samples:

  | Function | % | What it is |
  |---|---|---|
  | nc_shade | 5.4 | CPU car shading |
  | lazy_finish | 5.3 | upload block hashing/bookkeeping |
  | nw_lit_rgba | 4.0 | lit world vertex colours |
  | gs_load_clut | 3.6 | being fixed now |
  | vu0_vis | 3.1 | |
  | draw_setup | 3.1 | |
  | vif_feed_words | 2.7 | |
  | gif_path_qword | 2.7 | |
  | gs_backend_car | 2.4 | |
  | put_row | 2.3 | two thirds from tiny non-lazy palette uploads via xfer_run, the rest gs_sync_rect |
  | packet_bulk | 2.0 | |

### Done on 2026-10-12

- **CLUT fast path:** a CLUT that misses the CLUT cache is read straight from
  the pending upload. `gs_load_clut` fell from 3.6% to 0.9%.
- **Spoiler/kit flashing in races, reported by the user:** car palettes are
  rebuilt in one guest buffer per car. In-place uploads read that buffer
  lazily, so later palette loads got another car's colours.
  - Fix: uploads of 1 KB or less are copied at upload time and hashed in
    full (`lazy_data`, `upload_block_hash` in gs.c).
  - Found with screenshot bursts and per-shot brightness measurements (see
    the native-render-debugging memory).
- **Untextured world batches** (no UV unpack) are drawn natively when PRIM
  has TME clear.
- **Car display lists:** undeformed car batches use cached display lists
  plus per-frame colour arrays (`nc_shade_direct`). They were checked
  against `nc_shade`: 300/300 batches identical.
- **`lazy_finish`:** the in-place path steps through rows instead of
  recomputing them. 5.4% → 4.0%.
- **A/B switches** (autopilot): `nlog`, `clutup`, `clutcache`, `clutchk`,
  `extup`, and gxflags 1024/2048.
- **Triple-buffered display:** with two buffers the picture rubberbanded in
  time (see docs/native-renderer.md).
- **Small speedups:** an A+D GIF fast path, a bulk path for small uploads,
  a cache for indirect calls, and exact conversion shortcuts in the car and
  lit-world shading.
- **Autopilot fix:** a `waitdraws` wait now also accepts a later screen. A
  faster boot had made the script hang at the main menu.
- **Memory is the limit:** the status line now has a `mem:` line. With the
  profiler on, only about 300 KB is free in a race.

### Done later on 2026-10-12 (correctness; see the code comments named)

- **Car reflections flickering (spoiler, kit, anything facing the ground):**
  three bugs, found with a frame bar code on screen (gxflags 2097152) matched
  to log lines.
  - `packet_bulk` (runtime/gs.c) took the XYOFFSET of the context PRIM had
    *before* the packet: a packet that switched context with its own PRIM
    drew 1728 pixels off. In races that was the game's clear of the
    reflection views' depth buffers.
  - The game clears those views through other views of their memory (one
    32-pixel wide sprite into 0x3160 / 0x2D60 as a 64-wide buffer with ZBUF at
    the same address: the PS2 quick clear). `mem_clear` (gs_gx.c) turns that
    into clears of the targets whose colour or depth memory it covers.
  - `rt_copy` with clear only cleared what the current GX write masks let
    through, and draw state cached for a target survived its move in the
    strip.
- **Timers:** T0..T3 count from when they are started and raise overflow and
  compare interrupts (`hw_timers_poll`, runtime/hw.c). libkernel's timer
  system (T2, INTC 11) and the game's clocks built on it were stuck in their
  first 114 ms before; the parked camera drifted and stepped unevenly.
- **In-place texture uploads** are identified by a hash of all their data,
  re-read when a source comes back after a frame away and every 12 frames in
  turn (`ext_ident`, runtime/gs.c), no longer by address and five samples.
- **Start-up trap:** libogc's `__dsp_bootstrap` overwrites 128 bytes at
  0x81000000 for a moment; with over 16 MB of code a library function can be
  linked there (a build booted 1000 times slower: the cache flush routine ran
  as DSP code). Makefile.wii pads so that only padding or game code is there;
  `tools/wii/dspcheck.py` checks every link.
- **qsort** (0x125338) is native for the update's per-frame sort by float key
  (`hle_qsort`, self-checked); 192 hot game functions are compiled -O2
  (config/burnout3_hot.txt); the flip wait sleeps instead of spinning
  (`hle_flip_wait_entry`), so "idle" in the status line is what is left of a
  frame.
- **Test tools:** `SPEED=0` (unthrottled Dolphin), `work/burst.py` (fast
  screenshots), `work/flk.sh` + `work/rtev.py` (parked flicker test),
  autopilot `nopad 1`, `work/gdbpc.py` (PC samples through Dolphin's GDB
  stub), `work/pcenv.sh` + the PC host's `gsmem N` command.

Open user reports after these (2026-10-12, not yet re-checked by the user):
the car "sinking into the road", jitter while parked, timing in crashes and
slow crashes. Frame work in a race: ~29 ms parked, 33-40 ms driving and in
crashes; 16.7 ms is the target.

### Done in the evening of 2026-10-12 (pacing and frame time)

- **Game speed and frame pacing.** (Corrected on 2026-10-13, see "The game
  ran at half speed": the game counts passes by the clock in one of its
  timer's modes only.) The game keeps its own speed: the race
  loop (0x132600, state 5) runs N update passes of 1/60 s per drawn frame,
  N from 0x376570 (1 to 4, by the EE timer clock). With the timers fixed
  (see above) this works, so `g_fps` is no longer forced to 30: races run in
  the game's 60 fps mode and take 2 updates per frame while they draw at 30.
  - Two attempts to stretch the game clock here (several ticks a pass, then a
    varying step with a native timer update) were wrong and are gone: the
    first left the chase camera at the car's front wheel (builds between
    18:14 and about 19:30 on 2026-10-12), the second fought the game's own
    extra passes. Do not go there again.
  - What is left is `pace_frame` (runtime/hle.c, called once per race frame
    from `hle_env_clear_entry`): it picks the flip interval (1, 2 or 3
    vblanks, with hysteresis) so that frames near a boundary are not shown
    for 2 and 3 vblanks at random. Status line: "pacer: interval N, race
    frames of 1/2/3/4+ vblanks". Autopilot `pace 0` = the game's interval.
  - A race becomes 60 fps by itself once a frame's work fits 15.8 ms.
  - Slow frames no longer show as slow motion, so the picture has a frame
    time meter in its top left corner (1 to 4 white squares = vblanks the
    last frame took; `g_gx_meter`, autopilot `meter 0`).
  - Cost: at 30 fps the game's update runs twice a frame, at 20 fps three
    times (a few ms each); the forced 30 fps mode ran it once.
- **Stopped car turning to and fro** (user report: "slowly shifts and
  slides", "the world moving"): fixed by `hle_car_forces` (runtime/hle.c,
  override of 0x1CC900; autopilot/PC command `resthold 0` = as the game).
  - What it was: at 0 mph the car's centre stood still but it turned about
    its up axis by about a tenth of a degree, to and fro, and the chase
    camera swung the picture. Below 0.2 m/s the game scales velocity and
    angular velocity by 0.9 an update (end of 0x1C38F0); the velocity gets to
    zero, the yaw does not: the tyre friction of the same function and a
    constant suspension torque push it again, harder near zero (the angular
    momentum about y ran -6.5 .. 0 .. kick .. in a sawtooth).
  - The same on the PC host, so not the Wii side. Not compared with a PS2:
    it may well be the original's behaviour.
  - Measured on the PC host, car stopped: heading the same to 6 decimals
    over 20 s with the hold, 0.0035 rad per 20 s without. In Dolphin two
    screenshots 24 s apart differ only in traffic, lights and smoke.
  - Addresses: the player's car 0x57BAF0 (its rigid body first: +176
    velocity with the speed in w, +208 angular velocity, +224 angular
    momentum, +240/+256 force and torque sums, +272/+288 impulse sums, +516
    pointer to its matrix, wheels of 192 bytes from +2080). Car update
    0x1C4890 -> 0x1C1B20 -> 0x1C38F0 (tyres, the rest rule), 0x1CC900
    (suspension, wheel matrices), 0x1AC470 (rigid body integrator).
  - Traps met on the way: races have a rolling start (the car moves with no
    input until GO and then coasts for minutes); the blocks of 0x180 bytes
    around 0x53AB10 are traffic cars, one of which waits at the lights by
    the grid (an earlier "0.7 mm/s creep" was that car); the PC host's game
    time runs several times slower than the clock.
  - Tools: `work/parkpc3.sh` (PC host: brake to a stop, RAM dumps with the
    hold on and off), `work/carrest.py` (heading, speed, momentum from the
    dumps), `work/pretty.py ADDR N` (disassembly with FPU and VU0 macro
    instructions as assignments), PC command `stick LX LY RX RY`,
    `work/torace2.sh` (longer menu presses: fewer lost).
- **Profile of the stopped scene** (2026-10-12 evening, car braked to a stop
  by the junction after the start, `work/auto_restprof.txt`): 30 fps with
  3 to 12% idle, heavier than open driving (31-35% idle). 3183 draws and 603
  uploads (2.8 MB) a frame. By call stack: the frame's chain 55% (VIF1 40%:
  world 11.6%, cars 10%, micro program starts 10%; PATH3 12%), the game's
  render build (0x276300) 21%. The GIF bulk path is 20.9%: texture uploads
  7.1% (`lazy_finish` 5.5%, the largest single function), prims 5.8%, CLUT
  loads 2.6%. Flat otherwise: no function above 5.5%. 60 fps needs this
  scene at half its work.
- **The 60 fps path, tried with Dolphin's CPU clock at 2.5x** (testing only:
  `DOLOPT="-C Dolphin.Core.OverclockEnable=True -C Dolphin.Core.Overclock=2.5"`
  before `work/wiishots.sh`; a Wii has no such thing): the pacer goes to
  interval 1 by itself and the race holds 59.9 fps (25-36% idle), chase
  camera and game speed as they should be, nothing in the log. Frame work
  scales with the clock (no fixed part found), so what 60 fps needs at the
  real clock is the work itself: about 20 ms open road (one update a frame)
  and 29 ms in the stopped junction scene, against 15.8 ms.
- All of the hottest recompiled functions of that profile are in HOT_FUNCS
  already (-O2): nothing to gain from the list.
- **Reflection views in turn** (`hle_env_clear_entry` and friends in
  runtime/hle.c, `gs_backend_note` in gs_gx.c): one of the three views is
  drawn per frame, the other two keep their picture. About 4 ms a frame.
  The frame's choice travels to the backend inside the chain as writes of the
  GS's NOP register (gs.h, "notes"): the chain is drawn later than it is
  built, and a first version that told the backend directly held the wrong
  views. Autopilot `envrot 0` = all three every frame.
- **Visibility tests** (`box_plane8s`, runtime/vu.c): a box clearly on one
  side of a plane is decided from its centre and radius; only boxes near the
  plane are tested corner by corner. Checked against the recompiled code
  with `vu0chk 1`: 250000 calls per 10 s, none differ.
- **Texture cache** (gs_gx.c, "texture memory"): was a ring that dropped
  every texture each time it wrapped (every 30-60 s of a race: ~150 textures
  decoded again in one frame, a long stall), and TLUTs leaked into it. Now
  spans with first fit, the least recently used textures dropped for room
  (`tex_evict`), TLUTs in 512 fixed slots, and paletted textures are decoded
  a row at a time (`gs_read_row8`, `gs_upload_row8`), not a call per pixel.
  `dbg 4` at start shrinks the arena to test eviction.
- **Findings worth knowing:**
  - The frame's DMA chain is drawn inside the vblank interrupt that flips
    (handler 0x1F1CE8 -> 0x1F1778), usually while the main thread waits in
    the flip wait; a frame's time is update + render build + chain, serially.
  - A driving frame after these changes: about 20-23 ms of work (was 24-26),
    the start grid more. Split of the work: chain ~60%, render build ~20%,
    update ~9%. Nothing in the profile is above 4%.
  - Particles (VU1 46D090, 15 runs a frame in crashes and with boost) are the
    biggest single item when they show: up to 3 ms.
  - Each frame makes three full-screen EFB copies and two restores (the game
    draws into 0x8C0 and blits to 0x0: both "big" targets share the EFB). Free
    in Dolphin, several ms on a real Wii: to be removed.
  - devkitPPC's gcc sometimes dies with an internal compiler error on a gen
    file for no reason; run make again.

### Done at night on 2026-10-12 (textures at the game's level, blur, pacer)

- **Textures the game places are not uploaded any more** (`hle_tex_send`,
  runtime/hle.c, override of 0x221E80; "textures the game placed" in
  runtime/gs.c; `tex_lookup` in gs_gx.c). The game's own texture cache gives
  a texture a place in GS memory and sends its prebuilt upload script. The
  override leaves the function as it is and then rewrites the raster's entry
  in the chain into notes (gs.h `GS_NOTE_TEX_ENTRY`/`_AT`, or one
  `GS_NOTE_TEX_HERE` for 8 bit textures whose packet goes on with the
  palette: there the DMA ref is moved past the raster and the palette is
  still sent). The GS keeps a table by TBP (`g_placed`): the script entry, an
  identity of the data (`ext_source_ident`) and the GS write version at the
  time. `tex_lookup` asks `gs_placed_for` first and keys the GX texture on
  that identity; on a cache miss `gs_placed_load` writes the raster into GS
  memory once and the old decoders read it.
  - Numbers at the stopped junction scene: uploads 2.8 MB -> 0.6 MB a frame,
    about 50 textures a frame bound without an upload, idle 8-10% -> 11-14%.
    Autopilot `texplace 0` sends everything as before (A/B in one run).
  - Lessons, each cost a broken build the user saw: (1) the TBP must travel
    in the note itself. Read from BITBLTBUF at the time the note is handled
    it is the palette's for scripts with a second entry: records landed at
    palette addresses, pink and rainbow surfaces, 40 loads a frame. (2) A
    placement covers the texture's whole allocation ([tex+100] words), not
    only the raster's rectangle: stale records inside it must go when it is
    placed. (3) Whatever reads GS memory directly (the blur's ramp, render
    target reads) needs `placed_sync` in `gs_sync_rect`, only for reads at
    (0,0): palette writes into the buffer set it off 20 times a frame
    otherwise.
  - Left the old way: about 105 rasters a frame (script entries after the
    first, other upload functions) and about 235 palettes, staged and hashed.
- **Speed blur follows the game's fade** (`g_sb.comp_a`, gs_gx.c). The game
  draws its blur chain every frame and fades it with the vertex alpha of the
  last sprite (0 at a standstill). The native pass ignored that alpha: blur
  at 0 mph. Now the pass is skipped at 0 and scaled below 0x80. Status line:
  "speed blur in N frames" is 0 when stopped, 200-300 of 300 at speed.
- **Pacer** (`pace_frame`): the step down to a shorter interval compares the
  work times a learned `gain` (the work after a step down over the work
  before it; a frame at the shorter interval has an update pass less). It
  was stuck at 20 fps with 30% idle before.
- Removed: nothing. `resthold`, `texplace` are autopilot switches now.
- **Two-sided world geometry lost half its faces** (user: "something wrong
  with the bushes on the side"; a hedge by the first junction drawn as a
  wedge). Only two world microprograms cull by winding, 46F0D0 (prelit) and
  470650 (lit); their twins 4745C0, 47E300 and 472EB0 have no such test
  (hedges, foliage, fences). `native_cull` (gs_gx.c) culled by 1013.z
  whatever the program; now only for those two (`g_vu1_code`), and 1013.z
  from 0x20 up is the other winding (the microprogram has no "none").
  Found by parking at the same spot and switching `gxflags 32` (no native
  cull): whole with it, a wedge without.
  - Seen on the way and still open: at another spot (5078.98 1853.70) the
    same hedge was a wedge when everything went through VU1 (`native 2`):
    the emulated clipping path of the microprogram (1013.x set) drops
    triangles there. Only a fallback now.
- **Test positions** (plat_wii.c `goto_buttons`; autopilot `gotox <dm>`,
  `gotoz <dm>`, `goto 1`): drives the player's car to a point with the pad
  and stops it there (within 0.6 m; logs "goto:" once a second and
  "autopilot: at ..." on arrival). The heading it arrives with is whatever
  the approach gave. Without it: after the rolling start the car coasts to
  a stop at 5078.19 1849.78 by itself, the same in every run, facing along
  the road (hedge in view on the right).
- **Debug logs added:** `gxflags 67108864` = "carwatch" (the player's car
  batches per frame: count, deformed, depth and screen place, position,
  speed, tilt; the frames around a jump, and a sample every 600 frames);
  `carlog 1` (hle.c, on by default on the PC host) = the player's car after
  every 0x1CC900 call inside the box of the first fence of Silver Lake.
- **Trap, cost an hour:** helper loops that send `cmd.txt` commands to the
  PC host (screenshot loops with "cross:400") must end with the host. Loops
  left from killed runs kept pressing buttons into every later host and
  wrecked its menu navigation. Check `ps -ef | grep -E "ctl.py|scrapepc"`.
- **The lake's mirrored scenery drawn unmirrored** ("the ground renders
  over the lower part of the car, briefly and rarely", "sometimes almost
  the whole frame"). The microprogram 47E300 is not just a two-sided
  4745C0: at 0x02A8 it negates the camera's y column and adds twice 1009.y
  of it to the translation, i.e. it draws its batches mirrored about the
  plane y = 1009.y, the water level (and puts the camera back at 0x1368).
  Those batches are the scenery's copy for its reflection in the lake,
  coarser than the scenery and around it. The native path drew them where
  they are stored: a second, coarser ground that lay up to 0.7 m over the
  real one in places (measured by the first fence of Silver Lake, 4520
  1489: the road 0.22 m under the car's origin, a 47E300 surface 0.2 to
  0.67 m above it). `native_camera` (gs_gx.c) mirrors now when
  `g_vu1_code` is 47E300; `gxflags 134217728` = as it was.
  - How it was found, after ruling out skipped draws, deformation, NaNs,
    the camera (1.7 m above the car throughout), depth constants and state,
    and the order of the targets: the carwatch log grew a probe that
    intersects a vertical line through the car with every world triangle
    of the scene ("drawn ground from the car's height", with the
    microprogram of each surface). The lesson: a microprogram that differs
    from a known one "only by culling" has to be read to the end before it
    shares a native path.
  - `gotor <dm>`: the goto autopilot takes over only within that radius
    of its point. Parking by braking from 58 m/s spun the car; not used in
    the end.
- **Memory (MEM2) in a race: about 1 MB free** (was 0.5 MB). Found when
  the profiler's 0.6 MB of buffers made `post_bloom`'s `gx_alloc` fail at
  the start of a race (FATAL, the watchdog names the stack). Changed: the
  GS upload stage is 512 KB on the Wii (races stage nothing). Not changed:
  the texture arena stays 4 MB. It fills as far as it is allowed (3.4 MB
  driving, 1.1 MB standing: do not read its use off a standing scene); cut
  to 2.5 MB it thrashed, 5000 to 8000 decodes a frame, 7 frames/s. The only
  place several MB can come from is the 17 MB of recompiled code (MEM1):
  cold functions interpreted instead of compiled. Not started.
- **Driving profile of 2026-10-12 night** (`work/auto_driveprof.txt`, 21 s
  from 270 s, the fence scrape in it; `wprof.py work/prof.bin -e
  work/prof.elf -i` / `-t FUNC`; 23% of the samples idle at 30 fps, the
  rest = "work", about 25 ms a frame). Shares of all samples:
  - `dma_vif1` (the backend drawing the frame's chain) 44%: cars 13.8%
    (`nc_shade_direct` 6.4, `draw_setup` 2.6, the object's GS packet 1.4),
    PATH2 packets (`gif_path_bulk`) 13.7%, VU1 programs still emulated
    8.7% (sparks 46D090, the sky's strips 46A590, 47A060, 46D750), world
    batches 4.3%.
  - the game's frame render 0x276300 19.7%: about half of it the section
    traversal 0x21C780 with its VU0 box tests (9.5%).
  - the game's update: under 10%, though it runs twice a frame at 30.
  - For 60 frames/s: backend + frame render from about 21 ms to 14 ms.
- **What the PATH2 primitives are** (`gsstat N` now splits PATH2/PATH3 by
  texture and target: "code" = FBP * 32, "prog" = TBP0), per frame:
  - 666 triangles fold the three reflection views (3160, 3260, 3360) into
    the sphere map 2D60, every frame, though one view changes a frame;
  - 128 points draw a 16x16 palette into 2D20;
  - HUD text 100 to 400; the post chain's strips about 150;
  - PATH1: sparks (46D090) 240 to 1200.
- **The car's two passes** (carwatch logs "car pass N state"): pass 1 ALPHA
  0x8000000064 (opaque: FIX 0x80), TEST 50003 (Z GEQUAL, written), alpha
  out = texture x vertex alpha (the Fresnel term); pass 2 ALPHA 0x54 =
  (Cs - Cd) x Ad + Cd, the sphere map laid over by pass 1's alpha, TEST
  51001 (alpha test never, FB only: no Z written). Other pass 1 states
  blend (FIX 04..66, or As); pass 2 with ALPHA 0x6A draws nothing but
  alpha.
- **Done: both car passes in one GX draw** (`car_combo_ok`, `car_combo_tev`
  in gs_gx.c; `gxflags 268435456` = two draws as before). Where the two
  states are exactly the pair above: stage 0 the paint as pass 1 has it
  (apply_state), stage 1 the sphere map x 1012 into TEVREG0, stage 2 mixes
  by stage 0's alpha and puts out stage 1's alpha. The sphere map's UVs go
  in a second per-frame array (the car lists carry a fourth index;
  `NW_CAR_COMBO`, two texture coordinates). `g_gxc.tev == COMBO_TEV` marks
  the three stages as loaded; draw_setup drops the state for any draw that
  is not such a car draw (`g_combo_call`), since a draw with pass 1's GS
  state would else go through them. Blended paint (glass, fading cars)
  stays two draws. Looks the same parked at the start line (A/B shots).
  Profile of the same drive: idle 22.9% -> 28.2% of the samples (about 7%
  of the work), states applied 524 -> 110..180 a frame.
- **Done: the per-frame arrays GX indexes are flushed** (DCFlushRange after
  nc_shade_direct). The cars' colour array was not: nothing in Dolphin,
  stale or garbled car lighting on a console. The arena (`g_litcol`) is in
  MEM2 now, 160 KB a frame (a race 70..80 KB, the start line 120; the car
  select screen fills it and sends the rest immediate, as before).
- **Done: the game's static strips as display lists** (`gs_backend_strip`,
  gs.h; hook in `gif_path_bulk`; `gxflags 536870912` = off). The three
  strips that fold the reflection views into the sphere map are REGLISTs
  of ST and XYZ2, 446 vertices each, at fixed addresses (009FC8B0,
  009FE4D0, 00A000F0) with the same content every frame. Kept by address,
  content hash, RGBAQ, XYOFFSET and Z format; lost with the world's lists
  when the arena is reset (`g_dl_resets`). Idle 28.2% -> 29.3%.
- **Where that leaves the drive profile:** work 70.7% of a 2-vblank frame,
  23.6 ms (was 25.7); 5 or 6 race frames in 300 take one vblank. Next by
  size: the section traversal 0x21C780 and its callees (9.5% inclusive,
  2080 lines recompiled: a native walk of the sections is the big one);
  `nc_shade_direct` 5.1% (paired singles); uploads materialised at vsync
  (`ur_materialize` from `gs_vsync` 1.2% + `put_row` 1.2%: the "aged out"
  ones); `gs_load_clut` with its memcmp 1.4%; the sky's strips still built
  by the microprogram 46A590 (1.8% inclusive); sparks 46D090 when scraping.
- **What 0x21C780 is** (read from the recompiled code, nothing run; for a
  native walk). Called once a frame from 0x276300 with the renderer object
  W; it does view 0 and the reflection views 1..3 itself (the view is
  state byte +153; state S = 0x01E32A10).
  - Data: the PVS record `*(W+452)`, whose 3072 bytes from +96 are copied
    to the scratchpad 0x70002000 by DMA: +3 the count of visible
    "sections" (a section is a material group, 40 bytes at `*(world+8)`:
    type, texture table +12, texture index +16, TEST +32, flags +36,
    ALPHA +38/+39), their indices as s16 from +0x14, and per chunk c
    (17 chunks, 169 bytes each from +186) a batch index per section for
    the main view (s8 at +189) and for the reflections (+272), -1 none.
    Chunk groups `W+124[c]` (main) and `W+196[c]` (reflections), batches
    64 bytes at `*(group+84)`; instance tables `W+268[c]`; object types 80
    bytes at `*(world+56)`.
  - Order: 0x21D080 (two clip bits per chunk and view into `W+88[c]`, VU0
    4787D0 entry 0xA50); 0x26B8E0 (begin); up to 8 static lists
    (0x332D00); for each visible section: 0x26AE30, then per enabled view
    the 17 chunks' batches through 0x26AF50 (`hle_world_batch`), then
    0x26B2B0 (the group setup 0x26B2F0: VU1 program by section type,
    texture bind 0x221A70, setup packet; then the staged batches go into
    the chain); then the instanced types (0x26A7E0, a view added per
    0x26A710, 0x26A610 per chunk: `vu0_vis` per matrix, 0x26A1C0);
    0x21D550 (two more lists; it writes 7 into a section's type and puts
    it back); 0x26B7B0 (end).
  - To keep exactly: the chain's content and order (setup before the
    section's batches; the view's 15 qwords once, with the first batch
    after 0x26BAA0; the clip qword against S+152), the caches `gp-28060`
    (VU1 program) and `gp-24080` (texture), 0x667050, S+153 (the
    reflection skip reads it), S+48/64/80 (section types 1 and 2 change
    them, and that adds up over the frame), 0x1F3080 as the only way to
    chain space, the scratchpad copy (0x307A2C reads 0x70002980).
  - Cost by part (inclusive, of all samples): instances 0x26A610 2.0,
    section end 0x26B2B0 1.8, batches 0x26AF50 1.6, 0x26A1C0 0.9, static
    lists 0.7, chunk flags 0.7, itself 0.7. No part dominates: a native
    walk would take perhaps a third off the 9.5%. 0x26BAA0 runs once per
    section per view even when no batch follows (15 qwords each).
- **Seen in the log, not looked at:** target 2D60 reshaped twice a frame
  during the scrape ("gx: reshape 02D60 256x256 -> 320x320x448 (dropped)"
  and back); "native draw of kind 2 batch 3/4 (20 vertices) at 01487800
  failed" (an instanced batch falling back to VU1).

### Done on 2026-10-13 (list memory, strips, the GPU wait)

- **Every run's Dolphin log is kept** (`work/logs/dolphin_<date>_<time>.log`,
  the newest 8; `work/wiishots.sh` copies it before it empties the log).
  A session the user played was lost to the next test run before that, and
  with it the chance to look up a report ("the lighting changed all of a
  sudden"). Tell the user Dolphin's screenshot key (F9) as a marker: the
  file's time matches the log's wall-clock column.
- **The display list arena was emptied every 12 s in a race** ("58 drops"
  in a 12-minute session), and each time every list in view was made again
  in one frame. Two causes, both fixed:
  - the static strips of the night before: the speed blur's strip (the
    fourth; three fold the reflection views) is at another address every
    frame, so a 14 KB list was made per frame in the world's arena. Strips
    now have their own six buffers at the end of the arena
    (`SDL_SLOTS`), a strip gets a list only when it comes a second time
    unchanged in a later frame, and anything else is sent as it is
    (`strip_emit`, still without the GS vertex path). The menus' strips
    change colour every frame and go that way too.
  - the arena itself: now a ring of eight segments (`dl_room`). When the
    one being filled is full the next (empty) one is filled and the one
    after it is emptied for the turn after: lists drawn in the last 60
    frames are copied over (a list is bytes), the rest are left behind.
    No list is written where the GPU may still read (a segment is written
    a turn after its lists left; two turns within three frames wait for
    the GPU). Status line: "segments turned, lists carried over, left
    behind; all dropped N times so far" (0 in a drive now).
- **`gp_drain`** replaces every mid-frame `GX_DrawDone`: with the frame's
  own draw-done pending (set at vsync, waited for a frame later), a
  `GX_DrawDone` in between returned at the older finish, and its own
  finish interrupt then ended the next frame's wait early. A draw sync
  token has neither problem. Real hardware only; Dolphin showed nothing.
- **The cars' sphere map UVs are bytes** (0.8 fixed; the map is 64 pixels):
  6 bytes a vertex in the per-frame arrays instead of 12. With floats the
  six cars at the start line (22000 vertices a frame) overflowed the 160 KB
  and the last cars of the frame went the old way (nc_shade, immediate,
  two draws). The arena is 192 KB a frame now; the status line gives the
  most a frame asked for and the allocations refused.
- **All car vertices are shaded for the main view** (none for the
  reflection views): 13000 a frame driving, 22000 at the start line.
- **Sparks natively** (`nw_spark_segment` in nw_world.c, `gs_backend_sparks`
  in gs_gx.c, hook in `vif_start_micro`; `gxflags2 1` = through the
  microprogram, `gxflags2 2` = the same with the native result compared,
  "sparks check" lines). The microprogram 0x46D090 took a long scrape from
  30 to 20 fps (7000 GS primitives a frame, VU1 18%); natively the same
  scrape is 430-500 quads a frame at 30 fps, VU1 4%.
  - What the microprogram does (read from it; entries 0 = an emitter's
    first batch, MSCNT = the next ones; BASE 0, OFFSET 99): per trail
    segment s (913.x of them, at most 17) it goes over all particles of the
    batch (ITOP & 15 groups of 4 at TOP, 9 qwords a group: birth times, then
    position and velocity per particle; ITOP as a number times 913.y is the
    lifetime), keeps those with `t0 - L < birth < t1` (t0, t1: the times of
    camera history entries s and s+1 at 916 + 6s), puts the point at both
    times (y and w with the acceleration 913.w / 2, y not below w), each
    through its entry's matrix, drops the particle when either end is
    outside the view (no clipping), and makes a quad of half width 913.z
    around the two points in (x sx, y sy, w), each corner divided by its
    own third component; U from the time, V one of two; colour from the
    RGBAQ register the game sets by PATH2 before; additive, no Z write. One
    XGKICK a segment into buffers 198 and 551 in turn (`vi2`).
  - The decision native or not is taken at entry 0 only: the MSCNT runs
    need the state entry 0 leaves in VU registers.
- **The game's bloom chain, decoded from a `gxflags 16384` log** (for
  comparing `post_bloom` with it): bright pass per 64-pixel column,
  (Cs - Cd) x FIX FF over a buffer cleared to BF; each column halved with
  Cs x FIX FF and vertex colour 88 (another x2.1, which `post_bloom` does
  not have); 320x224 to 160x112 (colour 87); 20 sprites of
  lerp(Cd, Cs x 8A/80, 54/80) as the blur; accumulation Cd x 3B/80 + Cs;
  composite Cd + Cs. Then (speed blur) the scene to 320x224, a 32x32 ramp
  into its alpha, three zooms of lerp 40/80, back over the scene by alpha.
- **Open: the user sees "sudden shifts in lighting, like enabling and
  disabling, especially towards the beginning".** Found so far, from
  bursts of screenshots with state squares (`gxflags 1073741824`: squares
  under the frame meter for speed blur, bloom, particles, colour arrays
  full, decay raised, segment turned, odd frame): the bloom washes walls and
  railings towards white for about half a second while white pavement is in
  view near the start and drops within two frames when it leaves. The
  bloom's threshold and decay are steady, it is never skipped, and the
  speed blur coming on changes nothing in a still scene. Whether the game
  itself pumps like that is being checked against the PC host
  (`work/startpc.sh`: a screenshot every 3 s from the race load).
- **The PC host is the reference picture and it works for this**
  (`work/startpc.sh`: into the race, a screenshot every 3 s; the car coasts
  from the rolling start to a stop at the Food Kart corner, the same place
  the Wii autopilot passes at 12 mph). What it showed:
  - the wash to white at the crossing after the start is the game's own
    bloom (the reference does it too);
  - **the sky has clouds in the reference and had none on the Wii.** The
    game draws the cloud map once into the alpha bytes of the memory its
    depth buffer has (target 01AC0, 24-bit depth: the fourth byte is free)
    and reads it as a T8H texture (1024x256, through a palette) for the
    dome. It is one sprite at the track's load (FRAME 01AC0, 1024 wide,
    FBMSK 00FFFFFF, from a 1024x256 T8 texture: the alpha plane gets
    palette indices of any value). As a GPU target that cannot work: the
    EFB is 640 wide, its alpha is 6 bits and saturates at 0x80. Now
    `mem_plane_draw` rasterises such a sprite (colour masked, wider than
    640) into GS memory once, and the dome's texture is decoded from there
    as CI8 (`tex_decode`: T8H was RGBA8 through the CLUT, a megabyte here,
    keyed by `gs_clut_ver` so decoded again at every CLUT load: with the
    cloud map as a texture that was 58 decodes a frame and 5 fps until T8H
    became an indexed format). Reading it also made `gs_backend_sync` read
    the bloom's bright-pass buffer (target 01A40, 128x448, in the same
    memory) back from the EFB every frame, 896 blocks through
    `gs_write_pixel` (20 fps, "tex 24%"): targets now carry `awrite` (a
    draw wrote alpha since memory last got the content), a read of alpha
    bytes alone (T8H, T4HL, T4HH) passes targets without it, a readback
    leaves memory's alpha alone for them, and `mem_plane_draw` clears the
    mark of the targets it draws over. (The emulated sky, `gxflags 512`, had no
    clouds either: it was never the native strip.) `rt_alpha_from_tex`
    also exists now: a T8H view of a target that is not in the EFB takes
    the alpha from the target's RGBA8 copy instead of a stale buffer (small
    targets 01A40 and 02AC0 use it at times).
- **Slow frames are logged** ("slow frame N: V vblanks (pacer P), ms: ...",
  the first 100 that take longer than the pacer's interval, with the time
  by part). With native sparks the autopilot's crash is 7 frames of 3
  vblanks around the impact (EE 8.6, VU1 3.6, VIF 5-7 with the sparks'
  own maths in it, native 5.2, GX 2.8, GIF 2: 30 ms of work and the wait
  for the tick) and one of 4 after it (EE 33 ms: the restart). 2000 quads
  a frame there: `nw_spark_run` now works a trail point out once (it ends
  one segment and begins the next) and sends floats straight to GX.
- **The crash itself, measured and reduced** (user: "heavy slowdown right
  before a crash"). The worst seen: 2 s of frames of 66 ms around the
  impact, with 7000-7900 particle quads a frame (native 18-20 ms, about
  1.9 ms per 1000 quads) and EE 14-27 ms (the game runs an update pass per
  1/60 s of frame time: long frames cost more EE, which makes them
  longer). Done about it:
  - `nw_spark_run`: `sqrtf` was a library call (no fsqrt on the CPU) and
    most of a quad's time: now `rt_rsqrtf`; the four divisions of a quad
    are `ps_res` and a Newton step (`spark_recip`).
  - crashed cars (all their batches carry the deformation flag 1013.z, so
    none had a display list): `nc_shade_skin` (nw_car.c) is `nc_load`,
    `skin` and `nc_shade` in one pass over the render data, not blending
    vertices without weights (`gxflags2 4` compares it with the old three:
    400 batches, 0 differ; `gxflags2 8` = the old way), and
    `car_pass_both` draws both passes in one draw with the combined TEV
    (`NW_CAR_COMBO_IMM`: positions, colours and both UV pairs sent).
  - Measured on a crash as heavy as the worst one (7300-7700 quads a
    frame) with those two: native 15-16.6 ms where it was 18-20, so the
    quads themselves were the cost, not the square root. Then
    `spark_strips` (gs_gx.c): a particle's trail as one triangle strip, two
    vertices a trail point with the normal there the sum of its two
    segments', 20 bytes a vertex (2D position and UV; the colour a
    register): half the vertices, a third of the bytes. Not the
    microprogram's picture exactly where two quads met at an angle;
    `gxflags2 16` = as quads.
  - Left: frames of 3 vblanks with 14-29 ms of idle time in them after
    the impact (not CPU work: unexplained, perhaps the game's own waits or
    disc reads); one frame of 4 vblanks when the car is put back (EE
    33 ms).
- **Full-screen EFB copies (free in Dolphin, milliseconds each on the
  console).** A race frame made five: the scene as RGBA8 before the haze
  pass (never used: the haze reads the depth as Z8M), the Z8M copy
  (needed), the bloom's half-size RGB565 (needed), and the scene and the
  display buffer each evicted to a texture and drawn back around the
  final blit. The first is gone (a T8H view of a resident target makes no
  colour copy; status line: "blits passed on without a copy, copies of
  big targets": 2 a frame now). The last two stay: the race's blit is not
  a copy. It is the scene times two (vertex colour FF, modulate: the
  scene is drawn at half brightness) blended over the display buffer by
  the scene's alpha (TCC 1, ALPHA 44), TEST 31001. `blit_alias` now also
  takes blits whose alpha test fails with the colour written (FB_ONLY,
  RGB_ONLY), which changes nothing for this one. To do it without the
  round trips: double the scene in place (blend DSTCLR, ONE with white),
  keep last frame's picture as a texture and add it by INVDSTALPHA where
  the scene's alpha is below one -- first find out when it ever is.
  The impact blur of a crash still draws through the GS path into 02D60 as
  a 320-wide buffer without the bloom chain around it (reshape of the
  sphere map twice a frame while it lasts).
- **The sky's whole run natively** (`gs_backend_sky` in gs_gx.c, hook in
  `vif_start_micro` on program 0x46A590 entry 0; `gxflags2 32` = through
  the microprogram with its strip subroutine native as before, `gxflags2
  96` = that with the strips compared, "sky check"). What the microprogram
  is (read from it): one run a layer of the dome for a view (main view:
  lower dome, upper dome, clouds; the frame's reflection view: dome,
  clouds), all computed from the header at 1003..1006 (azimuth step, Y
  scale +-1, strips S, Z; height step and start, pairs N, mode; u0, du,
  v0, v scale; the colour) and the view at 1015..1019: S/4 rings, each a
  meridian strip of 2(N+1) vertices (r = 1 - h^2, y = 2h - h^2, cosines
  from its own polynomial) drawn four times a quarter turn apart; 144
  strips and 3552 vertices a frame, the same every frame but for the
  view's matrix and the clouds' U offset. Here each layer is a display
  list made once (16-bit positions and UVs, 37 KB for the five), the U
  offset a texture matrix, the colour a register. The header table is the
  same for every track (0x4B9CA0).
  Checked: 600 strips against the microprogram's, 0 differ; VU1 from 4%
  of a frame to 2%.
- **Where the drive profile stands after this day** (work/prof_0336.bin
  and .elf): idle 31.2% of samples, work about 22.9 ms a frame (23.6 at
  the day's start). `nc_shade_direct` 5.4%, `vu0_vis` 3.1%, the VU0 call
  1.8%, then memcpy, `put_row`, `draw_setup`, memcmp, `vif_feed_words` at
  1.5% each. By group: the game's code about 21%; the chain's parsing
  about 6%; uploads, CLUTs and texture identity about 9.7% (the largest
  thing left that is not the game's own code). Race memory: MEM2 1.6 MB
  free (855 KB before: the cloud map is no longer a GPU target with a
  640x256 RGBA8 copy).
- **In-place uploads, the reading in full** (`ext_ident`, gs.c). Counted:
  the reading "in turn" found no source changed behind unchanged samples
  in 430 s (status line: "N found changed so far"). Most of the reading
  was sources "back after a frame without them": the reflection views
  are drawn one a frame, so their textures come back every third frame.
  Now a source back within 4 frames with its samples the same is taken
  as it was (`EXT_BACK_FRAMES`), the samples are 16 words (were 5), and
  the reading in turn is every 30 frames (was 12). **Watch for the old
  bug** (a texture keeping the picture of one that was there before:
  parts of the car between two looks, wrong textures later in a race)
  on a long drive around the whole track; the status line would show a
  nonzero "found changed" if the net catches one.
- **Microprograms are looked up cheaply** (`rec_find`, vu.c): the game
  loads one before nearly every group of batches (about 70 a frame) and
  each load had the whole program compared with the recompiled image
  (memcmp from `run2`, 1% of a frame). A program found once for a chain
  and entry (`g_rec_chain`) is taken again when 12 words of each of its
  ranges match. 1.0% -> 0.4%.
- **Sparks, a heavy crash again:** 6463 trail segments a frame on average
  over ten seconds, 21.7 frames/s, native 24% of the frame, as ribbons.
  `spark_strips` now draws trails with every second point when the frame
  before had over 1500 segments and every fourth over 4000 (the ends
  stay; counted before thinning, so it does not flip). Being checked.
- **The pacer held 20 frames/s long after a crash** (a drive's log: ten
  seconds and more at interval 3 with 35-48% of each frame idle): a step
  down to 2 that fails within 300 frames makes the next one wait 240
  frames. Now a frame whose work would fit the shorter interval with a
  fifth to spare counts as "roomy", and 20 of those in a row step down
  at once (`pace_frame`, hle.c). Not yet seen working.
- **Most of a frame's 500 uploads are mip levels** (an analysis of
  `work/gsdump_race.txt` and the upload code, nothing run): about 355
  mips, 110 palettes, 36 rasters. The "split ... not RAM (path 3)"
  counter is misnamed: those are whole uploads of 1 KB or less, which
  `lazy_data` always copies (the car palette fix), and four fifths of
  them are small mip levels. No renderer here reads mip levels (no
  MIPTBP use; GX textures have one level), so they can be left out where
  the game sends a texture (`hle_tex_send`): an 8-bit texture's packet is
  raster, mips, palette into one buffer; a 4-bit one's is raster and mips,
  with the palette (8x3 CT32) as a second entry. `texplace 3` logs
  textures' records (CLUT block at record+16, MIPTBP at +24..+36, record =
  texture + [gp-24088]) against where their transfers go ("texlook"), to
  settle how to tell a palette from a mip. It did: in every texture
  logged the record's CLUT block and MIPTBP fields are exactly the blocks
  the transfers go to (entry DBP plus the block of TRXPOS).
  **Done** (`tex_rest` and `hle_tex_send`, hle.c; `texplace 2` is the
  default, `texplace 1` the way before): a transfer into a MIPTBP block
  is left out, the one into the CLUT block kept, an entry with anything
  else sent as it was. A race frame: 500 uploads (642 KB) -> 107 (55 KB),
  385 mip levels left out, uploads in place 107 -> 1, staged 235 -> 35;
  idle at the same point of the drive 15-19% -> 26-28% (about 3 ms of a
  frame); `put_row`, `lazy_finish`, `ur_materialize` and
  `ext_source_ident` are gone from the profile's top (work/prof_0400.bin).
  Menus, the loading screen and the race look right in screenshots.
  Second step, not done: palettes placed like rasters (a note with the
  CBP, the CLUT cache keyed by content without the CBP): 35 staged
  uploads and about 150 CLUT loads a frame are left.
- **What still goes through the GS path in a race frame** (`gsstat`, five
  frames, after all of the above): VU1 47A060 about 60 primitives a frame,
  476130 about 35, 478260 10; 256 points a frame into the 16x16 corner of
  target 02D20 (a palette drawn point by point; `point_to_mem`); the HUD,
  about 45 primitives a layer; the bloom and blur chain's 190 sprites
  (parsed and skipped); the haze's 40; the final blit's 20. The emulated
  drawing is small now: what is left of the frame is the game's own code
  (a third), then chain parsing, state and the native paths at a few
  percent each.
- **Lists' arrays written out less often** (`dl_touch`, gs_gx.c): the
  game's position and UV arrays that the lists index were flushed from the
  data cache every frame for every list drawn (some 700 lists, 66000 cache
  lines: nothing in Dolphin, time on the console). Now when a list is
  made, when it comes back after frames away, and every 16th frame.
  `gxflags2 128` counts lists whose arrays change in place behind an
  unchanged check: none in a drive of 360 s with a crash; 34 to 84 lists
  a frame are written out now, of some 700. `gxflags2 256` flushes every
  frame as before.
- **The CLUT cache misses less** (`gs_load_clut`, gs.c): it was one entry
  a key in 128, keyed with the CBP, and two loads in five missed (a frame
  loads some 110 palettes 150 times; the game moves textures and their
  palettes about its cache). Now the key is what the blocks hold and how
  they are read, the 256-entry palettes are two to a set, and the
  16-entry ones have a table of 512 of their own: 80-90 reads a frame ->
  5-19.
- **The pacer's quick step down works:** after a crash the interval is
  back at 2 within the next status interval (20 s and more before).
- **What a frame is made of, from an event timeline** (`tl_add` in hle.c:
  V a vblank, C and c the VIF1 chain starting and done, R the next
  frame's work starting, I an idle wait; a slow frame's line in the log is
  followed by "events:" with the last 44, in 0.1 ms): at a vblank the
  main thread runs for about 5 ms more, then the frame's chain is drawn
  (15 ms driving, 23 ms and more in a crash) and with it late the vblank
  that passed meanwhile, then the game works on the next frame (10 ms) and
  waits for a vblank at which that frame is ready and two have passed.
  The crash's frames of 3 vblanks are work after all: chain 23.5 + 9.7 +
  4.8 = 38 ms, over 33.3, and the rest of the third vblank is the idle
  time (I had left `native` out of the sum). In the chain then: native 9
  to 15 ms (crashed cars' lighting and sparks), VU1 3 to 4 (the crash's
  particle programs 46D750, 47C6F0, 4771B0, still emulated), VIF 4, GX 3
  to 5. `nc_shade_skin` takes its normals through the byte table and its
  colours through the fast conversion now, as `nc_shade_direct` does.
  Frames of 3 vblanks with little work and 25 ms idle were seen in an
  earlier log and not in this one: still unexplained.
- **The particle systems natively** (`nw_puff_setup`, `nw_puff_batch` in
  nw_world.c; `gs_backend_puffs`, `puff_draw` in gs_gx.c; the hook in
  `vif_start_micro`): VU1 0x46D750 draws smoke, dust, gravel, fire, glass
  and debris, a pool at a time: a header at 1000..1016, then batches of at
  most 60 particles at TOP, each run by MSCNT, 256 in ITOP with the pool's
  last (entry 0 is called once a frame and only sets the output buffers).
  Kinds 0 (a sprite), 2 (a turned quad), 4 (the same shaded at each corner)
  and 5 (a tumbling shard) are worked out in C and go to GX as quads, the
  pool's six registers written when its first particle is drawn, as the
  microprogram's first packet does; 3 (snow) and 1 (nothing uses it) stay
  with the microprogram, pool by pool. `gxflags2 512` runs the microprogram
  and compares each packet it kicks with what the native path would send
  (a ring of quads: packets span batches): 188724 quads in a drive with a
  crash, none differ (x and y within 1/16 pixel, Z equal, colours within
  1, the same particles kept). `gxflags2 1024`: the microprogram alone.
  What it took to get there: the reciprocal of w has to be a division (an
  estimate's 1 in 10^5 tips the tests on size for a particle at the
  limit); and the products and sums must not be fused (the section is
  compiled with fp-contract off, as vu.c is): a particle's depth is the
  rest of terms a thousand times its size, and fused it came out 1 in 10^4
  apart. The microprogram takes the turn's fraction with the 2^23 trick,
  which below 0 keeps halves (a shard spinning many turns backwards jumps
  there): `np_turn` does the same. A crash's smoke in the replay camera
  looks as before. In the profile of the crash `vu1_46D750` (1.4%) is
  `nw_puff_batch` (0.8%), and its packets are gone from the GS path.
- **The embers natively** (`nw_ember_setup`, `nw_ember_run`;
  `gs_backend_embers`): VU1 0x47C6F0 is the sparks' particles once more
  (the same batches: ITOP groups of four at TOP), each as a small sprite
  where it is now; a crash runs it some 40 times a frame, 80 particles a
  run. Entry 0 comes with an emitter's first batch and sets it up from
  1008..1023, MSCNT (0x01A8) does the others. `gxflags2 2048` compares the
  packets: 189105 sprites, all equal to the last bit (x, y, Z, colour).
  `gxflags2 4096`: the microprogram alone. Two oddities of the microprogram,
  both left out: it does not load a batch's second position (0x0230 loads
  the register after it), so that particle is drawn where whatever was read
  past the end of the run before would be (the compare mode hands that
  register to the native code; without it the particle is at its own
  place); and its extra passes (1023.y, never asked for) would take the
  colour times the last particle's. With the smoke and the embers native a
  crash's VU1 share is 1% of the frame (3 to 4% before) and the packets
  through the GS path 2% (3%).
- **Small things from the profiles of the start grid and of a crash:**
  `spu_tick` (iop_sd.c) did a 64-bit division for every playing voice a
  thousand times a second (`__udivdi3`, 1% of a frame): 32-bit arithmetic
  now, the same numbers. `nc_shade_direct` and `nc_shade_skin` write the
  colour's four bytes and the sphere UV's two with the paired singles'
  quantised store (`st_u8x2`, GQR 3; two instructions for two bytes where
  a conversion went through the stack, masks and shifts): the store stops
  at 0 and 255 where the microprogram's byte wraps, and 1 vertex in 80 of
  a race is out of range, so the stores are for vertices whose light term
  keeps red, green and blue in range (`nc_lrange`, two compares a vertex)
  and the others go the old way. `gxflags2 8192` compares the bytes with
  the old conversions: 44 million vertices, RGB and UVs equal, alpha
  within 1 (twice the alpha is converted, not the alpha doubled).
  `nc_shade_direct` 11.0% -> 9.6% of the grid's frame. `nc_shade_skin`
  blends the two bones' rows as floats
  where they are and takes the weights by a multiplication. `nw_lit_rgba`
  has the power's value at the floor ready (most vertices have no
  highlight): 3.7% -> 3.4%.
- **The car lighting's loop in paired singles** (`nc_shade_ps_loop`, a
  file-scope asm block in nw_car.c; `nc_shade_direct` fills its constants
  and calls it, `nc_shade_direct_c` is the C loop it replaced): the C loop
  was 165 instructions a vertex, 73 of them float arithmetic and 37 loads
  of constants that had no register. With two floats a register the
  light's rows are six multiply-adds for X, Y, Z and the linear term,
  eighteen pairs of constants stay in registers 14..31 (saved as doubles
  around the loop), and the colour and the sphere UV leave through the
  quantised stores; a vertex whose light term is out of range has its red,
  green and blue converted the old way in the loop (`.Lncps_fix`).
  `gxflags2 16384` runs the C loop as well and compares: 41 million
  vertices, none more than 1 apart in any byte (the sums are taken in
  another order). `gxflags2 32768`: the C loop alone. The grid's frame:
  11.0% -> 5.8%, and the grid runs at 29.7 frames a second where it ran at
  28.1. libogc's FPU exception handler saves both halves of the float
  registers (`PPCExcptFpuHandler`: stfd and psq_st for each), so a pair
  survives an interrupt on the console too; GQR 0 is checked to be 0
  (floats) before the loop is used.
  Found on the way: the normals' shortcut in both loops (bytes through a
  table of floats when the ROW is 2^23) never applied, because the cars'
  ROW is 65536.0 (a normal's component is byte / 128 - 1): every vertex
  took the slow way, three sums of bit patterns through memory. Now a
  table of the component for each byte (`nc_rows`, filled when the rows
  change: never, in practice), for any ROW.
  `nc_shade_skin` has it too (`nc_shade_ps_loop2`, the same pieces with
  the normals as floats, positions 12 bytes apart and the UVs as floats;
  the two loops are put together from string macros, numeric labels in
  both). `car_skin_check` (gxflags2 4): 8000 batches, none differ.
- **A crashed car's skinned vertices are kept from frame to frame**
  (`nc_sk_find`, `nc_sk_sum`, `nc_shade_skin_ps2` in nw_car.c). Timed in
  the crash with one car wrecked (121 batches, 5684 vertices, every one of
  them weighted): skinning 2.3 ms a frame (300 cycles a vertex: two bones'
  matrices blended by the vertex's weights, the position and normal
  through the result), the lighting's loop 0.6 ms, the UVs 0.4 ms (two
  conversions from integers a vertex); with three cars wrecked 13700
  vertices, 8 ms. The bones change at impacts only: with a sum over the
  bones a batch's weights name (about 6 of them; not the whole of VU
  memory from 974, which has the camera in it and never repeats), its
  weights, UVs and positions, 19 batches in 20 are the same as the frame
  before. Those take their positions, normals and UVs from three pools
  (384 KB from `plat_alloc`, 12288 vertices; a full pool starts over) and
  only the lighting is done. `nc_shade_skin` at the crash's height: 9.2%
  of the frame -> 3.3% (the sum is over every position word: it could
  sample). `gxflags2 65536`: every batch skinned every frame. Status line:
  "crashed car batches skinned, as the frame before left them".
- **Where a heavy frame goes now** (the start grid, 14 s profiled, no idle
  time: the six cars close up): the game's own code 20%, its VU0 calls
  14% (2400 a frame: `vu0_vis` 502, `vu0_box_test` 490, `vu0_row71` 582,
  the ray tests of 4787D0 at 0388, 0390 and 0550 400 as recompiled code),
  `nc_shade_direct` 10%, the lit world's colours 3.4%, `draw_setup` 3.3%,
  the VIF and DMA walk 8% (`vif_feed_words`, `memcpy`, `dma_run`,
  `unpack_bulk`, the two packet parsers), CLUT loads 3% (`memcmp` from
  `gs_load_clut`, `tlut_lookup`). The grid runs at 28 to 29 frames a
  second for its ten seconds, a crash at 24.
- **What limits a crash now** (work/logs/crash_final_1013.log, the slow
  frames' "events:" lines): the cars' lighting is 4.5% of the crash's
  30 s (10.6% before: the loop in pairs, and `nc_shade_skin` 5.5% -> 1.5%
  with the normals' table), VU1 0.5% (2.7%). A frame's work is serial:
  the chain (16 ms driving here, 20 to 24 at the crash's height with 2400
  spark quads) and the game's own code for the next frame (14 to 15 ms,
  the part before the kick included). I first read the timeline as 29.7 ms
  of work an ordinary frame with 3 ms to spare, and the dips at contacts
  as the sparks taking those 3 ms: wrong, the "spare" was sleep in the
  middle of the frame (the next entry). What is left of the crash after
  that entry is its height: 19 frames of 291 at three vblanks, where the
  chain is 24 ms (VIF 5 to 6 ms, `spark_strips`, the crashed cars).
- **RPC replies were held to the next timer event: the dips at every
  collision** (`sif_pending` in sif.c, the two idle loops in kernel.c).
  The game's main thread makes three calls a frame to the IOP (server
  80001300, functions 80001301, 80001302, 8000131A; more when sounds
  start) and sleeps on a semaphore until each is answered. The IOP served
  a call in microseconds, but its reply sat in the SIF queue: with no
  thread to run, `kernel_idle_wait` slept to the next vblank, timer or IOP
  deadline before `sif_poll` handed it over, up to 16 ms a call. A
  frame's idle time was those sleeps, in the middle of the frame: a crash
  frame with 29.7 ms of work took three vblanks with 13.5 ms asleep, and
  for ten seconds after the crash frames with 12 ms of work did too (the
  three sleeps fell on three vblanks instead of two, and stayed there).
  Now a deliverable packet counts as pending and the idle loops do not
  sleep past it. The scripted crash, frames shown for 2 / 3 vblanks in
  its ten seconds: 106 / 129 -> 271 / 19 (23.5 -> 29.1 frames a second);
  the ten seconds after: 151 / 100 -> 300 / 0; the grid 29.8. Autopilot
  `sifwait 1`: as it was. (work/logs/crash_sif_1013.log against
  crash_final_1013.log.)
  How it was found, for the next time frames wait with work to spare: the
  timeline's vblank events now carry the game's flip state (`V252`: the
  interval, 0x40 the frame's queue not empty, 0x20 DMA the handler started
  still running, 0x10 a frame drawn and waiting for its flip, the vblanks
  counted since the last flip), `K` marks the frame kick going ahead and
  `W` its wait; the sleeps were not in the kick, and a log of where the
  threads block (wait kind, id and return address in `kern_reschedule`)
  showed semaphore 31 under sceSifCallRpc.
  The game's frame loop, read on the way: the vblank handler 0x1F1CE8
  counts vblanks since the last flip (0x4E2841) and, once the interval
  (0x4E17DC) is reached and a frame is drawn (0x4E27CF, set by the queue's
  items 0x40 and 0x41), flips and runs the queue (0x1F1778), which kicks
  the next frame's chains; the frame kick 0x1F2408 waits while the last
  frame's queue is not empty, swaps the lists and runs the queue itself.
  So a chain is kicked from the vblank handler when the main thread was
  ahead, from the main thread when it was behind.
- **The pacer comes back sooner** (`PACE_NEED` in hle.c): frames whose
  work would fit the shorter interval before it is tried were 45, and 240
  after a try that failed; a crash's heaviest two seconds put the interval
  up, and the game stayed at 20 frames a second for four more seconds with
  the work back to normal. Now 16 (three times as many after each failed
  try), and 10 frames with a fifth to spare. The ten seconds after the
  crash: 174 / 84 and 151 / 100 frames at 2 / 3 vblanks -> 300 / 0 in every
  run since.
- **The crash is on the edge now, and the same script gives different
  crashes** (the autopilot's presses land on different frames): its ten
  seconds had 8, 9, 19, 82, 102 and 114 frames at three vblanks in six
  runs. In the heavy ones 12000 draws a frame and 2900 to 3200 spark
  stretches; a frame there is the game's code 15 to 17 ms, VIF 5, GX 4,
  native 7 to 10, GIF 2: 35 to 38 ms, and eight misses in sixteen put the
  pacer's interval up. Sparks are thinned to every sixth point above 7000
  stretches now (`spark_strips`). Compare several runs before believing a
  number from the crash.
- **VU1 microprograms are noted, not copied** (`mpg_note`,
  `vif_micro_words`, `vif_mpg_flush` in vif.c; `micro_differs` in vu.c):
  the game loads one before nearly every group of batches, 38000 words a
  frame, and nothing reads most of them (a recompiled program is known by
  12 words of each range, a native pipeline by where the upload came
  from). An MPG's words stay in the game's memory with a note of where
  they belong; a later load over the same place drops the older note; the
  notes are written out for the interpreter, for a search through every
  program, and when the chain is done. 31600 words a frame noted, 8100
  written out. It saves less than it sounds (copying 150 KB is a tenth of
  a millisecond here): `memcpy` 2.4% -> 2.1% of the grid's frame. Autopilot
  `mpglazy 0`: copied as before.
- **What the VIF and GIF time is** (15% of a frame together): not copies
  but the size of the game's display list: 6900 draws, 70 microprogram
  loads, 21000 words of UNPACK and 5000 qwords of register writes a frame,
  spread over `vif_feed_words`, `dma_run`, the two packet parsers,
  `unpack_bulk`, `gif_path_*` and `gs_write_reg`, 1 to 3% each. Short of
  drawing from the game's scene without its display list there is no one
  thing to take out. (The `memcmp` of 2% that was `gs_load_clut` on a
  cache hit is the next entry.)
- **A palette bound again is not compared again** (`gs_load_clut`,
  runtime/gs.c: `g_clut_slot`, `clut_holds`, `clut_note`). Each cached load
  has a serial (new whenever its entries are read from memory) and the
  hash of its entries; the CLUT buffer remembers, per 16-entry slot, the
  serial it was copied from. A hit whose serial is already in all its
  slots returns at once (165 of 294 loads a frame on the grid): before,
  each was a compare of the whole palette. `tlut_lookup` asks
  `gs_clut_hash(base, n)`, which is the load's stored hash when one load
  put exactly those entries there, and hashes the buffer only otherwise.
  `gs_clut_ver` still changes only when the buffer's content does (the
  T4HL/T4HH textures are keyed by it).
- **Target checks stop at a page test** (`gs_backend_sync`, gs_gx.c:
  `g_rt_pages`, stale when a target is made or reshaped): a bitmap of the
  512 pages the small targets span; 92% of the 600 checks a frame (every
  texture and palette bind) end there. With the palette change: `memcmp`
  1.9% -> 0.7%, `gs_backend_sync` 1.5% -> 0.5%, `tlut_lookup` 0.9% -> 0.1%
  of the grid's frame; idle on the grid 7.0% -> 8.9%.
- **World lighting without the root for vertices at the floor**
  (`nw_lit_rgba`, runtime/nw_world.c): a vertex has no highlight when
  R.D <= lo |D|, which is R.D <= 0 or (R.D)^2 <= lo^2 D.D: no reciprocal
  root, and its alpha is then a function of its intensity byte alone, read
  from a table (`lit_floor`: the last four pairs of the floor's value and
  the least alpha; one table a frame is made in a crash, when the light
  changes, none otherwise). 3.4% -> 2.9% of the grid's frame. The loop in
  paired singles (two vertices at a time) is still open.
- **What a heavy crash is** (the same script gives a light one half the
  time): more cars deformed at once. Light: 2 to 7 crashed batches skinned
  again a frame, 2000 to 3600 spark segments alive, 350 to 360 car batches;
  heavy: 26 to 50 skinned again, 8500 to 8900 spark segments, 470 to 500
  car batches, 12000 to 13000 draws, the skin pools started over 1 to 5
  times. A ten-second profile of a heavy one (4000 call stacks): display
  list 43.5% (cars 10.6%, sparks 8.7%, world 4.1%, smoke 2.4%), render
  build 17.7%, the game's update 11.8% (of it 0x1B8F00, the collision and
  physics step, 5.9%: about 2 ms a frame; user: "the collision testing
  logic itself too is heavy" - it is there, but smaller than what a
  collision spawns), idle 17%. After the entries below, one clean run of
  the spark-heavy kind (11200 segments alive, 14 batches skinned again):
  241 frames at two vblanks and 39 at three in the crash's ten seconds,
  28.1 frames/s, the pacer staying at 2 (before: 140 to 160 and 93 to 106,
  23.5 to 25.3). The kind with 50 batches skinned again a frame has not
  come up since the skinning was rewritten.
- **Spark trails to a tolerance** (`spark_strips`, gs_gx.c). The count-based
  thinning (every 2nd, 4th, 6th point by how many were alive) left points
  2 to 3 pixels off the trail on average and up to 76 at a camera cut. Now:
  the trail's ends, then the middle of a stretch; within `SPARK_TOL` (0.75
  pixels) of the line and the ends no more than `SPARK_SEG` (40) apart, the
  stretch is one segment, else its halves. The tolerance doubles when the
  frame before drew more than 800, 1600, 2400 segments. A stretch whose
  ends and middle are beyond the same edge of the view is not halved (it
  is not drawn). Dead particles (two thirds of an emitter's) are known by
  two compares, the history's times falling. Measured (gxflags2 262144):
  points left out are 0.35 pixels from the drawn trail on average, 5 at
  most; 290 segments drawn of 3000 alive in a scrape, 1650 of 8770 in the
  heaviest crash. gxflags2 131072: every point. Status line: segments
  drawn/alive, emitter runs, particles, alive, points worked out.
- **Crashed cars skinned again only where their bones changed**
  (`nc_shade_skin_ps2`, nw_car.c). What changes in a batch that is skinned
  again (gxflags2 524288, `nc_sk_diag`): always bones, one to four of its
  five to eight, typically one loose part moving every frame for seconds
  (a bone's translation). The entry keeps a sum of each bone and one of
  the rest (`NcSkSum`); while the rest is the same, only vertices whose
  weights name a changed bone are skinned (gxflags2 1048576: the whole
  batch). The batch's sum itself is two running sums now, not a multiply
  a word. Reusing the blended matrix between vertices gains nothing: 354
  blends for 379 vertices (every vertex has its own weights).
- **UNPACK of whole qwords is a copy** (`unpack_fast`, V4-32 with no row
  added, no skip, no wrap: 620 of them a frame, 27000 to 48000 words, the
  particle systems' data). The time moved from `unpack_bulk` to `memcpy`
  (78 words on average, the call's overhead): no gain worth the name.
- **The collision code's VU0 calls.** 0x4787D0 entries 0x0388, 0x0390,
  0x0550 are a segment against a triangle, 380 to 450 calls a frame in a
  race. Written natively (`ray_tri`), checked with `vu0chk 1` (270000
  calls every ten seconds, none differing in any register, flag or data
  word) and then **taken out again: no faster** (1.07% of the grid's frame
  against 0.95% for the recompiled code, whose `rec_run` share fell by as
  much). All the program's registers have to be left as it leaves them, 57
  stores around 96 operations, and the recompiled code does no more than
  that. A version writing only what the game reads back would be three
  times shorter, but that needs proof that nothing reads vf11..vf22 after
  the call. Entry 0x0A50 (the box test with four corners and a height, 40
  a frame, 1800 instructions each recompiled) is native: `vu0_box_test_y`,
  the corners and then `vu0_box_test`'s planes. The lesson for natives in
  C: lanes in arrays with loops over them stay in memory (the first
  `ray_tri` and the skinning were both 1000 and 400 cycles for that); a
  variable for each lane.
- **What a VU0 call costs** (autopilot `vu0stat 1` now times every call by
  entry: "vu0 calls per frame, and what one costs", cycles from the time
  base; 24 entries listed, the 16 before hid some). At the grid: 479610@0138
  (`vu0_vis`, native) 541 calls of 1470 cycles = 3.3% of the frame;
  4787D0@0A18 (`vu0_box_test`, native) 485 of 1079 = 2.2%; 4787D0@0028 and
  @0000 (recompiled: a sphere against a triangle, the top of the program)
  263 of 1171 and 247 of 1083 = 2.4% together; the segment tests @0388,
  @0390, @0550, @03D8 about 590 of 800 = 1.9%; 479610@0920 621 of 71;
  4839E0@0000 12 of 1464; 47F6D0@07D8 12 to 75 of 1732, @0B50 32 to 141 of
  354. The profile's samples could not tell these apart (the skid above,
  and the grid's idle time moves between 5.6% and 9.3% from run to run).
  `vu0_box_test` was the same array-and-loop code as the first `ray_tri`:
  its corners are variables now and its sphere comes from the box's four
  diagonals (e8 +- e7 +- e6; they meet in the middle), not from all eight
  corners; `vu0chk 1`: 245000 calls every ten seconds, none differ. That
  took it from 1079 to 900 cycles only: the rest is `box_test_planes` (the
  planes copied to `pl`, `box_plane8s` four to six times and corner by
  corner where the sphere is within a thousandth of the terms' sizes of a
  plane, `box_corner`, the rows copied back). `vu0_box_test_y` (@0A50) is
  1480 cycles, what the recompiled code took: no gain there either.
  `vu0_vis` (@0138): its eight corners went through the matrix by way of
  the registers' memory (the matrix read again and ACC written for each);
  with the matrix and ACC in variables 1470 to 1548 cycles a call became
  1161 to 1209 (0.7% of a frame), exact over 244000 calls every ten
  seconds. (The same rewrite of a spark trail's point changed nothing:
  the compiler had those lanes in registers already.)
  **Next here:** the planes in `box_test_planes` and `vu0_vis` (copied to
  `pl`, tested four to six times, `box_corner`, rows copied back), and how
  often the sphere decides: `vu0stat 1` now also logs "box planes per
  frame" (decided by the sphere, more with a margin of 1e-5 of the terms
  instead of 1e-3, corner by corner). A margin of 1e-5 is still sixteen
  times the rounding of d - n.p (3e-7 of |d| + |n||p|), so it would be
  safe; whether it is worth it is what that line says. Then @0028/@0000.
- **Where a heavy crash's native time goes** (gxflags2 2097152: the native
  paths timed by part, in each slow frame's line; and the crashed cars'
  steps in the status block). The frames that miss, 36 to 39 ms: EE 13 to
  17.5, VIF 4.5 to 6.2, GX 3.8 to 4.4, GIF 1.6 to 2.6, and native 6 to 10:
  spark trails 1.5 to 4.1 (2.1 to 3.3 after the next entry), crashed cars
  1.4 to 4.7, smoke and embers 1.2 to 2.4, car lighting 1.3 to 2.2, world
  lighting 0.3 to 1.1. A crashed batch (45 vertices): kept from the frame
  before 4300 cycles (its sum 5 words a vertex, the `used` bitmap of its
  bones, 16 multiplies a bone, the copies to `NcOut`, the ADC bytes: 95
  cycles a vertex, more than its lighting at 70); skinned again 255 cycles
  a vertex (130 instructions now, 24 fused multiply-adds); constants 215 a
  batch; the normals' table is never made again. At the peak nearly every
  batch of the crashing cars is skinned again (their bones all move while
  they crumple). Then done: the entry keeps its bones' numbers and a sum
  of each (`nc_sk_rest`, `sk_bone`, `nc_sk_bones`: while the slots' and
  positions' sum is the same they are the same bones, no bitmap from the
  weights; a bone's sum is adds over the twelve words the skinning
  reads): a kept batch 5.9 -> 3.2 microseconds, 8000 batches against
  `nc_shade` 0 differ. Left: the copies to `NcOut` (pointers instead), and
  the skinning itself, 255 cycles a vertex, 1.5 ms at a peak of 100
  batches; in paired singles it would be about 190 (the blend of two 3x4
  matrices does not pair well: 45 operations for 61).
- **Sparks, further.** A stretch whose ends are both in view and within
  twice the tolerance of each other is one segment without its middle
  being worked out; one whose ends are both beyond the same edge of the
  view is dropped the same way. With more than 900 particles alive in the
  frame before, every second one is drawn (by its place in its emitter),
  with more than 1800 every third; back at 700 and 1400, so that the
  sparks do not blink at the edge (gxflags2 4194304: all of them). A heavy
  crash then: 650 alive a frame on average, 299 not drawn, 905 segments,
  2040 points worked out (6300 before for 860 alive); 250 frames at two
  vblanks and 33 at three, 28.3 frames/s. Not seen by the user yet: whether
  the thinner shower at a crash's height is noticed.
- **Tools.** The profiler takes 4096 call stacks (was 1024; 787 KB of MEM2
  while profiling, which has 1.2 MB free in a race). `work/callgraph.py`:
  the static call graph of gen/ (`tree`, `callers`, `under`; indirect calls
  are not in it). In Dolphin a sample lands at the start of the block after
  the one that ran: a straight-line function's time shows in its caller
  (`ps2_vu0_callms.part.0` 3%, `vu0_4787D0` 1.4% are their callees').
- **What a VU0 routine has to leave behind** (`work/vu0live.py`, new). At
  the grid the VU0 calls are about a fifth of the frame (the recompiled
  programs 10%, `vu0_vis` 3%, `vu0_box_test` 2%, their dispatch), as much
  as all other game code. A native routine was held to leaving every
  register as the program does; the tool works out which are read again
  before being written: every COP2 instruction of the executable (by the
  recompiler's own lifter, lanes apart), every microprogram entry (reads
  before writes, what it always writes), calls and returns followed. Its
  commands: the table by entry, `sites` (every VCALLMS and what is live
  after it), `entry E`, `local`, `why F`, `which`, `readers F PC REG..`
  (who reads a register after a call, with the path), `--strict`.
  - There are 36 VCALLMS in 21 functions, no VCALLMSR. Which program a
    function's calls mean is in `SITE_PROG` (found with `which`: the
    program whose reads the function sets up): 0x1618C0 4839E0; 0x21F790
    and 0x21FBB0 47F6D0; 0x26A1C0..0x26A7E0 479610; 0x26AF50 and
    0x272EA0..0x275D20 4787D0.
  - Outside those functions a read of a register the function did not
    write is a store of lanes it does not care about (sqc2/qmfc2 of a
    register of which it set x or xyz); "why F" lists them. Without
    `--strict` they do not count.
  - Never read after any VU0 call: ACC, MAC (no entry reads the flags of
    the run before; the game's COP2 code reads none), CLIP (unused), and
    after the box and visibility tests the corners in vf2..vf5 (vf1..vf7
    xyz for the visibility test), the planes, VI3..VI6, VI9..VI14, I.
    Still counted as read, by paths the tool cannot rule out (returns to
    every caller, any function through a pointer): vf6..vf9, vf19, vf24,
    VI2, VI7, VI8; those are left as the program leaves them.
  - **`vu0_box_test` 950 -> 584 cycles, `vu0_vis` 1183 -> 675** (0.5 ms a
    frame in a race): the dead registers are not made; a plane the sphere
    does not decide is decided by the box's extent along its normal (all
    in, all out, or corners on both sides, clear of 1e-5 of the terms'
    sizes), and only a box within that of touching a plane has its
    corners made and tested one by one. The visibility test puts two
    opposite corners through the matrix, not eight (data 213..220 are the
    corners of a box along the axes; checked on every call, anything else
    goes corner by corner). Per frame 1863 planes by the sphere, 529 by
    the extent, 149 corner by corner (those are entry 0x0A50's, which is
    as it was).
  - Checking: `k_vu0_native` has what each native does not leave behind
    (`dead`, `dead_xyz`, `dead_vi`); `vu0chk 1` compares everything else:
    250000 calls every ten seconds, none differ. `vu0poison 1` (new)
    overwrites what a native does not leave behind (NaNs, 0xDEAD): a race
    with a crash looks and paces the same.
  - **Not done here:** the collision entries of 4787D0 (0x0000/0x0028/
    0x0060 a sphere against a triangle, 1100 cycles; 0x0388.. a segment
    against one, 800): most of what they write counts as read by those
    unruled-out paths, so a native would have to make it all. The world
    batch's test at the game's level (`hle_world_batch` could test before
    calling 0x26AF50) gains nothing unless the function is split at
    0x26AFE4: a batch partly in view would be tested twice.
- **The recompiled code's register traffic: callee-saved registers kept
  out of the context** (`tools/recomp/abi.py`, new; `work/abisave.py`
  reports). The game code is a third of a race frame and its profile is
  flat. 0x26A610 (64 guest instructions) compiled to 537, 100 of them
  stores and 95 loads: it loaded all four lanes of s0..s4 from the context
  to save them on the guest stack with `sq`, and at its end loaded them
  back and stored them to the context, 16 memory operations a saved
  register a call, for values no caller and no callee ever reads from the
  context.
  - `abi.py` follows every function for s0..s7, fp and ra with a small
    state (saved in which stack slot; may it hold the caller's value; may
    it hold the function's own) and gives: **preserves** (the caller's
    value is back wherever the function leaves), **consumes** (it may
    read the caller's value: a variable used before it is set, or code
    that is no function of its own; "raw" when it never saved it),
    **neutral** (preserves by a save of its own and calls only functions
    that preserve and do not consume: the save and restores are left out
    and the register is a C local that never reaches the context; the
    lanes above bit 63, which only the `sq` read, go with them).
  - In `emit.py`: a preserving function's summary does not show the
    register as written, nor (unless it consumes) as read, so callers
    neither store nor reload it; `skip_pcs` makes the save/restore
    instructions no-ops; `private` keeps the register out of every store
    to the context; calls through pointers sync everything but
    `prog.ind_safe` (registers every function whose address is taken
    preserves and none uses raw).
  - Title config: `ABI_EXEMPT` (not called through pointers: the entry
    point, the crash handler's register dump/restore, data that decodes
    as code), `ABI_FOLLOWS` (follow the ABI in a way the analysis does not
    read, looked at by hand: two functions with frames over 32 KB that
    save through a copy of the old sp, two with a frame pointer),
    `ABI_SAVES = False` turns it all off.
  - Result of the analysis: 21787 of 22162 saves left out, in 5761 of 9124
    functions; all ten registers safe through pointers; the generated C is
    12% smaller (108.6 -> 95.3 MB). What does not preserve: the C++
    exception unwinder (0x101FD0, 0x102040, 0x1009F0: never run), crt0.
  - **Measured (Dolphin, the scripted race, work/last_abi1.txt):** text
    17.18 -> 12.37 MB (the DOL 17.6 -> 12.8 MB), MEM1's arena 0-16 KB ->
    4472 KB free. The game code 34% -> 26-29% of a 33 ms frame in the
    drive; the crash interval 300 frames at two vblanks and none at three
    (it was 266/23 at best); and in the stretch after the crash the pacer
    went to interval 1 by itself: **59.9 frames/s**, 590-598 of 600 frames
    in one vblank, idle 10% (EE 7.5 ms of 16.7). The drive in traffic
    before the crash is 19-21 ms a frame: still 30. Every self-check in
    the log passes (VU0 natives, qsort, car lists), the screenshots are
    right.
  - The sources before it are in `gen.old/`, the binary before it
    `work/burnout3_before_abi.dol/.elf`.
  - **Second step (built after the measurement above):** the callee-saved
    FPU registers f20..f31 the same way (`swc1`/`lwc1` saves: 1499 of 1537
    go), and sp: 9113 of 9124 functions leave it as they found it (its
    distance from the entry value followed through `addiu sp,sp,K`), so it
    is not written back and read again after a call (a call site stored 4
    words and reloaded 2; now 3 and none). For that the config got
    `NORETURN` (_exit: abort() runs on into the next function behind its
    call; the C++ unwinder), `JUMP_TABLES` (one table the analysis does not
    find, at 0x353DB8) and a stub in `ABI_FOLLOWS` (0x10E87C, a lone
    `jr ra` whose delay slot is the next function's first instruction).
  - **The second step measured** (work/last_abi2.txt): text 12.08 MB,
    MEM1's arena 4756 KB free; the same scenes, every self-check passing.
    The drive in traffic: the game code 26-28% in the status line (it
    counts VU0 with it), idle 38-43%; after the crash idle 49-51%, the
    work of a frame 16.5 ms: at the edge of the pacer's step to interval 1
    (it needs the work under 17.2 ms for 16 frames; the run before made it,
    this one did not).
  - **The drive's profile now** (work/prof_drive_abi2.txt, 14 s in
    traffic at 30 frames/s): idle 43.4%; recompiled game code 12.3% (it
    was the largest item); VU0 about 10% (its recompiled programs 4.0%,
    `vu0_vis` 1.1, `vu0_box_test` 0.8, and what Dolphin's sampling puts on
    `ps2_vu0_callms.part.0` 1.7 and `rec_run` 1.35: Dolphin's JIT follows
    direct branches, so a "block" ends far behind where it began, and
    those samples are the callers' code before the call and the programs'
    stores before their return); `nc_shade_ps_loop` 2.5, `nw_lit_rgba`
    2.45, `vif_feed_words` 2.3, `draw_setup` 1.9, `memcpy` 1.8, `dma_run`
    1.45, `nw_packet_parse` 1.4, `gs_backend_world` 1.1, `f_0021C780` 1.1.
    For 60 in traffic the frame has to lose about 2 ms more (6% of 33).
  - `run_alarms` was 0.6% of that frame: `kernel_poll` runs some 400 times
    a frame (every interrupt raised asks for it) and walked the 64 alarm
    slots each time. Now it returns at once while no alarm can be due
    (`g_alarm_soonest`).
  - **The state after all of it** (work/last_abi3.txt, the scripted race,
    no switches): 30.0 frames/s through the start and the drive in traffic
    (idle 18% at the grid, 38-44% in the drive); a heavy crash 269 frames
    at two vblanks and 21 at three (29.0 frames/s; its slow frames are 36
    to 39 ms, of which the native paths 8 to 11: sparks, crashed cars,
    smoke); and from 20 s after the crash 59.8-59.9 frames/s at interval 1,
    idle 10-17%. The PC host builds with the same sources.
    `gen.abi/` is a scratch copy of what the generator writes (equal to
    `gen/`); `gen.old/` the sources before the change.
  - **A third step, written and switched off** (`RET_LIVE`, `IND_SUMMARY`
    in the title config): return registers no caller reads not written
    back at returns (`return_values` in emit.py: what a function always
    leaves, what callers read after each call), and calls through pointers
    reading only what the functions whose address is taken read (part of
    the summaries' fixpoint). The generated C shrinks by under 1%: 5000 of
    9124 functions still count as having v1 read after them, and a call
    through a pointer still "reads" v0, v1, t4..t9, f0 and the VU0
    registers, because of how the summaries treat a register a function
    writes on some paths only (it counts as read at entry, to have a value
    to store on the other paths; 548 functions "read" lanes 2 and 3 of v1
    that way) and of lanes stored without being set (sqc2). Making those
    two notions exact (a must-write analysis for the stores at exits; junk
    lanes) would have to come first. `work/../indroots.py` in the session
    scratchpad listed them; not kept.
  - **What is left at function boundaries** (stores to / loads from the
    context in the generated C, counted statically): 326000 stores, of
    them arguments 157000 (89000 lane 0, 68000 lane 1), v0/v1 59000 (half
    lane 1), VU0 registers 23000, sp 26000, t registers 31000; 129000
    loads, of them v0/v1 45000, arguments 28000. **Next there:** lane 1
    where every call site passes a sign-extended value (then the callee
    makes it from lane 0: an interprocedural version of the emitter's
    "wide" state, for arguments and for v0 at returns); v1 not stored at a
    return that no caller reads it after.
- **The game ran at half speed in ordinary racing, and at double speed in
  parts of crashes** (user: "a hitch less than a second before a
  collision"; found on the afternoon of 2026-10-13 with the frame history
  below). The earlier note that the game "runs N update passes by the
  clock" was true of one of three cases only:
  - The race loop (0x132600 state 5) runs the passes kept at 0x51BAC4, each
    1/60 s of game time and one tick of the game clock (0x4E273C). Its
    timer (0x376570, object 0x51BA50: +0 mode, +32 the step in Count
    units = 16.67 ms, +40 the passes the clock asks for, +52 a "one pass"
    flag set from 0x51BA94, 0x51BAD4 and 0x4EB1E0) returns the clock's
    count only in mode 1.
  - Ordinary racing is mode 0: one pass a frame whatever the frame took
    (the PS2 game slows down under 60). At this port's 30 frames a second
    that was half speed. A crash sets the game's own 30 fps flag
    (0x51BAA6: its flip interval 2, two passes at least): right at 30,
    double speed when the pacer showed those frames for one vblank. For
    some seconds after a crash the game uses mode 1, with the clock read
    wherever the loop happens to be: 1, 2 and 3 passes in no order for
    frames each shown for two vblanks.
  - Now `pace_passes` (runtime/hle.c, called from `pace_frame`) writes
    0x51BAC4 itself every frame: the pacer's interval, put right by what
    the screen did (the backend counts flips and their vblanks:
    `g_flip_count`, `g_flip_vb_total`; a frame that was a vblank late is
    made up for by the next one built). Autopilot `passes 0` = the game's
    timer. The status line has "gx: game clock: T ticks in V vblanks":
    100% in a race at 30 and at 60 (50% in the menus, whose step is 1/30).
  - Cost: a second update pass in every 30 fps frame of ordinary racing,
    about 2 ms (EE 26-28% of a frame became 31-35%; the start grid has 8%
    idle left).
  - Not yet seen by the user. The autopilot's drive is twice as fast as
    before: its scrape and its crash come earlier (first sparks near 218 s,
    the crash near 250 s).
- **The pacer** (`pace_frame`): it counts a frame's vblanks by the flips
  (its own count, between two calls in the middle of a build, said 1 and 3
  for two frames of 2); two builds within a vblank no longer start its
  history over; down to the shorter interval with a tenth to spare (a
  twentieth: 15.5 ms of work went to 60 and every eighth frame missed it);
  up from 1 at three missed frames of 16 (it was half of them: between 17
  and 25 ms of work it never went up, and the picture stayed a mix of one
  and two vblanks; the user's start grid had two seconds of that).
- **A frame history around every collision, and the frame's thread's
  waits** (gs_gx.c at the flip, kernel.c; debugging, on for now):
  - "frames around the first sparks (frame N)": 96 frames before and 32
    after the first sparks in two seconds, a line each: vblanks (game's /
    screen's), pacer, passes owed / clock ticks, VU0 calls, particle
    quads, lists made, ms by part, and a wait of 3 ms or more of the
    thread that kicks the frames. Autopilot `fhist 0` = off.
  - "frame N: thread T waited ..." for each such wait of 8 ms (first 80),
    and "wait of X ms (kind K id I) from: ADDR..." with the host return
    addresses (`powerpc-eabi-addr2line -f -e build-wii/burnout3.elf`),
    what the other threads did meanwhile, the alarm it waited for and where
    the last twelve 1 ms ticks found the processor (first 24 after the
    menus).
  - "frame N: update passes ... (timer flag, mode, step ...)" when the
    game's timer changes its way.
  - `work/dis.py ADDR...` prints a recompiled function's instructions from
    the comments in gen/; `-g REGEX` greps all of them.
- **Placed textures are decoded from their transfers** (`placed_src`,
  `placed_tiles` in gs_gx.c; `gs_placed_source` in gs.c). The game sends an
  8-bit texture as a 32-bit image of half its size and a 4-bit one as a
  16-bit image: block for block the bytes GS memory would hold, so a block
  of the image is a block of the texture with its texels in another order
  (one table a pair, from the GS's address functions). Through GS memory a
  256x256 texture took about 3 ms and a frame that met several new ones
  (a collision's first frames, a vehicle seen first: 5.2 ms of "tex" and
  1.6 of "upload" in one frame of the history) was late. Checked against
  the old way: the first 48 of a run (0 differ; `gxflags2 16777216` =
  every one, `8388608` = the old way).
- **Lit world: most lists keep their colours** (`lit_kept` in gs_gx.c,
  `nw_lit_floor`, `nw_lit_rbounds`, `nw_lit_all_floor` in nw_world.c):
  - `x1`: how far above the floor x may be and the alpha byte is still the
    floor table's for every intensity (found by halving on the bit
    patterns when a table is made; 16 tables kept, keyed by the
    constants). The per-vertex loop's quick test uses it.
  - A list whose vertices all stay under it, by the bounds of their
    reflected light (per list and light direction) and of their positions
    against the eye, draws with colours kept behind the list in the list
    arena (`DList.ext`). 43% of the lit lists in the drive; the first 3000
    kept draws of a run are compared with the vertices' own sums (0 wrong;
    `gxflags2 1024` = all, `65536` = never kept).
- **Disc reads no longer stop the game** (`plat_read_guest` in plat_wii.c,
  `disc_read_sectors_guest` / `disc_read_guest`, `kern_wait_host` in
  kernel.c): a reader thread (a real LWP thread; the game's threads are
  coroutines on the main one) fetches a 32 KB block of the image while
  the IOP thread that asked waits as a guest thread, and the block a
  stream will want next is fetched ahead. Before, `sceCdRead` and the
  IOP's `read` held every thread for the time a block takes from the card
  (Dolphin's own log: "request to /dev/sdio/slot0 blocked emulation for
  12181 microseconds"; the music alone is a block a second, sounds that
  start are more): with the processor idle and the kernel's own sleeps
  zero, as the ticks showed during the race's loading. Status line "disc: N
  blocks read by the reader thread (M of them ahead), W waits for it; the
  game held H ms by reads that do not wait" (0 in a race now).
  - **A trap in the wait log:** the game's main thread sleeps three lines
    (190 us) in every update pass so that its other threads run (0x131B90
    -> 0x237050: an alarm and SleepThread), and that sleep often "takes"
    8 or 17 ms. It is not a wait: a vblank fell into it and the vblank
    handler kicked the frame's chains, whose whole render runs inside the
    handler (the ticks find the processor in the car lighting and GX).
    The log now takes the handlers' time off ("wait of X ms and Y ms in
    interrupt handlers").
  - When nothing else can run, an alarm due within 250 us rings at once
    (`alarm_soon` in `kernel_idle_wait`): those 190 us were time asleep
    twice a 30 fps frame.
- **Where the scripted race stands with all of this** (work/last_fh14.txt,
  2026-10-13 afternoon; `work/auto_fh.txt` = the race script without
  `nopad`): 30.0 frames a second in every ten seconds from the grid to the
  end, 300 of 300 frames at two vblanks through the scrape and the crash
  too; game clock 100%; passes and vblanks equal frame by frame in all
  four frame histories; no wait of the frame's thread above 6 ms. Idle 27%
  at the grid, 35-38% driving, 41-51% after the crash. No stretch at 60:
  with a tenth to spare the pacer wants under 15 ms at one pass, and the
  lightest stretch has 16.3 ms at two (it went to 60 there with a
  twentieth, and missed every eighth frame).
  - One frame of the crash has 21.8 ms of game code where its neighbours
    have 9 to 12 (15 frames after the first sparks, 1060 quads: the
    impact). It fits two vblanks here by 1.2 ms. Not looked into.
- **The drive's profile with two passes a frame** (work/prof_drive_p2.txt,
  222-236 s): idle 32.8% (22.4 ms of work). By the call stacks: the game's
  VU0 calls 12.0% inclusive (4.0 ms: 3100 calls a frame, their routines
  2.4 ms by `vu0stat`: 4787D0@03D8 0.45, 479610@0138 0.44, 4787D0@0A18
  0.37, @0028 0.28, @0388 0.25, @0000 0.25, @0550 0.10, @0A50 0.08), the
  frame's chain 20.8% (`vif_feed_words` inclusive: cars 7.4, world 6.7),
  the visibility traversal 0x21C780 8.7% inclusive. By function:
  `nc_shade_ps_loop` 2.9%, `vif_feed_words` 2.5, `draw_setup` 2.3, memcpy
  2.0, `nw_lit_rgba` 1.4 (2.45 before the kept colours), `ps2_sqrtf` 0.8.
  The VU0 calls are the largest single thing now: the collision code runs
  once a pass.
- **VU0 routines compiled a second time, lean** (2026-10-13).
  `tools/vu/vurecomp.py` takes what the game
  reads after a call of each entry (`VU0_LIVE` in the config:
  config/burnout3_vu0live.txt, the table `work/vu0live.py` prints) and
  compiles each VU0 entry in it again as `vu0_CCCCCC_EEEE_l`: at its end it
  stores only the live lanes it wrote and Q (4787D0@03D8: 9 stores for 68;
  ACC, I, MAC and CLIP are never read after a VU0 call), and the compiler
  drops what was worked out for the rest. A run that leaves the code for
  the interpreter stores everything. `rec_run` takes `fn_lean` when there
  is one (autopilot `vu0lean 0` = the full ones); `vu0chk 1` runs the full
  one after each lean one from the same state and compares what was to be
  left ("vu: lean VU0 routines: N runs a frame, M checked, K differ");
  `vu0poison 1` overwrites what a lean run did not leave. `work/vuregen.py
  OUT` makes the VU sources alone (copy what differs into gen/).
  **Run since** (work/last_chk1.txt, last_poi1.txt): with `vu0chk 1` the
  whole scripted race, some 2.5 million lean runs, none leaves a live
  register other than the full routine; with `vu0poison 1` the race is the
  same to the crash (draws within 20 of a plain run every ten seconds to
  250 s; the pictures of the drive and the crash are right). It saves
  little: the drive's idle went from 35-38% to 37-40%. The routines with
  the most calls (4787D0@0A18, 479610@0138, @0920) were native already.
- **What the user's driving showed (2026-10-13 afternoon, work/last_play1.txt:
  seven minutes, eleven collisions in the frame history) and what was done:**
  - **The status block cost a frame every ten seconds.** In five of the
    eleven histories the frame of the "(sums restarted)" line was on the
    screen three vblanks and the next one. `plat_vlog` wrote each line to
    the UART and the SD log file where it was made. Now lines go into a
    64 KB queue and a thread below the game's writes them (`log_main`,
    priority 20; `log_drain` at a fatal error and the end; the watchdog
    shows the queue). Not yet seen in a session the user played.
  - **The frames with twice the game code before a collision were the
    disc reader.** Three histories had one frame with 23-24 ms under "EE"
    (7 frames, 27 and 78 before the first sparks) and the usual VU0 call
    count. A capture was added for such frames (below) and the scripted
    crash gave eight: 4 to 11 of the frame's 1 ms samples in `get_fat` and
    `f_lseek`, the reader thread (above the game's) seeking. A seek in a
    file walks its cluster chain, forward from the handle's place, back
    from the start of the file: a sound starting elsewhere in the image is
    some 50000 table entries (32 KB clusters, 1.7 GB in). The status line
    had it as "seeks over 4963 MB of the image" in ten seconds of the
    crash.
  - **The image is now read by sector** (plat_wii.c, "the disc image by
    its sectors"): its place on the card is read once at boot (FAT16/32,
    512 byte sectors, 8.3 names; a list of runs of clusters, one run in
    the test image) and `file_read` takes blocks straight from the SD
    interface, any block at the same price. The card's volume is mounted
    again on an interface of our own (`sd_remount`) whose transfers take
    `g_sd_lock`, as the sector reads do, so the file system (the log,
    saves) and the reader cannot interleave on the card. Three blocks read
    both ways must agree, and the file system must have been seen behind
    the lock; otherwise (and for USB) the image is read through the file
    system as before, with the seeks. Log at boot: "disc image read by
    sector: N runs of clusters ...". In the scripted race: every block by
    sector, no spike frame from the reader, the first ten seconds' loading
    with 30 waits for the reader where there were 409. **On a console this
    is untested** (the remount of the SD volume in particular).
  - **Tried and dropped: the reader below the game's thread.** The seeks
    then cost only idle time, but a game thread that polls for its data
    without sleeping keeps the reader from ever running: one run hung at a
    loading screen (work/last_fh16.txt: 0 frames a second, EE 100%).
  - A block read ahead that nothing then reads (the one after the end of
    a sound the game loops) was fetched again every three seconds: the
    last eight such are remembered (`g_dc_unused`).
  - The image's handles are unbuffered now (four 32 KB buffers less, and
    no copy of each block through them).
- **Spike frames named** (`work/spike.py LOG [-e ELF] [-v]`): the 1 ms
  ticker keeps pc, lr and six return addresses of every tick since the
  last picture (`plat_tick_take`), and a race frame with half as much
  again under "EE" as the frames before it (15 ms at least, 6 over) is
  logged with them ("spike frame N: game code ..."; 40 a run at most).
  The script names the samples: the innermost game function, all game
  functions on the stacks, where the processor was. What "EE" holds is
  whatever ran while the game's code was the part being timed, another
  host thread included: that is how the reader showed.
- **60 frames a second in the scripted race** now and then after the
  crash (work/last_fh15.txt: 59.8 and 59.4 in two ten-second windows,
  44 in the two between; fh17: 51.5 and 44.4). At one vblank a frame that
  misses is followed by one with two update passes, which misses again:
  the picture is then 30 until the pacer steps up (three misses in 16),
  so a stretch on the edge alternates between the two every few seconds
  (the backoff: 16, 48, 144, 240 good frames before the next try). The
  drive itself stays at 30: 20-21 ms of work at two passes, 15 wanted.
  - With 17.0 ms of work at one vblank a frame, each frame is ready a
    little later against the vblank than the last until one misses it
    (the events of fh18's slow frames: the kick 3.2, 3.5, 4.2 ... 8.4 ms
    after the vblank, then a chain that ends on the next one): 57.7
    frames a second, two repeats a second, and the pacer's "three misses
    in 16" never came together. It now also steps up on `leak` (each miss
    64, a 64th of the sum gone every frame, 150: a miss in 35 frames or
    more often). The long run after it (work/last_fh19.txt, X held to
    440 s): 30.0 through three crashes, 58.9 and 59.8 in two windows of a
    light stretch, back to 30 when the traffic returned.
- **Nothing here has run on a console.** Of today's changes the SD
  volume's remount is the one that could behave differently there; if
  the volume does not come back on the locked interface it is mounted as
  before (then by `fatInitDefault` again), and the image is read through
  the file system.
- **Next, by size** (nothing started): the crash's height (above);
  `nw_lit_rgba` in paired singles (2.9% of the grid);
  palettes placed like rasters (35 staged uploads and about 150 CLUT loads
  a frame: `gs_load_clut`, `tlut_lookup`, the staging); the ribbons of
  VU1 4771B0 (one run a frame from the grid on, a general strip with its
  own clipper) and kind 0 of the particle systems once a compared run has
  had one; the final blit without the two
  EFB round trips (console only); the crash's impact blur natively (it
  goes through the GS path and reshapes target 02D60 twice a frame);
  the section traversal 0x21C780 natively (see above). Unconfirmed on a
  heavy crash: the thinned spark trails and the pacer's quick step down.
  Unconfirmed by the user: whether the lighting still "switches" now the
  sky has its clouds; the look of a crashed car's underside (black in one
  crash, a gravel texture in another).
- **A second session the user played (12.5 minutes, with the native sky,
  the mips left out, the pacer change; work/logs/play_0412.log):** 30.0
  frames/s in 66 of 75 status intervals, none below 26.7 (the start grid,
  20 before, is 29.0 with 1% idle), 55 slow frames, idle in the race 10%
  to 30%, median 25%; the reading in turn found no source changed. What
  the user saw in it is not known yet.
- **A session the user played (12.5 minutes, the build before the native
  sky):** 30.0 frames/s in 61 of 75 status intervals, 36 slow frames in
  all, most at the start; its log is kept as work/logs/play_0320.log.
- **Ruled out for the lighting shifts:** the bloom's threshold and decay,
  bloom frames skipped, the speed blur coming on in a still scene, the
  depth haze pass (Z in alpha, the scene in the EFB and the palette were
  the same in every frame of a drive).
- **Seen:** the autopilot drive is not repeatable enough to compare builds
  by frame rate: the scrape differs from run to run. While the speed blur
  is on, target 02D60 is reshaped twice a frame (the game's blur uses that
  memory as a 320-wide buffer after the cars have used it as the sphere
  map): the chain's draws are skipped, the reshape is not.

### Done on 2026-10-14 (the 60 fps gap measured; libc's copies replaced)

- **The user's rule, stated again: no quality cuts.** I tried the cars' reflection
  views at one drawing in six frames instead of three; the user stopped it
  ("not bs optimizations like halving the quality"). Only exact rewrites, or
  Wii-native methods whose picture is the same. Not reuse of last frame's
  shading, not lower refresh rates (memory: no-quality-cuts).
- **How far 60 fps is, measured** (`work/bench.sh work/auto_grid.txt TAG`: the
  scripted race to the start grid and the first seconds, unlimited speed,
  autopilot `pace 0` = the game's own interval of one vblank, so the frames
  per second are what the work allows; the runs repeat to 0.4 frames/s; the
  windows at 210, 220, 230, 240 s): **51.6 frames/s before, 53.1 after the
  copies below.** 60 needs 16.7 ms a frame; the frame is 19.4 ms. (`work/auto_fh.txt`
  as the script drives a car whose place differs per run: do not compare it.)
  `work/bench.sh` waits for the runner itself; do not start two runs at once
  (a second wiishots.sh kills the first's Dolphin).
- **Done: memcpy, memcmp, memset** (`runtime/memfast.c`, Makefile.wii flags):
  newlib's copy 16 bytes a turn, compare a word a turn and take unaligned data
  byte by byte; they were 5.2% of a frame at the game's 60 fps interval
  (memcpy 3.7: the game's scratchpad DMA `dma_spr` 30%, `unpack_bulk` 16%,
  `pc_cached` 12%, `vif_mpg_flush` 9%, `draw_setup` 7%, `gs_load_clut` 6.5%).
  Now 32 bytes through the floating point registers; +1.5 frames/s.
  `plat_time_us` divided 64 bits (a library call) at every call: not when the
  time scale is 1 (0.9% of that frame was in it).
- **The 60 fps frame, shares of its 19.4 ms** (grid, profile `work/auto_gp60.txt`):
  `nc_shade_ps_loop` 4.7 (cars' light, asm already), `vif_feed_words` 4.2,
  `draw_setup` 3.7, memcpy 3.7 (now less), VU0 dispatch 3.3 + `rec_run` 1.75 +
  `vu0_vis` 1.9 + `vu0_4787D0_l` 1.4 + `vu0_box_test` 1.2 (VU0 all together
  about 11%), `nw_lit_rgba` 2.4, `gs_backend_world` 2.2, `dma_run` 2.1,
  the walker 0x21C780 1.9, `pc_cached` 1.6, and the upload/placement machinery
  about 7% in many small pieces (`ext_source_ident` .9, `gs_write_reg` .95,
  `put_row` .7, `lazy_finish` .7, `gs_place` .7, `mark_rect` .5, `gs_sync_rect`
  .5, `tex_rest` .5, `gs_load_clut` .5, `gs_block_of` .45).
- **What a world batch costs** (autopilot `wstat 1`, cycles per frame at a race
  moment: 517 batches): the game's 0x26AF50 490k (1210 a call, of which the box
  test ~540), the VIF's call 1.05M (parse 278k, draws 648k for 232 batches
  = 2800 a draw, the rest 128k), `gs_backend_world` in all 856k = 1650 a batch:
  `draw_setup` 428k, list lookup 88k, mode/projection/cull 80k, scroll/camera/call
  94k, arrays 55k, draw state 41k, prologue 40k, drawn+flush 31k. The chain a
  batch leaves (dumped with the old `wb:` log): CALL of the render data (qwc 0),
  S+0 = CNT + FLUSHE, [the pending packets of the group, with the first batch
  only], S+16 = CNT + STCYCL 4/4 + UNPACK V4-32 x1 to 1013, the data qword
  {clip code 0/2/10, 0, cull, flag}, S+32 = CNT + MSCNT: five qwords.
- **Counters** (all behind autopilot switches): `wstat 1` the world path's
  cycles and `gs_backend_world`'s stages; `wstat 3` a hash of what each world
  batch is drawn with, 48 frames from a race's first (`wh: frame +K hash ...`:
  the same lines in two builds that draw the same; `work/whrun.sh TAG`; runs
  were not yet checked to repeat); `vu0stat 2` VU0 calls by call site (host
  address after the call: nm names the guest function): the collision's
  4787D0@0028/0000/03D8/0388/0AD8 and 47F6D0@07D8 and the instanced props'
  479610@0138 (574 calls a frame, 600 cycles each) are the VU0 time.
- **Ideas with their measured weight, not built** (exact ones only):
  a native list of a group's batches in the chain (one command, the VIF
  expanding the known effects of the five qwords: ~0.65 ms); a `gs_place`
  that clears only live records (a bitmap per 64 blocks: its loop clears up
  to 4096 records a placement, 0.7%); palettes as notes (the upload
  machinery, ~0.4 ms); `draw_setup`'s texture objects prebuilt per texture
  (GX_InitTexObj + LOD + a 32-byte memcmp per applied state); the
  constants-generation check for `native_projection/camera/cull/scroll`
  (174k cycles a frame); `pc_cached` used in place (no copy of the blocks);
  the instanced props' loop 0x26A610 without the VU0 register traffic; the
  car light's loop is at 95 instructions a vertex (asm).

### Done later on 2026-10-14 (measuring: a benchmark with percentiles; exact micro-wins; two things that did not pay)

- **`work/fb.sh TAG` is the benchmark now** (the user: "keep percentiles too"). The scripted race to the start grid
  (`work/auto_grid.txt`, `nopad`) at the game's own interval (`pace 0`), unlimited speed, autopilot `wstat 5`: the work
  of each of 400 ticks of the game's clock from the 150th (the time of the frame less what the kernel waited), logged as
  `bench: ... work a frame (us): p5 p25 p50 p75 p90 p95 p99 max mean, mean of the lower 98%`; also the median of 16
  stretches. `work/fb.sh` waits for any run to end first and prints the line; the log is `work/last_fb_TAG.txt`.
  What repeats and what does not: the frames are the same ones in every run (`wstat 3`'s hash of what the world batches
  are drawn with is equal frame for frame), but their times are not: runs of one binary differ by up to 5% in the mean
  (bimodal: about 22.5 or 23.6 ms), and p50 is meaningless here (the window is two scenes, 200 frames of the intro at
  16.5 ms and 200 of the start at 29 ms, p50 sits between them: 19 or 24.7 ms by luck). Read p75 to p99 (repeat to 1%:
  p75 29.4 to 29.8 ms, p90 30.8 to 31.6, p99 33.7 to 34.2) and take the least of several runs for the mean. Small
  changes (under 2%) are not seen in it: count their cycles where they happen (`wstat 1` counters, the profile).
- **Done, exact:**
  - `ps2_vu0_callms` takes the usual native call (checked entry, no debug switch on) without the layers between
    (`vu0_fast`, `flags_drain`): 0.2 ms a frame.
  - `gs_place` clears only the records that exist (a bitmap per block: it cleared up to 4096 records a placement): 0.1 ms;
    more in the race start's big textures.
  - `plat_time_us`: a conditional expression `ts_cur == 1 ? x : x / ts_cur` compiled to a 64 bit library division
    always (x / 1 = x, merged); an `if` now. `__udivdi3` was 1.4% of the race start's frames. (Check with objdump that a
    "hot path" really has no `bl __udivdi3`.)
  - memfast (committed earlier).
- **Built, measured, taken out (no gain):**
  - the world batches of a group as one chain command (`VIF_NATIVE_LIST`, a list entry in place of the five qwords
    0x26AF50 leaves, the VIF expanding the same steps): the game's function is 1210 cycles a batch of which the box test
    and its VU0 register traffic are the bulk, the five qwords are ~100 and the VIF's four tags and a dozen words were
    not worth the list (114 of 460 batches went through it: 9000 cycles a frame less). The code is in the session
    scratchpad (nwl_game.c); a trap found on the way: a list command must be checked as still being what was written
    (the chain buffers are reused, a stale "open command" was extended and the VIF misparsed what followed).
  - 0x26AF50 natively, code for code, with the box test straight from memory (live registers from
    `work/vu0live.py sites`: vf6 to vf9, vf10.w, VI1, VI2; checked equal over 3000 calls): 50000 cycles a frame less
    (0.07 ms), nothing in the benchmark.
- **Where the frame's time is** (the race start, profile `sh work/pf.sh 181 5`, `work/byfile.py`): game code 19%,
  gs_gx 15%, VU0 (the dispatch, the natives, the recompiled collision) 14%, gs.c 10%, vif 9%, cars 8% (`nc_shade_ps_loop`
  alone 6%), memfast 3.5%, iop_sd 3.5%. No single piece over 6%. Trap of the scripts: a command whose own argv has the
  word "wiishots" or "Dolphin" matches `ps -ef | grep -q "[w]iishots"` and waits forever: put loops in a script file.

### Done in the night of 2026-10-14 (the debug time split was 2.8% of a heavy frame; the remaining pieces, weighed)

The user: "address the remaining avenues". The profile of the race start is flat, so each piece was weighed in cycles
first (counters, below); what paid is kept, what did not is out.

- **The time split in the status line cost 2.8% of a heavy frame, and is off by default now.** `pt_enter`/`pt_leave`
  (runtime/rt.h) read the time base at every change of category: 2800 to 3200 changes a frame in the race start,
  and `target_time` (gs_gx.c, per draw setup, 963 a frame) summed the categories at each. Measured in one binary,
  alternating (`work/fb4.sh`: `work/auto_grid_pt.txt` has `ptstat 1`): **mean work a frame 28.77 and 28.79 ms with
  it, 27.96 and 27.98 without (-2.8%)**, p25 27.4 to 26.0 ms, p50 29.3 to 28.5. `draw_setup` in `wstat 1`'s stages went
  from 449k to 314k cycles a frame. On: autopilot `ptstat 1`, the frame meter (`meter 1`) and `fhist 1` (which is
  off by default now; the frame history of a collision needs the split) switch it on; the status line says
  `(time split off: autopilot "ptstat 1")` when it is off. **Look for this kind of thing first: a debug aid in a hot
  path costs more than any exact rewrite found in the profile.**
- **`GX_SetCurrentMtx` through a cache** (`set_mtx`, gs_gx.c): libogc marks its state dirty at each call, the same
  matrix or not, and writes the XF index register at the next draw; the camera's matrix was set 500 times a frame.
  And the camera compare in `native_camera` is twelve words, not a `memcmp` call. "Scroll/camera/call" in the world
  stages: 102k to 67k cycles a frame (0.05 ms). Exact (the only `GX_Init` is `gx_init`'s).
- **The benchmark's window moved to ticks 600 to 900 of the game's clock** (`wh_bench`): before tick ~560 the
  intro's camera is not the same in every run (220, 306, 463 world batches at tick 357 in three runs of one build), so
  the old window (150 to 550) had a mean that swung by a fifth between runs and a p50 that meant nothing. From tick
  600 the frames are the same in every run (`wstat 3`'s hashes are equal) and all heavy: after this night's changes: p5 21.7 ms, p25 25.8,
  p50 28.3, p75 29.7 to 29.8, p90 32.6 to 32.9, p99 33.9, mean 27.83 and 27.85 (two runs; before them 28.77 and
  28.79: -3.3%, p50 -3.5%, p25 -5.5%). 60 frames/s needs 16.7 ms.
- **Counters added (autopilot `wstat 7`)**, per frame at the race start, all cycles:
  - `apply_state` (the GX state of a draw; 200 to 390 applied a frame, ~2000 cycles each): decode+blend 113 a call,
    `rt_for_texture` 184, `gs_backend_sync` 269, `tex_lookup` 235, `tlut_lookup` 130, texture object + load 430, TEV 223,
    pixel state 97, placement + scissor 130. Whole: ~0.8 to 1.1 ms a frame. Nothing in it over 0.1 ms to gain (the
    texture object cached per texture: ~60k cycles; a table for `rt_find`: ~40k; a word scan in
    `gs_backend_sync`: ~50k).
  - the draws that reach the backend at the race start: 256 points (the sky's palette, into GS memory by
    `point_to_mem`, ~700 cycles each: ~0.25 ms), 68 lines, 1 sprite. The status line's "6400 draws" are the native
    batches, counted when drawn.
  - `pt_enter` calls a frame.
- **VU0 by call site** (`vu0stat 2`, race start, cycles a frame): instanced props' visibility 479610@0138 536 calls x 619
  = 332k (already native, `vu0_vis`); the world batch's box test 4787D0@0A18 470 x 549 = 258k (inside `f_0026AF50`, native
  box test exists); 47F6D0@07D8 from `f_0021FBB0`: 69 to 109 x 1472 = 100k to 161k (recompiled VU0 code, no native);
  the collision's 4787D0@0388/0390/0550/03D8 together ~390k (tried natively before: no gain); 4787D0@0028/0000 ~100k.
  All of VU0: ~1.5M cycles (2 ms, 7%) at the race start.
- **Where the frame is (stack samples)**: 54% of the samples are inside the VIF1 chain (`dma_vif1`), 29% in the game's
  frame render (`f_00276300`); the game's own code is 19% of which 11% is render submission (0x21xxxx 5.4%, 0x26xxxx
  4.5%, 0x1Fxxxx 1.2%); physics/AI are 7% spread over hundreds of functions. Sound mixing 2.6 to 3.7% of the time
  (`decode_at` 1.3, `voice_stretch` 1.1).
- **Tried and taken out**:
  - a cache of decoded sound blocks (the key: the 16 bytes and, for the filters that predict, the last two samples):
    0.5% to 6% of the blocks hit (167 of 31660, 4242 of 69000 in 10 s): the sounds do not repeat block for block. Out.
  - (earlier today: the batch list command and the native 0x26AF50: no gain, see above.)
- **What is left, each under 0.3 ms (1%) of a 28 ms frame**: a native 47F6D0@07D8, nw_lit_rgba in paired singles (done
  2026-10-16, bit for bit: ps_madd is fused like fmadds), the sky palette's points in a run, prebuilt GX
  texture objects, constants-generation checks, `pc_cached` in place (630 cycles a packet, 590 packets a frame).
  **The frame is ~28 ms of work in this window and 16.7 ms is the target: the exact pieces left add up to 3 to 5%,
  not 40%.** What would: a different way to take the world batches (no chain, no VIF: the game's walker handing the
  native renderer its batches directly), or the sound mixer on the DSP (3% of the time), or fewer cars' vertices
  (not exact). And remember that Dolphin's guest time counts instructions: cache misses on the real console (MEM2,
  the 16 sampled words of `ext_source_ident`, the guest vertex data) do not show in it.

### Wii hardware tricks: none built, none can be measured in Dolphin (asked 2026-10-14; pushed as 726d9d3 just before)

A grep of runtime/ and platform/wii/ finds no `dcbt`, `dcbz`, locked cache, L2 or DMA use. The only Wii-specific part
today is the memory split: the 32 MB of PS2 RAM in MEM2 (`plat_alloc_guest_ram`, fixed at 0x90100000), the recompiled
code, libogc and the VU/scratch data in MEM1. **Dolphin's guest time counts about one cycle an instruction and has no
cache model**, so these can look neutral or a little worse in the benchmark and still win on the console. The rule "verify
speed before keeping" cannot be applied to them here: put them behind a switch (autopilot or compile-time) and A/B them
on a real Wii. Order to try:

1. `dcbt` prefetch (the 750 has no hardware prefetcher; a miss stalls the loop at each 32-byte line). One instruction per
   iteration, a touch four lines ahead, harmless if wrong (cannot fault). Where: the car light loop `nc_shade_ps_loop`
   (positions 16 bytes apart, the normals' bytes), `nw_lit_rgba`, the big copies in `runtime/memfast.c` (`dma_spr`'s
   scratchpad DMA, `vif_mpg_flush`, `unpack_bulk`), `pc_cached`'s guest-word validation.
2. `dcbz` on destination lines a copy or clear overwrites whole: a store to an uncached line reads it first; `dcbz`
   skips that and about halves the traffic. Only for 32-byte-aligned, completely overwritten lines in cacheable memory
   (a partial line loses its other bytes): `memset` to 0 first, then `memcpy` after a head copy that aligns the
   destination.
3. The 16 sampled words in `ext_source_ident` (runtime/gs.c; ~115 texture placements a frame, each word a different cache
   line of MEM2): issue the 16 `dcbt`s, then read, so the misses overlap.
4. The locked cache (16 KB of the L1 data cache as a scratchpad: libogc `LC_*`): the hottest small tables so streaming
   data cannot evict them: `g_nrm_f` (3 KB), the VU1 constants at 1005 to 1019, the sound's lookup tables.
5. Hot small structures into MEM1 (lower latency than MEM2; the code uses about 17 of its 24 MB): parse-cache entries
   (`g_pc_world_e`, `g_pc_car_e`), display-list headers, the GS block version arrays.
6. The hot recompiled functions at `-Os` instead of `-O2` (`PS2_HOT`, config `HOT_FUNCS`): Broadway's 32 KB instruction
   cache against 12 MB of code; fewer instructions are not what counts there.
7. The car light loop software-pipelined (two vertices interleaved) to hide the FP latency of its one serial chain:
   probably does not fit in the 32 FPRs (the loop keeps 18 pairs of constants in them); Dolphin shows nothing either way.
8. Sound mixing on the DSP (AESND voices): 2.6 to 3.7% of the time. Big and risky: the game reads the voices' positions
   back (NAX) and the engine's sound is made by the mixer (`engine_channel`, `voice_stretch`); the user hears any change.
9. GX side: state changes (`apply_state`, 200 to 390 a frame) and the EFB copies for the reflection views cost GPU time
   that Dolphin does not charge the way the console does; count them when a console run exists.

### Done on 2026-10-15 (exact speedups: car batches, GX state, CLUT loads, normals; tools to measure them)

The user: "polish the performance to perfection", then "make the optimizations as best as you can". Everything below is
exact (the same GX commands' effect, the same bytes) and **all of it is behind autopilot `newopt 0`** (`g_opt_new`, gs.c),
except two rewrites that are exact by construction (`rt_find`'s table, `gs_backend_sync`'s page test). Nothing is
committed yet.

**The benchmark** (`work/fb.sh TAG`, the race start, ticks 600 to 900 of the game's clock, `pace 0`, unlimited speed):
- First the benchmark itself: `wstat 5` ran the world batch's 76-word hash (`wh_batch`) and eight clock reads per batch;
  now only `wstat 3` hashes and only `wstat 1` times the world path (vif.c, hle_render.c, gs_gx.c `WST`). Mean 27.84 ->
  26.87 ms with no change to the game: **compare only with runs from 2026-10-15 on.**
- Then (two runs each): 26.87/26.86 -> 26.30 (car caches, texture memo, `rt_find`) -> 26.15/26.20 (CLUT repeats, draw
  state dirty only on change) -> 25.88/25.84 (normals' copies) -> 25.88/25.86 (pass 2 by list; VU0 block generations).
  p50 27.2 -> 26.1 ms, p90 30.5 -> 28.6, p99 33.6 -> 30.7.

**What was done** (gs_gx.c unless named):
- **Speed blur** (`post_speed_blur`): it read its 32x32 ramp texel by texel every frame (`gs_texel_rgba`, 1024 calls)
  and evaluated the mask point by point against every sprite. The ramp is kept while the blocks under it (relative to
  TBP: the game's texture cache moves it every frame), the CLUT entries and the shape fields of TEX0 are the same
  (`speed_blur_ramp_same`, compared whole); the grid is made sprite by sprite, first sprite wins (`speed_blur_grid`,
  the same expressions: 200 grids checked against the old function, 0 differ) and reused when its inputs are equal.
  The ramp is now read 0 times a frame; the grid is still made each frame at speed (the sprites move).
- **Car batches** (`gs_backend_car`; counters: autopilot `wstat 8` gives cycles per step and per path):
  - the object matrix into PNMTX1 only when it differs (`car_mtx_load`; `g_nst.car_valid`, dropped by `native_camera`
    and `native_instance`);
  - `car_combo_ok` remembered by the draw state's `seq` (a register write gives a new one) and the target found
    (`g_cok`); `native_projection` compares its inputs first (`g_nst.pin`);
  - `native_mode(NW_CAR)` only for passes sent immediately: every list pass set it and then `NW_CAR_DL` over it (two
    vertex descriptions a pass; "passes' draws" 440k -> 293k cycles a frame);
  - pass 2 of a two-draw batch through the batch's own display list (`NW_CAR_ENV_DL`: the sphere map UVs as the bytes
    the shading made, by the list's TEX1 index, 8 bits of fraction, which is what was sent as floats);
  - **the normals from a copy in host order** behind each car list (`car_dl_get`, 4 bytes a vertex, the guest's bytes
    xor 0x80; two normal words joined the list's check): the game's rows make a byte k into (k - 128) / 128 exactly
    (ROW 65536, offset 65537), so `nc_shade_ps_loop3` reads them through GQR 5 as signed bytes scaled by 2^-7: 4
    instructions a vertex where the byte and table way took 19. `nc_rows` finds the scale or falls back to the table;
    the first 300 batches are also shaded the old way and compared ("car normals' copies: 300 batches checked against
    the table (0 differ)").
  - tried and taken out: `nc_matrix` kept by its 32 input words (only 121 of 471 batches a frame have a new one, but
    the compare costs what the product does).
- **GX state** (`apply_state`; `wstat 7` counts draw setups by caller and the memo's hits): a texture memo of four
  entries (`TexMemo`: TEX0, TEXA, TEX1, CLAMP while `gs_ver`, `gs_clut_ver` and `g_tmemo_gen` are unchanged; the
  generation moves when a texture or a TLUT slot is given up; a hit marks the texture and TLUT used, as the lookups
  did), about 200 hits a frame in 350 textured applies; `gs_backend_sync` still runs first (it may read a target back,
  which moves `gs_ver`). TLUT0 is loaded only when it holds something else (`tlut_load`), the texture matrix has its own
  validity (`g_gxc.tm_valid`), `rt_find` is a table by FBP (`g_rt_at`, kept by `rt_fbp_set`).
- **GS** (gs.c): a CLUT load that repeats one of the last two (the same CLUT fields of TEX0, TEXCLUT, `gs_ver`,
  `gs_clut_ver`) returns at once (`g_clut_last`: 193 of 330 loads a frame in the race); a register write of the value
  the register has does not make the draw state be captured again (`gs_write_reg`: draw setups checked 549 -> 469 a
  frame).
- **VU0 program lookups** (vu.c `rec_find`, `vu_micro_written_at`): micro memory remembers which 64-byte blocks were
  written when, and a program found for an entry is kept when none of its blocks was written since. Measured: 0 or 1
  a frame kept: the 9 to 18 program swaps a frame do overwrite the code (it costs nothing; the real fix is item R.1c).
- **VIF NOPs** (vif.c `vif_feed_words`): half of the 9000 VIF1 commands of a race frame are NOPs (`wstat 9` counts
  commands by code: 00 4789, STCYCL 891, UNPACK V4-32 882, MSCNT 603, FLUSHE 595, DIRECT 402, FLUSH 226, OFFSET/BASE
  147, MSCAL 145, MPG 71); zero words are now skipped in a loop: 25.86 -> 25.73 and 25.71 ms.

**Tools added:**
- `work/st.sh TAG "LINES"`: the benchmark's race with autopilot lines (e.g. `"0 wstat 8"`), log `work/last_st_TAG.txt`.
- autopilot `freeze N` (with `wstat 5`): in a race (mode word 0x4E2CFC 0), the first frame at the game's tick N stays on
  the screen and its display buffer (YUV 4:2:2) goes to `frz.bin` on the SD image; `work/frz.sh TAG TICK [DOL]` runs it
  (with `XFBToTextureEnable = False` in the test profile's GFX.ini for that run only, else Dolphin leaves the buffer
  empty), `work/yuv2png.py` makes a picture, `work/pxcmp.py` compares two. One build repeats itself byte for byte;
  the old and the new build gave the same frame at tick 700 except the HUD's message banner (it follows the frames
  since boot, and the new build reached the race a frame later). By tick 800 the cars are elsewhere: **the AI is
  randomized by timing, so pictures of two builds cannot be compared after the first seconds** (the user said so);
  use checks inside one binary (the patterns above) or `newopt 0` runs.
- counters: `wstat 7` (draw setups by caller, the texture memo), `wstat 8` (car batch steps), `wstat 9` (VIF1 commands).

**Where the race-start frame is now** (profile `work/pf.sh 181 5`, 2026-10-15, before the NOP skip): game code 21%,
gs_gx 14%, vu 12.8% (dispatch, natives, the recompiled collision code's tails), gs.c 10.3%, vif 9.3%, cars 7.4%
(`nc_shade_ps_loop3` 5.3%), memfast 3.6%, sound 3.1% + 1.2%. Per frame: world path 1.09M cycles in the VIF's call (parse
301k, draws 641k) + 536k in the game's batch function (468 calls, 251 batches); car batches 1.65M shading + about 1.6M
in their setup; draw setups 1270 (GS path 140/52/38 calls/checked/applied, world 585/127/127, cars 553/289/274),
`apply_state` about 1200 cycles each (TEV 270 when its key changes, texture object and load 370, pixel state 105,
place and scissor 120, decode and blend 110).

### Done later on 2026-10-15 (the collision code's VU0 tests; item R.1)

All behind autopilot `vu0coll 0` (= the recompiled routines; separate from `newopt`). Benchmark, alternated in one
binary (`work/fbcoll.sh`, `work/auto_grid_nocoll.txt`): `vu0coll 0` mean 25.74 / 25.82 ms (p50 26.03 / 26.10, p75
27.10 / 27.31, p90 28.56 / 28.66), on 25.50 / 25.50 (p50 25.60 / 25.70, p75 26.66 / 26.84, p90 28.30 / 28.43): -1.1% on
the mean, -1.6% at the median.

- **0x273070's sphere tests (4787D0@0000/0028), decided as misses without the routine** (`hle_coll_sphere_call`,
  `coll_sphere_miss` in runtime/vu.c). The first version (box distance plus a margin) was replaced before it ran: the
  routine's edge test takes |a|^2|b|^2 - (a.b)^2, a difference of huge terms for a small or thin triangle far from the
  sphere, so the routine itself can say "hit" where the box says no. The decider instead does the routine's own
  operations in its order and its flag logic (MAC flag words ANDed bit by bit: sign with sign, zero with zero): a
  vertex inside, the nearest vertex's region, the edges' lines and sides; where that says miss, the routine says miss
  whatever its plane test (square root, division) gives. At the race start 432 of 466 calls a frame are decided;
  checked: the first 20000 decided calls and one in 256 after also run the routine (0 wrong in every run; a
  disagreement turns deciding off and is logged). A decided call leaves VI1 0 and the decoded vertices (what 0x273070
  reads); if the function's last call was decided, `hle_coll_sphere` (the OVERRIDE of 0x273070) runs the routine on
  its inputs when the function returns, which leaves every register and the stack record's vf8 as the original.
- **How the decider knows its caller: config `VCALLMS_SITES`** (new in tools/recomp: lift_vu0.py, emit.py,
  analyze.py, __main__.py) makes the recompiler call a named native at a given VCALLMS address. First tried: matching
  `c->r[29]` against 0x273070's frame; only 15% of the calls matched, because **the recompiled code keeps guest
  registers in C locals and writes sp back only before calls**, so `c->r[29]` inside a function is stale (and inside a
  hit's callback it would have matched the caller: unsafe).
- **Natives for the segment tests 4787D0@0388/0390/0550/03D8/03E0** (`seg_tri` and its five entries; f_00273400,
  f_00273BF0, f_00274C90; about 900 calls a frame). The 2026-10-13 native of these was no faster because it left every
  register; these write only the lean routine's set (its `done:` stores in gen/vu0_4787D0.c: what vu0live found may be
  read), by the program's operations in order: decoded vertices, vf19.x, t/u/v over det (vf24, or vf8 and the point in
  vf9 for 0x0550), VI1/VI2, VI8 (the OR of six MAC words), Q = 1/det. `vu0chk 1` race: about 2.7 million native calls
  checked, 0 differ.
- **The native check now compares against the full routine and compares Q.** Its first-100 calls ran the lean
  routine, which leaves dead registers stale where a native writes them: that was the "34 differences" at the first
  box tests (and the 0x0A50 "differs in VI/data") in every run since the lean routines came in. Now 0 for all natives.
- `g_coll_stat` was used before its definition (the earlier, unbuilt version did not compile).

### Done on 2026-10-16 (night): exact pieces of R.0's list, behind autopilot `opt3 0`

Benchmark, alternated in one binary (`work/fbopt3.sh`, `work/auto_grid_noopt3.txt`; that build still had the sound
memo below, ~0.15%): `opt3 0` mean 25.30 / 25.25 ms (p25 23.77 / 23.81, p50 25.57 / 25.48, p75 26.77 / 26.62), on 24.99
/ 25.03 (p25 23.14 / 23.23, p50 25.13 / 25.26, p75 26.56 / 26.59): -1.1% on the mean, -1.3% at the median, -2.6% at
p25; p90-p99 moved within their run-to-run 3%. The props' colours came after it: -0.38% in the profile.
After the lit world's paired loop (later that night, same switch, `work/fbopt3b.out`): `opt3 0` 25.28 / 25.29 ms
(p50 25.50 / 25.52, p75 26.61 / 26.99, p90 28.48 / 28.84), on 24.81 / 24.81 (p50 25.12 / 24.93, p75 26.12 / 26.24,
p90 28.00 / 28.36): -1.9% on the mean and at the median for the whole of `opt3`.

- **The parse cache answers in place** (`pc_cached`, `nw_packet_ref`/`nc_packet_ref` in runtime/hle_render.c;
  `vif_defer_call_in`, runtime/vif.c): a kept packet's batches were copied out of the pool (`memcpy`, ~0.3% of the
  frame); now the caller reads them where they are (the pool's copy, or the parse's own blocks), valid until the next
  call; the inherited CL/WL/MODE/ROW are written into the pool's copy (they are written at every hit anyway). No
  pointer to a batch outlives `vif_defer_call_in` (the deferred one is copied, `v->dbatch`/`v->dwbatch`). The words a
  parse read are kept as offsets in guest RAM (`PS2_RAM_MASK` applied: what `LW` reads, the I/O checks being out of
  release builds), and the check of a kept packet is a branch-free loop, the RAM base in a register: 6 instructions a
  word (it was 8; GCC's own unrolling made 8.5, so `#pragma GCC unroll 1` and pre-incremented pointers for `lwzu`).
- **`dl_get` reads the batch through its pointer** (gs_gx.c) instead of copying the NwBlock.
- **The VIF1 command ring for `log_native_run` is written only with `nlog 1`** (`vif_command`): it was two stores
  a command, ~9000 commands a frame, for a log that stops after its first lines.
- **The lit instanced props' colours are kept** (`inst_kept`, `inst_lit_rgba` in gs_gx.c): an instance's colours
  are a function of its list's normals (in the render data, beside the positions the list already trusts) and six
  words of its record (ambient, diffuse, light direction, alpha). 128 arrays of up to 224 colours (115 KB of MEM1,
  memalign at the first use), two places per hash of list + six words; a place is written again only three frames
  after its last draw (the GPU may still read it), else that instance's colours are made for the frame as before.
  The first 3000 kept arrays taken are compared with the sums (gxflags2 1024: all; 65536: none kept). At the race
  start: ~40 instances a frame, all kept, ~1 written per 100 frames, 0 wrong. `gs_backend_world` 1.62% -> 1.25%.
  Status line: "gx: lit props per frame".
- **The lit world's colour sums in paired singles** (`nw_lit_ps_quads`, `lit_rgba_ps` in runtime/nw_world.c; the
  scalar loop is kept as `lit_rgba_c`, unchanged, as the reference). The quick path (a vertex at its floor: ~93% of
  them) two vertices a register, four a turn: the normals' bytes as floats by `psq_l` through GQR 6 (bytes with a sign,
  scale 0: exactly `(float)(int8_t)`), positions by `psq_l` and merges, and the sums by the instructions gcc compiled
  the scalar loop to (it is outside the file's `fp-contract=off` region: nd = kz nz + (kx nx + ky ny), r = n nd - d,
  D.D = dz dz + (dx dx + dy dy), R.D = rz dz + (rx dx + ry dy), fused where gcc fused, operands in the same places):
  `ps_madd` is fused and rounded once like `fmadds`, so the lanes are bit for bit the scalar results (the 2026-10-14
  note "would differ in the last bit" was wrong). The colours' bytes: three `lwbrx` a group of four (the guest's byte
  order) and shifts. A vertex above its floor is stored without its alpha, with R.D and D.D, and listed; C finishes
  it with a copy of the scalar branch (`lit_above`). The first 6000 lists and one in 64 after run `lit_rgba_c` too and
  are compared word for word (a difference is logged, the scalar result used, the paired loop turned off): 12245
  lists, 0 different. `nw_lit_rgba` 2.23% -> 1.55% (quads 0.91, the C part 0.51, the checks 0.10); the C part is
  mostly the ~860 vertices a frame above their floor (the same work as before) and `lit_floor`.
  Status line: "gx: lit world's colour sums in paired singles".
- **`lit_floor` remembers the table it found last** (a set of constants is in one table at most): it was a search
  of 16 tables, ~320 calls a frame, 0.22%.
- **DMA tags: empty transfers and NOP-only tags skip the VIF** (`transfer`, `vif_tag_sink` in runtime/vif.c, behind
  `opt3`): of ~3200 tags a race frame, ~1900 carry no qwords (CNT 0: VIF codes in the tag alone, as the world
  batches' state qwords) and were still handed to `vif_feed_words` with n = 0 (its prologue saves 14 registers), and
  ~550 carry two NOPs with no command waiting for data (now: code 0, stage 0, what the NOP path leaves). Profile:
  `vif_feed_words` -0.70, `dma_run` -0.40, `vif_tag_sink` +0.54 (its own symbol now; mostly Dolphin crediting the
  loop's preceding block to it); benchmark -0.04 ms, inside the noise. `wstat 9` reports both counts.
- **The structural items weighed (the user: "continue with the remaining"), measured before building:**
  - *Recompiler, upper halves of arguments* (the 68000 static lane-1 stores): the hot recompiled functions have few
    calls (0x21FBB0 1 call in 235 guest instructions, 0x26AF50 none, 0x26BAA0 none, 0x1F3080 2 in 336); only the
    walker 0x21C780 (19 calls: 105 lane-1 stores, 33 loads) and 0x21E6F0 carry much. Game code is 22.3% of the frame
    over 549 functions (the top 7 a third). Well under 1% to gain for a change to every function: not built.
  - *Sound on the DSP*: the DSP is 16-bit fixed point and the mixer is floats; not the same sound bit for bit.
    Not started (needs the user's say).
  - *Drawing at the game's level*: what is left of reading the PS2's stream is spread out: the VIF/DMA loop ~5%
    (3989 commands a frame: 838 STCYCL, 820 UNPACK V4-32, 527 MSCNT, 517 FLUSHE, 404 DIRECT, 72 MPG of 38790 words,
    noted not copied), the GIF/GS path ~4% (`gif_path_qword`, `gif_path_bulk`, `packet_bulk`, `gs_write_reg`: the
    HUD, VU1 47A060/476130/478260, the blur and haze sprites, the final blit), `draw_setup` 3.3% (~400
    `apply_state` a frame, 274 of them the cars'; the texture memo misses 143 of 346: streamed textures move
    `gs_ver`), `pc_cached` 1.7%. No one piece is large; each source (the HUD, each VU1 GS program) would need its own
    native path.
- **The texture lookups, `apply_state`, the GS-path programs (the user: "keep going with the next exact items"),
  measured (line tables: `make -f Makefile.wii OPT='-O2 -g'` after touching a file gives the same code with lines;
  `fnlines.py` in the session scratchpad grouped samples by inlined function and line):**
  - `draw_setup` (3.3%) is split: `draw_setup_slow` (rt_get, apply_state, the lookups) is a function of its own, so
    the ~800 draws a frame that change nothing no longer save and restore 18 registers. Neutral in Dolphin (0.46 +
    2.88% against 3.28%: the epilogue's samples were Dolphin's crediting); kept for the console. The slow part is
    ~470 calls a frame at ~1140 cycles; `apply_state` 1.96% spread over the whole function, no 25-line stretch over
    0.24% (it is GX state calls in a row; replaying them recorded would bypass libogc's shadow registers).
  - `GX_InitTexObjLOD` (0.18%, the lookup misses) done as its fields (masks found from libogc, 1000 uses checked:
    0 different): `tex_lod` cost 0.16% itself. Backed out.
  - `gs_backend_sync` 0.68%: 586 checks a frame, 47 past the page test, and those walk every target (span per
    target, block lists): most of its time. Not changed.
  - The GS path by source (`gsstat 10` at 200 s): the sphere map's three grids (3 x 3552 primitives in 10 fields)
    are display lists already in normal runs ("3 static strips from display lists"; gsstat turns that off); the
    sky palette's 2048 points (`point_to_mem`), the HUD 1424 (target 08C0, tex 3DC0), VU1 47A060 478, 476130 544,
    478260 96, the blur/bloom/haze sprites ~2000. `gs_write_reg` 1.2% is the count of writes (vertex kicks, TEX0 with
    its CLUT load), lean already.
- **The user allowed differences that cannot be seen ("trade bit-exactness where it can't be seen: start with car
  lighting on the GPU, check it against the CPU version, and tell me how big the differences are").**
  - *Car lighting on GX's XF lights, weighed, not built.* Per vertex (VU1 46BB80): L = (n,1)^T M (n,1) (a quadratic
    form: the 9-coefficient irradiance), colour = c0 + d L per channel, alpha = amax - d3 (2|n.f| - (n.f)^2), and the
    sphere map's UVs from the eye vector reflected about n. GX has no texgen that reflects, so the UVs (about 40 of
    the loop's ~80 instructions a vertex) stay on the CPU; L needs 7 of the 8 lights (an eigenvector's square as two
    specular lights, h and -h, attenuation x^2; the linear term a signed diffuse light; the constant a light of its
    own) and the two channels subtracted in the TEV, and the fresnel two more lights: over the limit, so it stays
    too. What GX could take is L and the colour (~20 instructions, at most ~1.4% of the frame), through 8-bit
    channel values (1-2 steps off typically) and with the ~1 in 80 vertices that wrap on the PS2 clamped instead.
  - *Built instead (default): the sphere map's scale 128 / |e| from frsqrte's estimate alone* (`nc_shade_ps_loop5`,
    runtime/nw_car.c; the core macro is in parts now, `NCPS_CORE` reassembles to the old code: loop3's machine code
    compared identical). Autopilot `carest 0` = loop3, `carest 1` = both roots from the estimate (`loop4`: 2 UV bytes
    in a race were 90 apart: vertices at the sphere map's rim, where |e| is near 0), `carest 2` (default) = |v| with
    its step. Compared with loop3 (the first 2000 batches and one in 64): colours identical, 0.39% of UV bytes one
    step off (by 1: 3682, by more: 0 in 927408). `nc_shade_ps_loop3` 5.74% -> loop5 4.19% + loop3 1.38% (the batches
    without a sphere map, and the checks): -0.17%. Status line: "gx: car UVs from the square roots' estimate".
  - *Done next, exact: the car loop two vertices a register* (`nc_shade_ps_pairs`, runtime/nw_car.c; autopilot
    `carpair 0` = loop5/loop3 vertex by vertex). Lane 0 vertex 2i, lane 1 vertex 2i+1, the normals' and positions'
    components transposed by merges after the loads; per lane loop5's arithmetic (loop3's without a sphere map):
    `ps_madds0/1` with K's packed constants as the scalar (X = nz m20 + (ny m10 + (nx m00 + m30)), as loop5 fuses),
    sums in loop5's order, and both reciprocal square roots lane by lane in scalar `frsqrte`/`fmuls` (the estimate is
    a double with more bits than a pair's second slot keeps, and the multiplies take it so). Constants added to a
    pair come by `lfs`, which fills both slots (`nc_pairs_ok` checks once). A pair with a light term out of (Llo,
    Lhi) gets its RGB bytes as loop5's label 4 makes them (both lanes: in range the byte is the same). An odd batch's
    last pair is padded: the second vertex's inputs are readable (guest RAM; the normals' copy is rounded to 32
    bytes) and its outputs go to the arrays' rounding (litcol_alloc). Checked against loop5/loop3 (the first 2000
    batches, one in 64 after): 23219 batches, 0 colour words and 0 UV pairs different. Profile: the car shading
    6.17% -> 5.33%; benchmark (`work/fbpair.sh`, `work/auto_grid_nopair.txt`) `carpair 0` 24.73 / 24.72 ms, on 24.47
    / 24.49: -1.0%. A first version that sent out-of-range pairs and odd tails to loop5 calls gained only 0.24%.
- **Weighed after the car loop, not built:**
  - `ps2_sqrtf` (runtime/cpu.c, 1.0%, called from dozens of game functions and the VU0 routines): ~38
    instructions, the two double-precision Newton steps needed for the +-1 ulp check; a cheaper exact test (r = d -
    s^2 against s^2 2^-23 decides ~72% of calls without the integer step to the next float) would take ~0.1%.
  - `gs_backend_car`'s own 2.0%: spread over the function; the clock reads seen in it (`PPCMftb`) are out-of-line
    `wstat 8` code credited by Dolphin with the loop before them.
  - **The native world-batch command** (`hle_world_batch_ee`, runtime/hle_render.c, kept for reference;
    `VIF_NATIVE_CODE` still parsed in `vif_command`): estimated at 1-2% and asked for (the user: "build the native
    world-batch command behind a switch"), then not built: **it was built and measured on 2026-10-14** (section 5,
    "Done later on 2026-10-14", "Built, measured, taken out"): the five qwords cost the VIF ~100 cycles a batch, the
    group-list command saved 9000 cycles a frame, 0x26AF50 natively 0.07 ms. A batch's cost is the render data's
    parse check (`pc_cached`) and the draw (`gs_backend_world`, `draw_setup`), which a native command still needs.
    Read section 5 before estimating an avenue.
- **The console package is rebuilt from 54ba74d** (`build-wii/hbc/apps/burnout3`, the ELF kept as
  `work/console2.elf`).
- **Tried and backed out: decoded sound blocks remembered** (iop_sd.c `decode_at`; the patch is
  `work/sound_memo.patch`). A block's 28 samples are a function of its 16 bytes and the decoder's two history
  samples; keyed by block address, a write generation per 4 KB page of sound RAM (sceSdVoiceTrans bumps it; the
  mixer's capture rings 0x2000-0x4FFF left out) and the history. Exact (20000+ checked, 0 differed), 52-64% of blocks
  hit in the race, but: 16384 entries = 1.1 MB, which left MEM2 675 KB at the race start (1763 without), for -0.33% in
  `decode_at` and +0.21% in `voice_stretch` (net ~0.15%); and on a console it trades ~300 cycles of decoding for two
  or three cache misses into a 1 MB table. Not worth the memory.
- **Looked at, nothing to take:** VU0 program lookups after program swaps (the full compare is what proves the
  program unchanged); the VU0 registers' 128-bit moves (the recompiler already loads them with four `lfs`);
  `draw_setup` (3.3%, no one hot spot); `gs_write_reg` (lean); `target_time`/`pt_enter` (return at once with the
  split off); `gs_load_clut` is 0.48% now (the repeats memo took it from 1.0); `memcpy` 2.5% is a third the game's
  scratchpad DMA (`dma_spr`), then `unpack_bulk`, `hle_coll_sphere_call`, `gs_backend_car` (already 32 bytes a
  turn); `vif_feed_words` 3.8% is the dispatch and the NOP-padding skip (the cost of reading the stream at all).
- **Profile now** (`work/prof_kc.bin`/`.elf`, race start, 50172 samples): `nc_shade_ps_loop3` 5.5, `ps2_vu0_callms`
  4.8 (mostly its callers' code), `vif_feed_words` 3.8, `draw_setup` 3.3, `memcpy` 2.5, `nw_lit_rgba` 2.2,
  `gs_backend_car` 2.0, `pc_cached` 1.8, `vu0_vis` 1.7, `seg_tri` 1.7, `f_0021C780` 1.7, `dma_run` 1.65,
  `gs_backend_world` 1.25. `python -I work/pcmp.py ELF_A BIN_A ELF_B BIN_B [FUNC..]` compares two profiles.

### Was in flight before that

- **Background run `bq7ve7l64`** tests a fast CT32 path in `clut_from_upload`
  (runtime/gs.c). It reads palette entries straight from the pending upload
  through one row pointer per 8×8 block. It replaces a generic per-entry path
  that had made `gs_load_clut` cost 3.55%. The command is the one in §4.
- **To check it:**
  - read the log for `frames/s` and errors;
  - run `python -I work/wprof.py -n 30` and confirm `gs_load_clut` dropped;
  - look at `work/frames/wii_race.png` for palette or texture corruption.

## 6. Next tasks, in order

G. **The 60 fps gap, and the ideas big enough for it (2026-10-16, the user: "if we collect all of our findings it
   doesn't get to 60fps? do you have any smart out-of-the-box thinking or trick?", then "write these into the doc").**
   - **The sum of what is known does not reach 60.** The race start is ~24.5 ms of work a frame (`work/fb.sh`, Dolphin's
     count); 60 frames/s needs 16.7 ms: a gap of ~8 ms (a third). Everything still listed, exact or not, adds up to
     about 1.5 to 2.5 ms: the native world walk 0.3-0.5 ms (item R.0 avenue 1), palettes placed like rasters 0.3-0.5 ms
     (avenue 2, the "pink surfaces" risk), keeping several textures bound for the cars' two passes ~0.07 ms (R.2), the
     47 target checks a frame past the page test ~0.07 ms, `ps2_sqrtf` ~0.02 ms, the placed textures' audit every 60
     frames instead of 30 ~0.1 ms (not exact: a changed texture caught later), car lighting on GX's lights at most
     ~0.3 ms (not exact; section 5, "Done on 2026-10-16"). Each needs care for its size; together ~23 ms.
   - **Two things soften it.** The race start is the heaviest moment (lighter stretches of the drive reached 59.8-59.9
     frames/s at interval 1 earlier); and every number is Dolphin's, which counts about a cycle an instruction and no
     cache misses: the console can rank all of this differently (section 4, "On a console": the test package).
   - **The ideas of the size the gap needs, largest first:**

     | # | idea | could take | exact? | effort |
     |---|---|---|---|---|
     | 1 | **Stop rebuilding the PS2 frame:** the static world as GX display lists per section, replayed, instead of walking the game's chain batch by batch through the parse check (`pc_cached`), `draw_setup` and `apply_state` every frame | ~3-5 ms | yes, with checks that the inputs (batches, textures, constants) are unchanged | very large; the only single item near the gap |
     | 2 | **The idle chips:** the audio DSP mixes the sound (the mixer is ~4% of the CPU now) | ~1 ms | no: fixed point against today's floats; likely not audible | large |
     | 3 | **The GPU directly:** GPU register writes straight to the command FIFO (write-gather pipe) in the hot draw paths instead of libogc's functions and their shadow-state bookkeeping (`draw_setup`/`apply_state` ~3%, libogc GX ~2%, the backends) | ~0.5-1 ms | yes: the same register writes | medium |
     | 4 | **The compiler across game functions:** hot caller-callee chains of recompiled code in one file (gen/ is 54 files, ordered by address), so GCC can inline across guest functions and keep values out of the context; the hot ~1% of functions at -O2 instead of -Os (`GENOPT`) | ~1-2 ms (uncertain) | yes | medium |
     | 5 | **The locked cache (console only):** Broadway can lock 16 KB of its L1 data cache as scratch memory, the size of the PS2's scratchpad, which the game uses heavily (`dma_spr` is a third of `memcpy`'s time; the scratchpad is guest RAM 0..0x3FFF now, in MEM2); the scratchpad and VU memory there, and `dcbt` prefetch in the hot loops (R.5) | unknown in Dolphin; possibly a lot on the console | yes | medium (the guest addresses of the scratchpad go through one base today) |

     All of them together could reach 60 at the race start; no smaller set does.
   - **The order proposed:** the console run first (if the Wii is bound by cache misses rather than instructions, 5
     moves to the top; if by the GPU, the plan changes again), then 1 as the one big bet with 3 and 4 beside it; 3 is
     the safest place to start.

R. **What is left after 2026-10-15, by weight** (the race start, 25.9 ms of work a frame = about 18.9M cycles; 60
   frames/s needs 16.7 ms; the user wants exact changes only and judges by a real Wii, which Dolphin's timing does not
   model: no cache misses, about a cycle an instruction).

   **R.0 Where the frame is after the collision work, and the avenues by size (2026-10-15, night).**
   - **Profile** (`sh work/pf.sh 181 5`, the race start, today's build, 50149 samples; `work/pf_now.out`): by file game
     code 21.8%, vu 14.3%, gs_gx 13.9%, gs 10.5%, vif 9.4%, nw_car 7.7%, memfast 3.8%, iop_sd 3.1 + hle_audio 1.1,
     nw_world 2.6, hle_render 2.4. Functions: `nc_shade_ps_loop3` 5.5, `ps2_vu0_callms` 4.8, `vif_feed_words` 4.2,
     `draw_setup` 3.2, `memcpy` 2.8, `nw_lit_rgba` 2.1, `pc_cached` 1.9, `dma_run` 1.8, `gs_backend_car` 1.8,
     `gs_backend_world` 1.7, `seg_tri` 1.7, `vu0_vis` 1.6, `f_0021C780` 1.6, `rec_run` 1.4, `f_0021FBB0` 1.2,
     `vu0_box_test` 1.1, `hle_coll_sphere_call` 1.1. Inclusive (stack samples): `f_001B8F00` (the physics and
     collision step) 5.2%; `f_0026A610` (the props' visibility) 3.3%, of it `vu0_vis` 1.7 and the dispatch 1.0;
     `p3_run` (GIF PATH3 under `vif_feed_words`: palettes, GS packets, register writes) 5.0%, `gs_load_clut` 1.5%.
   - **Read with care:** `ps2_vu0_callms`'s 4.8% is mostly its callers' code (Dolphin's JIT blocks run on past a
     direct call; the fast path is ~30 instructions), which is why the native 0x26AF50 saved only 0.07 ms. And
     `f_00221E80` (the texture send) is 8.8% inclusive only because the DMA kick inside it runs the renderer
     (`kernel_poll`).
   - **Counters at the race start** (`wstat 8`, `wstat 1`, `wstat 7`; cycles a frame): car batches 2.6M (shading 1.2M;
     passes' `draw_setup` 392k, passes' draws 234k, GS packets 217k for 205, matrices 170k for 106, combo steps 310k);
     the world path 0.97M in the VIF's call (parse 272k, draws 563k: `draw_setup` 305k) plus 0.53M in the game's batch
     function and ~0.3M in the walker; `apply_state` 440 calls (texture lookups 89k, texture object and load 163k, TEV
     118k; `tex_lookup` 143k, `gs_backend_sync` 114k); recompiled VU0 calls 458 x 727 = 333k.
   - **The avenues, by what they could take** (25.5 ms of work; 60 needs 16.7: about 35% to go):

     | # | avenue | its cost today | could take | size, risk |
     |---|---|---|---|---|
     | 1 | the world walk as native code (0x21C780 and its callees; measured later that night: the chain per batch is small, the game side is ~12% of the frame, see "The world path, measured") | ~3 ms game side + ~1.5 ms backend | 0.3-0.5 ms (read later: the recompiled code is already lean; only skipped work pays) | large: every function's chain bytes and side effects checked |
     | 2 | the texture path's rest (palettes as TLUTs from their source, no CLUT loads through the GS; the other ~105 rasters; `apply_state`'s lookups) | PATH3 5% incl., lookups ~0.4M | ~0.8-1.2 ms | medium |
     | 3 | car batch set-up (R.2) | 1.4M besides shading (timer-inflated: see "stage counters overstate") | under 0.4 ms | medium, exact by construction |
     | 4 | recompiler: values at function boundaries (the "third step", section 5) | 326k context stores (static) | 1-3%? | medium-large; general (crashes, split screen) |
     | 5 | the render side's VU0 callers (`f_0026A610`, `f_0026AF50`, `f_0021FBB0` + a native 47F6D0@07D8) | ~0.8M incl. | 0.2-0.4 ms | small-medium |
     | 6 | sound: decoded engine loops (tried 2026-10-16: exact, ~0.15%, 1.1 MB; backed out) / the DSP | 4.2% | 0.04 ms / ~3% | small / large, audible |
     | 7 | a paired-single back end for the VU recompiler | ~3% of recompiled VU code left | ~1% | medium |
     | 8 | console-only (R.5: dcbt/dcbz, locked cache, MEM1, -Os, interleaved shading) | not seen in Dolphin | ? | needs a Wii |

     1 to 3 together are about 3-4 ms (to ~21-22 ms in Dolphin's count); the small exact pieces add up to 3-5%. 16.7
     ms needs 4 as well, and a console to say where the real time is.
   - **Direction (user, 2026-10-15 night): "utilize wii's native capabilities"; "would we benefit if we stopped
     thinking in terms of vu0 and vu1 entirely?"** Yes, with one line drawn:
     - *Rendering:* VU1, the GS and the VIF are a means; the picture is what must be exact, not the PS2 state. The
       native renderer still reads the game's output in the PS2's shape every frame (chain, VIF packets, GS registers,
       GS state turned into GX state): vif 9.4% + gs 10.5% + much of gs_gx's 13.9%, about a quarter of the frame.
       Taking the game's draw decisions at its own functions (texture bind 0x221A70, world batch 0x26AF50, the car
       batches) and handing GX native objects (display lists kept per batch, GX texture objects and TLUTs, hardware
       transform, GX's own clipping) is where that goes. A render-only VU0 test (the box tests, the props' visibility)
       need only be conservative: never cull what the VU program would draw (a batch drawn that the VU culled is clipped
       by GX: the same pixels). Check first that nothing but the chain reads a test's result.
     - *Gameplay (collision, physics):* results feed the simulation, so the arithmetic stays bit-exact (VU float rules:
       round toward zero, no fused multiply-add, the program's order), but not VU0's register file: natives of whole
       functions with values in C locals (R.1's "replace the loop that calls them"). Less to gain (~1-2%).
     - *Already Wii-native:* textures are CI4/CI8 with RGB5A3 TLUTs or RGBA8 (gs_gx.c `tex_make`), the world and the cars
       use GX's hardware transform and display lists, the sky strips GX transform and clipping, the car light and the
       mixer paired singles with GQR quantized loads. Not used: TLUT slots past TLUT0, texture slots past TEXMAP1 for
       state reuse, the locked cache, `dcbt`/`dcbz`, the DSP.
   - **Item 2's plan below (override 0x221E80) was built on 2026-10-12** (section 5, "Done at night on 2026-10-12":
     `hle_tex_send`, placed rasters, mip levels left out); the text of item 2 is older than that. What is left of the
     texture path is avenue 2 above, measured (the profile above; status lines at the race start):
     - under `p3_run` (5.2% inclusive): `gs_place` 1.3% (of it `ext_source_ident` 0.9: the 16 samples of each of ~117
       placements a frame, and the full re-reads in turn, ~4 a frame, 89 KB, none ever found a change),
       `gif_path_image_host` 1.0% (`lazy_finish` 0.8: the transfers still sent, ~115 script entries a frame, nearly all
       palettes, 36 of them staged), `gs_load_clut` 1.0% (339 loads a frame: 193 repeats, 135 cache hits keyed by
       `gs_block_ident`, 11 read; each calls `gs_backend_sync` before the repeat test), `gs_write_reg` 1.1%,
       `gs_sync_rect` + `mark_rect` ~0.5%;
     - `apply_state`'s texture part (~2%): the memo hits 203 of 346 a frame; `tex_lookup` 143k cycles, texture object
       and load 163k, `gs_backend_sync` 114k, `tlut_lookup` 27k.
     - The bind 0x221A70 builds TEX1, TEX0, MIPTBP1/2 and TEXFLUSH writes into a buffer of the state (+16, count at
       +0) that the chain carries later; a texture's TBP is reused within a frame (the game's cache halves are smaller
       than a frame's textures), so anything keyed by TBP has to travel in the chain's order, as the notes do.
     - **The world path, measured on 2026-10-15 at night** (asked for: "the walker hands GX each batch's display list
       and texture directly"; nothing built, the facts first). A one-off log (now removed) of what 0x26AF50 writes:
       five qwords a batch, always: the DMA CALL of the render data (VIF NOP NOP), state+0 (CNT 0, VIF FLUSHE), a
       CNT 1 with STCYCL 4/4 and UNPACK V4-32 to 1013 plus its data (clip value 0 or 0xA, mode 1 or 3), state+32 (CNT
       0, VIF MSCNT). Render data packets are nearly all one batch, without MSCNT inside: each is deferred and drawn
       at the MSCNT. (That run had `dbg 91`, which also cut the reflection views: `g_dbg` bits 1 and 2, hle.c.)
       Where a batch's time goes (profile above, not the `wstat` stage timers: in Dolphin a clock read costs ~170
       cycles, so those inflate every stage by about that much, and `wstat 7`'s ~2000 cycles an `apply_state` are
       mostly the nine reads):
       - the backend: `pc_cached` ~720 cycles a packet (a tight loop, 8 instructions a word, ~40 words a packet: the
         VIFcodes the parse read; no cheaper check without trusting the data to be static), `vif_defer_call_in` ~280,
         `gs_backend_world` ~460 + `draw_setup`'s fast path and the projection/camera compares (each already a
         compare-and-skip; a "steady state" path skipping them would save ~0.15 ms: not built). Its one hot loop is the
         lit instanced props' colours (`inst_lit_draw`, 20% of the function, 0.34% of the frame), recomputed every
         frame from inputs that rarely change.
       - **the game side is the larger part, ~12% of the frame (~3 ms)**, inclusive under `f_0021C780` less the chain
         processing that runs inside its I/O reads (`ee_read32` -> `kernel_poll`): section end 0x26B2B0 2.7% (group setup
         0x26B2F0 2.5 with the texture bind and send), batches 0x26AF50 2.2% (the box test 1.6 with its dispatch), the
         walker's own code 1.9%, props' visibility 0x26A610 1.5%, chunk flags 0x21D080 1.0%, section state 0x26BAA0 0.7%,
         static lists 0x332D00 0.7%, 0x21D550 0.5%.
       - The chain's own cost a batch is small (the 2026-10-14 list command: ~80 cycles a batch), so "no chain per
         batch" by itself gains little; the gain is in running the walker and its callees as native code. Natives of
         0x26AF50 alone gave 0.07 ms (2026-10-14): the work is spread over the walker, its seven callees and the box
         tests, and only a native walk of the whole (each function with a check of the chain bytes and state it
         writes) takes a real share: perhaps 1-1.5 ms of the ~3, a large job (0x21C780 is 2080 lines recompiled;
         "To keep exactly" in section 5 lists its side effects).
     - **Then (the user: "build the native world walk in stages, each function checked against the original's chain
       bytes and state")**, read before building, and why it stopped there:
       - The recompiled walker is ordinary optimized C: guest registers in locals, `PS2_PROF` empty in release builds,
         `PS2_POLL` one flag test. What the host code is made of (`python -I work/addrcost.py FUNC..`:
         objdump of a function, counting its loads/stores and the guest-address adds `addis rX,rY,-28656` of the fixed
         RAM base): f_0021C780 1383 instructions for 525 guest ones, 629 loads/stores, 65 guest-address adds (most of
         the rest is the context traffic the recompiler writes around its ~35 call sites, which callees read and a
         native must write the same); f_0026AF50 958 (153 guest), 132 adds; f_0026B2F0 1194, 122; f_001B8F00 2182, 169;
         f_00273070 1051, 75. So a code-for-code native walker compiles to about the same; what a native one can drop is
         work: the 17 chunks x 4 views tried for every group though few have a mode (perhaps 0.15-0.25 ms), and the
         box tests' trip through the VU0 registers (the 2026-10-14 native 0x26AF50: 0.07 ms). All of the walk natively:
         perhaps 0.3-0.5 ms, for a hand port of ~2000 lines that must leave the context as the recompiled code does at
         every call (scratch registers, HI/LO, lane 1 included, or prove them dead).
       - The check designed for it (not built): `ENTRY_HOOKS` on the walker's eleven recompiled callees, and the two
         native ones (`hle_world_batch`, `hle_inst_view`) at their entry, record a call when `c->r[29][0]` is the
         walker's sp (the walker writes its sp to the context once, before its first call; nested calls have a lower
         one): the target, a0-a3, t0, t1, at, v1, HI/LO, ra, f12-f14 and S+152..191, folded into a per-frame digest,
         plus a digest of the context at the walker's return. The same hooks fire under either walker, and the
         benchmark window (ticks 600-900) is deterministic, so a `wwalk 0` and a `wwalk 1` run of one binary must give
         equal digests frame for frame (with `wstat 3`'s world hashes as well).
       - **The `wstat 1`/`7`/`8` stage counters overstate small stages**: each stage boundary is a time base read,
         ~170 cycles in Dolphin. `apply_state`'s "~2000 cycles" (nine reads) and R.2's car batch steps (eight or more
         reads a batch) are inflated by that much; rank by the profile, or subtract the reads' own cost.
     - The piece left: **palettes placed like rasters** (a note for the palette's transfer, the CLUT load answered
       from the source data, keyed by its content; GS memory written only when something reads it): about 0.3-0.5 ms.
       The risk class is the one that gave "pink and rainbow surfaces" on 2026-10-12: check every load against the
       old path at first (the `clutchk` pattern).

   **R.1 The collision code's VU0 work: about 5% of the frame (1M cycles), the largest single piece left.**
   - **Done (2026-10-15, later; section 5, "Done later on 2026-10-15"):** 0x273070's sphere tests are decided as misses
     by the routine's own flag logic (432 of 466 a frame at the race start), and the segment tests 0x0388/0x0390/
     0x0550/0x03D8/0x03E0 (0x273400, 0x273BF0, 0x274C90) are natives that write only what may be read: -1.1% of the
     mean frame, -1.6% at the median (`vu0coll 0` for A/B). Hit rates before (`vu0time 1`, the race start): 0x0000
     14/244, 0x0028 15/258, 0x0388 1/290, 0x0390 3/188, 0x0550 4/225, 0x03D8 49/201 hits/calls a frame.
   - **Left in R.1:** the render side's calls (`f_0021FBB0`: 47F6D0@07D8 121 x 1472 cycles, @0B50 252 x 281;
     `f_0026A610` -> `vu0_vis`, 559 x 617; `f_0026AF50` -> the box test, 449 x 552: natives already, the dispatch and
     the trip through `c->vf` are what is left); 4787D0@0060 (`f_00274ED0`, rare at the start); the lookups after VU0's
     program swaps (below). The decided sphere calls could also skip the 112-byte input copy and the remaining 7% (plane
     test misses) would need the routine's square root and division done exactly (ps2_sqrtf, vu_div).
   - What it is: VU0 program 0x4787D0, entries 0x0388/0x0390/0x0550 (a segment against a triangle, 380 to 450 calls a
     frame, about 800 cycles each), 0x0000/0x0028/0x0060 (a sphere against a triangle, 1100 cycles), 0x03D8, and
     0x47F6D0@0x07D8 (from `f_0021FBB0`, 70 to 110 calls, 1470 cycles). They run as recompiled lean routines
     (`vu0_4787D0_*_l`, gen/vu0_*.c: they store only what the game reads afterwards); in the profile they are
     `vu0_4787D0_l` 2.2%, `rec_run` 2.6% (mostly the routines' last blocks: Dolphin delivers the sample after the
     return), `vu0_47F6D0` 0.5%, and `run2`/`rec_find` 0.5% (below). In a heavy crash the collision and physics step
     0x1B8F00 is 5.9% of the frame (section 5, "Done on 2026-10-13").
   - Why the earlier native went nowhere: `ray_tri` (0x0388..) replaced the VU0 entry alone and had to leave every
     register as the program does (57 stores around 96 operations), and the recompiled code does no more than that
     (section 5, "The collision code's VU0 calls"). The lean routines took the dead stores out since.
   - **The solution: replace the loop that calls them, not the VU0 entries.** The EE function that walks a car's
     candidate triangles loads vf registers (`lqc2`/`qmtc2` into `c->vf`), calls the entry (`vcallms`: the dispatcher
     `ps2_vu0_callms`, `rec_run`'s context set-up, the routine), reads the result back (`qmfc2`/`sqc2`), and does it
     hundreds of times a frame. A native of that loop (an `OVERRIDES` entry in config/burnout3.py, as 0x2FBE20 is)
     keeps the vertices, the segment and the partial sums in C locals (one variable a lane: arrays of lanes compile to
     memory traffic on Gekko, section 5), does the test inline in the order the microprogram does its operations
     (`#pragma GCC optimize("fp-contract=off")`, real divisions where the program divides, Q's latency as the
     recompiled code has it), and writes to guest memory and EE registers only what the game's code would. No VU0
     registers are left at all, because no VU0 code runs; what the game reads of VU0 later must be checked first
     (`work/vu0live.py sites`, `readers`, for the call sites in that function and what follows them).
   - **The call sites, measured on 2026-10-15** (`vu0stat 2`, the race start, `work/last_st_vu02.txt`; cycles a frame):

     | guest function | VU0 entry | calls | cycles each | a frame | called from |
     |---|---|---|---|---|---|
     | `f_00273070` | 4787D0@0028, @0000 (sphere/triangle) | 251 + 238 | 778, 707 | 365k | `f_003BE9B0` <- `f_00301B00` <- `f_001ABE70`, `f_001C5080`, `f_002BDDF0` |
     | `f_00273BF0` | 4787D0@0550, @0390 (segment/triangle) | 228 + 192 | 498, 500 | 210k | `f_003BF720` <- `f_00301340` (<- `f_003078F0`, `f_003012D0`), `f_00301C80` |
     | `f_00273400` | 4787D0@0388 | 296 | 451 | 134k | `f_003BEF60` <- `f_00301BC0` |
     | `f_00274C90` | 4787D0@03D8 | 199 | 416 | 83k | `f_0015F410`, `f_001CDB70`, `f_001CE1A0` |
     | `f_00274AF0` | 4787D0@0A50 (native box test) | 39 | 1526 | 61k | `f_0021D080` |
     | `f_00274ED0` | 4787D0@0060 | (rare at the start) | | | `f_003BE9B0` |

     So about 850k cycles a frame in the calls alone, plus the five functions' own code (`f_00273400` 0.21%,
     `f_00273BF0` and `f_00273070` in the hot list). Start with `f_00273070` and `f_00273BF0` (575k between them).
     The render side has the same shape: `f_0021FBB0` calls 47F6D0@07D8 (121 calls, 1472 cycles: 179k) and @0B50
     (252, 281: 71k); `f_0026A610` calls the native `vu0_vis` (479610@0138, 559 x 617 = 345k) and `f_0026AF50` the
     native box test (4787D0@0A18, 449 x 552 = 248k): those two pay the dispatch and the trip through `c->vf` for
     natives, and a native of the caller would take that out too.
   - Steps: (1) read the callers' loops (`work/pretty.py ADDR N` for the guest code; `python -I work/callgraph.py tree
     f_00273070`) and what the game reads after each call (`work/vu0live.py sites`, `readers`); (2) write the native next to the
     recompiled function with a check mode (the first N calls run both from the same state: compare the EE registers
     the caller leaves, the guest memory it wrote, and VU0's live registers; log "N calls checked (M differ)" as the
     other natives do); (3) measure with `wstat`-style cycle counters around the call (sampling hides it: the time sits
     in other functions' blocks), and in a crash, where it matters most.
   - Fallback or addition, broader: **a paired-single back end for `tools/vu/vurecomp.py`**: lanes x,y and z,w in one
     register each, `ps_add`/`ps_sub`/`ps_mul` for two lanes at once; never `ps_madd` (fused: the VU rounds after the
     multiply, which is why the build has -ffp-contract=off), lane masks through `ps_merge*`, the VU's clamping (no
     Inf/NaN) and the flags where a branch reads them kept as the scalar code does. Up to twice as fast on the FP-heavy
     blocks: perhaps 2 to 2.5% of the frame, and the VU1 programs that still run (the sky's generator, particles).
     Checks: `vu0chk 1` (every call both ways) and the per-entry compares.
   - Small, separate: the program lookups after VU0's program swaps (9 to 18 a frame; every entry is searched again,
     `memcmp` of whole ranges in `rec_find`: about 0.5%). The swaps do rewrite the code (the block generations of
     2026-10-15 keep 0 or 1 a frame), so take the program from the upload's source as VU1 does (`g_rec_chain`: the DMA
     chain that uploads it, then 12 sampled words).

   **R.2 Car batches: about 1.6M cycles of set-up a frame besides the 1.65M of shading** (`wstat 8`).
   - Two-draw batches (74 a frame: blended paint, glass) alternate their passes' GS states: two full `apply_state`s a
     batch (about 1200 cycles each). Keep up to four textures in GX texture slots (TEXMAP0..3, the combined draw uses
     TEXMAP1) and point the TEV order at the slot that holds one, so going back to pass 1's state does not load its
     texture again; this is also what the real GPU's texture cache wants. A memo of the derived GX state per GS state
     (the A-B-A pattern) would skip the decode and most compares.
   - The per-object GS packet (252 a frame, about 1000 cycles each: CLUT loads that are not repeats, the GIF tag's
     parse). The shading loop (`nc_shade_ps_loop3`, 5.3%, about 80 instructions a vertex now): on the console it is
     latency-bound; two vertices interleaved (or colour and UV loops apart, so the constants stay in registers) would
     help there more than in Dolphin.

   **R.3 The VIF and the world batches.** The world path is 1.09M cycles in the VIF's call (the parse cache 301k,
   the draws 641k: `draw_setup` 306k of them) plus 536k in the game's batch function 0x26AF50 (the box test through
   VU0). `vif_feed_words` was 4.4% (NOPs, half of its commands, are skipped now). The remaining overhauls of item 0a
   (world sections as one list, textures by identity for palettes) still hold, with the numbers above.

   **R.4 Sound, 3 to 6% of the CPU** (`decode_at`, `voice_stretch`, `engine_channel`; 31 voices playing in a race, 19
   heard): the DSP (item 0, "Cost"), or decoded copies of the engine's looping samples (memory: MEM2 has about 1.7 MB).

   **R.5 The Wii hardware tricks** (section 5, "Wii hardware tricks"): unchanged, all still to try on a console, each
   behind a switch.

   **R.6 Commit.** Done: the work of 2026-10-15 (both rounds) is commit e15f1a9 on wii-port, pushed; staged by path,
   docs/native-renderer.md left out. Commit when the user asks, staged by path.

0. **Sound: built on 2026-10-13; the user has heard the first two builds**
   (the first had no engine, which they reported; the second has it). There
   had never been any before (`iop_sd.c` only kept the chip's time).
   - **The mixer** (`runtime/iop_sd.c`, `spu_mix` from `spu_tick`, no
     oftener than every 3 ms): ADPCM blocks decoded as a voice enters them
     (`decode_at`), straight-line interpolation at the pitch, the chip's
     envelope rule (`rate_step`) and its volume slides (the driver steers
     every voice's volume with them: `vol_set`/`vol_run`), per core the
     voices the mix switches name, core 0 times its volume into core 1 by
     that core's input volume, core 1 times its volume out. Not modelled:
     reverb (the race has core 1's on, mode 5 at an eighth), noise, pitch
     modulation, the chip's four-point interpolation.
   - **The sums are floats and the loops paired singles** (`voice_stretch`,
     `mix_out`, `mix_f2s16`: inline assembly in iop_sd.c, GQR 4 set to
     signed 16 bits by `MIX_GQR`, C versions for the PC): a stretch of a
     block is one loop of 14 instructions a sample whatever the voice's
     sides and slides. The menu music's spectrum is the same to two
     decimals as the integer mixer's recording.
   - **The engine** (and whatever else the game filters itself): core 0's
     voices 0-7 are in the effect switches only. On the PS2 the driver
     reads core 0's effect sum back out of the capture rings half a ring
     (256 samples) at a time, the EE's handler 0x2FC530 filters each
     channel (0x2FC070: mode 1 at state + 112 is `audio_filter`, a
     state-variable filter with a wave folder; mode 2 is 0x2FB890, a
     two-pole filter), 0x2FC820 makes one sound of the two and pans it, and
     the driver writes the result into core 1's sound data input. That
     loop got round 60 to 90 times a second here where the chip needs
     187.5 (measured with the rings modelled: `engnative 0` still runs it
     that way). So the mixer hands core 0's effect sum to `hle_engine_dsp`
     (runtime/hle_audio.c) and adds what comes back times BVOL: the same
     arithmetic from the game's own state (found when 0x2FC070, now an
     override `hle_audio_channel`, is first called: 0x01E7AA40 in the
     race), the filters' memories kept where the game keeps them. The
     game's loop still runs with nothing to work out. Only mode 1 has been
     met; modes 0 and 3 write nothing in the game and give silence here.
   - **Out** (`plat_audio_write`/`plat_audio_level` in plat.h): on the Wii
     a queue of 8192 pairs and the audio interface's DMA in blocks of 512
     from two buffers (`aq_next`); a pair goes out right word first. The
     tick makes a little more or less to keep 2400 pairs queued. On the PC
     `B3_PCM=file` writes what is mixed (not run yet).
   - **Cost.** RenderWare Audio keeps about 34 voices playing all the
     time and steers each volume with slides around its target; 10 to 13
     are above the threshold (`GAIN_HEARD`, 60 dB down) in a race with the
     engine's four. In the scripted race: 7.3% of the processor with the
     first engine build, 4.6% now (profile: decode_at 1.13, voice_stretch
     1.09, engine_channel 0.91, voice_run 0.46, the rest under 0.25 each).
     What is left to take: the engine's eight samples are loops of 18 to
     30 KB decoded over and over (a decoded copy would save about half the
     decoder, some 700 KB); a higher threshold (three of twelve voices sat
     40 dB down or more in one dump); 32 kHz out would cut a third of the
     rest but the engine's filter is tuned for 48 kHz (f = 1.18: at 32 kHz
     the same cut-off is beyond where that filter is stable). The DSP
     could do all of it; nothing of that is started.
   - **Silence while a track loads** (seen in the logs of the user's
     sessions and of every scripted run: 30 to 40 blocks of 10.7 ms "of
     them silence for want of samples" in the status block around 180 s,
     none in a race): the output holds 50 ms and the game cannot tick the
     chip from inside long native work. `iop_sd_pump` (iop_sd.c) mixes
     when the output is below half its mark; it is called per texture
     decoded and per display list made (gs_gx.c), which was not where the
     time went, and every eighth row of the software rasteriser
     (gs_raster.c), which was: a picture drawn pixel by pixel at the
     track's load, a third of a second (found with the probe in
     `aq_dry_note`, plat_wii.c: where the output's interrupt found the
     processor when it had nothing to play, logged in the spike frames'
     form for work/spike.py). What that picture is and why it is drawn in
     software has not been looked at.
   - The tick was every 1 ms, then 3, now 5 (`spu_tick`); reads of a
     timer's count no longer poll the kernel unless that timer's event is
     due (hw.c, with the status line "timers: ...": the race reads none,
     so `plat_time_us`'s 0.5% is somebody else's).
   - **Reported and not solved: choppy sound in the car select** (section
     0 has what is known and the leads).
   - **Not verified:** the balance against the original (no reference was
     allowed: PCSX2 needs the user's word), clipping (the mix reaches full
     scale in crashes; `spudump 1` counts the samples), anything on a
     console.
   - Debugging: autopilot `mute 1`, `spudump 1` (the voices and the
     routing with every status block, and since the evening of 2026-10-13
     the voices' events: `voice_event` in iop_sd.c, 'k' keyed on, 'o'
     keyed off, 'e' its data ended, 'w' a write over the block a voice is
     playing), `engnative 0`, `atone 1`, `aswap 1`;
     the status lines "sound: ..."; Dolphin records with `DOLOPT="-C
     Dolphin.DSP.Volume=0 -C Dolphin.DSP.DumpAudio=True"` (scripted runs
     are run that way: at unlimited speed the speakers would get noise) to
     `D:/b3wii-dolphin/user/Dump/Audio/*dspdump1.wav` (its header's length
     is wrong after a kill: read the data from byte 44).
0a. **Overhauls thought through on 2026-10-13 for the 3.5 ms the drive
   lacks for 60** (20-21 ms of work at two passes; the profile is flat, so
   these are changes of level, not of loops), by what they should give:
   - *Textures by what they are* (below, item 2: the rasters are placed
     already; the palettes, the staged uploads, about 170 CLUT loads and
     the script entries a frame are not): the GS path is 6.1% of the frame
     and the game's bind and upload code 1.6%; perhaps 1.2 to 1.5 ms.
   - *World sections drawn whole:* the game tests and emits 524 world
     batches a frame one by one (0x21C780 and below 8.7% of the frame with
     its VU0 tests, the chain's side of them 6.7%). GX clips by itself, so
     the clip mode a batch is tested for means nothing here: a section's
     batches of one texture as one display list, tested once. Perhaps 2 to
     2.5 ms; the lit batches, the order of blended ones and LOD are where
     it can go wrong.
   - *Microprograms by which they are:* the game sends VU1 programs 72
     times a frame (39700 words of MPG); a note of which program it is
     where the game queues it would spare the noting, copying and
     comparing (about 1%).
   - *Static packets parsed once:* `nw_packet_parse` and `nc_packet_parse`
     read the same packets' headers every frame (2.1% with their memset).
   - *The cars' light from a texture:* the colour is a function of the
     normal alone (a quadratic form, nw_car.c), so a small texture looked
     up by the normal could replace the per-vertex loop; the reflection's
     UVs depend on the position too and would stay, so no more than 0.4 ms
     and not exact. Not worth it before the others.
   - Not visible in Dolphin at all: what the picture costs the console's
     GPU (five target copies and a bloom a frame) and how the game's data
     behaves in Broadway's caches.
   - **Where the frame stood after that day's round** (scripted race, 30
     frames a second, a third of the time idle; shares of all time):
     sound 4.6; the cars' shading 2.5 (`nc_shade_ps_loop`) and their
     backend 0.5; the VIF 2.0 (`vif_feed_words`: 42000 UNPACK words a
     frame that are not render data, 464 V4-32 uploads of constants, and
     19000 DIRECT words), `dma_run` 1.0, `unpack_bulk` and `packet_bulk`
     0.8 each; `draw_setup` 1.6 (550 to 650 calls a frame, 170 to 190
     states applied: 2000 cycles each); the game's world walk 0x21C780
     0.95 with 0x26BAA0 0.5, 0x26B1C0 0.4, 0x26AF50 0.4, 0x1F3080 0.5; VU0
     calls 1.4 in the dispatcher (`ps2_vu0_callms.part.0`), `vu0_vis` 1.2,
     `vu0_box_test` 0.7; `memcpy` 1.6 (two fifths of it the game's
     scratchpad DMA, `dma_spr`); `ps2_sqrtf` 0.7 (tuned already: the game
     calls it that often); `nw_lit_rgba` 0.9; `plat_time_us` 0.5 (callers
     not found: the stack samples end in it). At 60 the drive lacks 1 to
     1.5 ms with the pacer's tenth to spare.
   - **The world walker's main loop, read on 2026-10-13** (0x21C780, a0 =
     the walker s5; s4 = [0x64C994], the world's data; the group state is
     at 0x01E32A10). After a DMA of 192 qwords from [s5+452]+96 to the
     scratchpad at 0x70002000, a call of 0x26B8E0(state) and eight
     conditional calls of 0x332D00, it is:
     `for g in 0..(int8)[0x70002003]: 0x26AE30(state, [s4+8] + 40 *
     (int16)[0x70002014 + 2g]); pass 0 (if [0x665F1F] and not [0x4EB8F6]):
     for sec in 0..16: mode = [s5+88+2 sec] & 3; b = (int8)[0x70002000 + g
     + 169 sec + 189]; batch = [[s5+124+4 sec]+84] + 64 b; if mode and b >=
     0 and [batch+12]: 0x26AF50(state, batch, mode, 0); passes 1..3 (if
     [0x665F20]): the same with [s5+196+4 sec], mode = ([s5+88+2 sec] >> 2
     pass) & 3, b at + 272, cull 1; then 0x26B2B0(state)`, with
     0x26BAA0(state) whenever the pass (the byte at state + 153) changes.
     So a batch is one material in one of 17 sections, the sections' modes
     are worked out before (0 unseen, 1 inside: no test, 2 and 3: the box
     test), and the order is by material. A second loop (the instanced
     props: [s4+52] records of 80 bytes at [s4+56]) follows.
     0x26AF50 (153 instructions) is: the box test unless mode 1 (VU0
     0x0A18 on the batch's bounds and the camera's planes at 0x01E327D0 +
     96 cam: 0 out, 2 in), then into the chain a CALL tag to the render
     data [batch+12], the qword at state + 0, the packets waiting at
     [gp-23648], two qwords of clip flags when the mode changed (state +
     16 and {mode value, cull}), and the qword at state + 32 or a CALL to
     the second packet [batch+28] with MSCNT.
     A native loop can be spliced in without replacing the walker: an
     override of 0x26AE30 that on its first call of a walk does all the
     groups itself and then zeroes [0x70002003], [0x665F1F] and [0x665F20]
     for the rest of the game's first turn (restored in an override of
     0x26B2B0, which skips that one call). Not built: what it would save
     is the loop in recompiled code and the test's trip through the VU0
     registers, about 1%; the larger part is on the chain's side.
   - **Done that day: the packets' parses remembered** (`pc_cached` in
     hle_render.c behind `nw_packet_cached` and `nc_packet_cached`,
     autopilot `parsecache 0` for the old way). The parsers report the
     words they read, the VIF registers the packet set and those it used
     as it found them (`pc_word`, `pc_set`, `pc_dep`, `pc_batch` in gs.h);
     a hit needs the first and the last 24 of those words unchanged and
     the registers it leaned on the same, and puts the registers a batch
     inherited in from the VIF's now. Two places a packet (the same packet
     comes with CL 4/WL 4 and CL 3/WL 1 before it: both are kept), one
     pool of 512 KB emptied when full. In the scripted race 90% of the
     world's packets and 95% of the cars' are answered that way: the two
     parsers were 1.8% of the frame, the lookups are 0.7%. On the console
     the words compared are the same cache lines the parser read, so the
     gain there is the instructions alone.

1. **Confirm the flashing fix across a whole race,** not only at the start
   line. Other parts that use car paint (kit) should look steady too.
2. **`lazy_finish` (4.0% now).** For every block of every upload (about 250
   uploads a frame) it computes a hash (`upload_block_hash`, or
   `ext_block_hash` for data in guest RAM) and writes 5 separate per-block
   arrays. Together those arrays are about 256 KB, as large as Broadway's L2.
   - Idea, not yet measured: interleave them into one 16-byte struct per
     block so one cache line covers all five. Also skip work when an upload
     repeats last frame's data at the same place.
   - Users of the arrays: `gs.c` (about 30 sites) and `gs_gx.c` (4 reads of
     `gs_blk_ver`); `gs.h` line ~153 is an inline reader.
   - Profile with `wfn.py lazy_finish --dis` first.
   - Looked at again on 2026-10-12 (evening): of about 600 uploads a frame
     in a race, 219 are rasters read in place and 235 are palettes (1 KB:
     copied to the stage and hashed in full, about 2% of the frame); with
     the CLUT loads (2.6%) and the rest the texture path is 11-12%.
   - **The overhaul that would remove most of it:** bind textures at the
     game's level instead of following its GS uploads. The game's texture
     bind is 0x221A70(state, texture, a2, a3 = context, f12 = LOD bias): a
     texture object has +32 flags, and from +52 its raster record: +8/+12
     the TEX0 value, +20 LOD K, +22 mip levels, +24..+36 MIPTBP1/2, +88
     "uploaded this frame"; when that is clear it calls 0x221650 (the
     upload: PATH3 image transfers of raster and CLUT). An override of
     0x221650 could put a note in the chain (as the world batch command
     does) saying "this texture object is now at this TEX0", and the backend
     could key its GX textures on the source (raster and palette addresses,
     with `ext_ident`'s sampled checks for changed data) without any image
     transfer, block ownership or CLUT load. To keep: GS memory written by
     an upload must still count as overwritten for render targets
     (`mark_rect`). Not started.
   - **What the game does (read 2026-10-12, user: work at the game's level,
     not the PS2's):** it keeps its own cache of textures in GS memory, two
     halves used in turn. 0x221650(state, tex) places a texture when it is
     not in the current half (or empties the other half first: 0x222030 per
     texture clears its "placed" mark), 0x221850(state, tex) records it,
     calls 0x221E80(state, tex, base) to send it and moves the texture's
     TEX0/MIPTBP (record at tex+52: +8/+12 TEX0, +16 the CLUT's offset in
     blocks, +24..+36 MIPTBP1/2, +48 its size) to `base`. The halves are
     smaller than what a race frame uses, so nearly everything is placed
     again every frame: that is the 600 uploads.
   - **The upload itself is a prebuilt script** at [tex+36]: word 0 the
     number of entries, entries of 64 bytes from +16. An entry is four
     qwords copied into the chain: a DMA cnt tag with VIF DIRECT, a GIF tag,
     BITBLTBUF (0x221E80 adds `base` to its DBP: the entry holds the offset
     within the texture) and a DMA ref tag to a packet in the asset: TRXPOS
     0, TRXREG (w, h), TRXDIR, an IMAGE tag and the data (so the pixels are
     at ref address + 80 bytes; a few packets hold more images after the
     first, 232 of 412 in one dump: qwc larger than 5 + the data).
     Entry 0 is the raster, sent as CT32 or CT16 whatever the texture's own
     format (4 and 8 bit textures written through the 32/16 bit layout, as
     PS2 games do), the next entry is mostly an 8x3 CT32 palette (16x16
     for 8 bit ones). `work/pp_221A70.txt` has the bind function.
   - **Plan:** override 0x221E80 to put one note in the chain (script
     address and base) instead of the entries; the backend keeps a table by
     TBP of what the game placed there (script, the entry's shape, the data
     address, an identity from `ext_ident`) and `tex_lookup`/`tlut_lookup`
     ask that table first, decoding from guest RAM with the row decoders
     that exist (`gs_upload_row8` reads 4/8 bit rows out of data laid out
     as CT32/CT16). A placement is valid while `gs_blk_ver` of its first
     block is not newer than the note (something else written there: car
     paint palettes are sent another way and must win). No per-block
     ownership for placed textures. Check with the flicker tests
     (`work/flk.sh`, gxflags 4096) and `upchk`.
3. **`nc_shade` (cars) and `nw_lit_rgba` (world colours).** Not for GX
   hardware lighting: the car's light is a quadratic form of the normal
   (m33 + n.b + n'Mn), the world's a power of a reflected view vector.
   What is left is cheaper arithmetic (paired singles) or fewer vertices.
4. **Tiny non-lazy palette uploads** (16×4 CT16, 8×3 CT32) go through
   `xfer_run` → `put_row` per word. Add a direct path for small CLUT
   uploads.
5. **Car select's remaining VU1 work:** some lit batches still fall back,
   and the lit colour array (192 KB a frame) overflows there. Overflowing
   batches are sent immediate, which is native but slower.
6. **Remaining VU0 entries:** 47F6D0@0B50/07D8 and 4787D0@0388/0390/0550.
   Also VU0 dispatch overhead in general.
7. **Sky generator:** about 1% is left in `vu1_46A590` outside the hooked
   subroutine.
8. **Keep `docs/native-renderer.md` up to date** ("Native CPU code
   (performance)" section).

## 7. How to add native code safely (the established pattern)

- **Exactness first: every replacement checks itself against the recompiled
  code in the first N calls and logs differences.**
- **VU0 entry:**
  1. Disassemble with
     `python -I work/vuimg.py CHAIN out.bin; python -I tools/vu/vudis.py out.bin --start X`.
  2. Read `gen/vu0_*.c` for timing questions, e.g. which Q value a `mulq`
     sees, or the branch delay slots.
  3. Write one C statement per VU instruction in `runtime/vu.c`.
  4. Add `{chain, pc, tpc, fn}` to `k_vu0_native[]`.
  5. A race run must log 0 differences.
- **VU1 subroutine:**
  - Add `VU_NATIVE_SUBS = {chain: {bal_target: "c_function"}}` in the config.
  - `vurecomp` then spills registers to `VuCtx` at that `bal` and calls
    `int f(VuCtx *)`. A return of 1 means done; 0 runs the microcode.
  - Add a weak stub returning 0 in `runtime/vu.c` for the PC build.
- **EE routine:**
  - Add `OVERRIDES[addr] = "c_function"` in `config/burnout3.py`, implement
    it in `runtime/`, and regenerate.
  - Self-check against the original `f_XXXXXXXX` for the first calls (see
    `runtime/hle_audio.c`).
- **Native render pipeline:** see `docs/native-renderer.md`.
  - The PC host's software GS is the ground truth.
  - PC comparators: `B3_CARCMP=1` and `B3_WORLDCMP=1` via `work/pcdump.sh`.

## 8. Pitfalls (each of these cost time before)

- **Timing runs in Dolphin must be read from outside the game.** The game's
  log lines are written by a low-priority thread; anything that keeps the main
  thread busy (the idle spin under Dolphin) holds them back and they come out
  in bursts, so the wall-clock stamps of status lines mean nothing then (the
  kernel now sleeps in pieces while lines wait: `plat_log_pending`). Use
  `work/dumprate.sh` (audio recorded per wall second) or the log's stamps only
  when the build is known to log on time. Runs were also spoiled by my own
  shell: `ps | grep wiishots | kill` matches the command that runs it; and
  `pkill` does not exist in this shell (kill by pid).
- **A change of how the kernel waits can cost Dolphin its speed with the game
  looking fine** (idle time, fps and the game's clock unchanged): the host's
  short sleeps end late. Check wall time per game second before keeping it.
- **Memory is tight.**
  - MEM2 has about 5 MB free after init, and MEM2 is a bump allocator that
    never frees.
  - Growth of MEM1 BSS pushes libogc's heap into MEM2.
  - Adding about 1 MB of static buffers broke render-target copies ("gx:
    cannot allocate … texture memory").
  - Keep new buffers small. Current sizes: `DL_MAX` 2048, disc cache 8×32 KB,
    card cache 16×8 KB, profiler buckets 17 MB/64.
- **gcc 15 ICEs on some loops** (gs_backend_sync). Move the loop into a
  `noinline` helper (see `rt_reads`).
- **Interrupts:** use `IRQ_Disable()`/`IRQ_Restore()`.
  `_CPU_ISR_Disable` is not declared.
- **Bash heredocs mangle backslash escapes** (`\n` becomes a real newline).
  For edits that contain escapes, use the Edit tool or write a `.py` patch
  script to the scratchpad and run it with `python -I`.
- **Dolphin keeps EFB copies on the GPU by default,** so CPU readbacks of EFB
  copies read zeros there. Don't rely on them.
- **Low frame rates can be a screen, not slowness.** Low frame rates in boot
  turned out to be dialogs, not loading; identify the screen by draw count.
- **Display-list cache churn:** `dl_check` once read the scratchpad when
  `col == 0`, which rebuilt lists every frame. Watch the "drops" counter in
  the gx status line.
- **Never leave two PC host processes running:** both read `work/cmd.txt`.
- **Static tables in .bss hang the game silently.** +216 KB of debug tables
  (MEM1 arena 424 -> 224 KB free) froze it at the title screen: no FATAL, no
  more status lines. Allocate debug tables with `calloc` on first use; watch
  the `mem:` status line.
- **The user plays in the Dolphin window during test runs** (throttle,
  steering): a run can end up elsewhere on the track. Check the screenshots
  before trusting a scene-specific result.
- **The autopilot car crashes into the first barrier** when it holds
  accelerate only; speed-dependent effects show around 245-255 s.

## 9. Further reading

- `docs/native-renderer.md`: renderer map, pipelines, render targets,
  performance notes.
- Persistent agent notes, if your harness loads them:
  `C:\Users\sherv\.claude\projects\C--Users-sherv-Desktop-Projects-burnout3-Wii\memory\`
  (`MEMORY.md` index, workflow, profiler, VU0 native entries).
