# Handoff (2026-10-25)

Where the work stands and how to pick it up on another machine. Details and history are in `RECOMP_NOTES.md`.

## Goal

At least **50 fps in the match**. Speed comes before sound and controls. The match runs at about 24 fps today:
most frames take two vblanks (40 ms), some already take one (20 ms).

## Setting up a new machine

1. Install the requirements in the README (CMake, Ninja, GCC 13+, FFmpeg dev libs, X11/Wayland + OpenGL
   headers, bsdtar, an AVX2 CPU).
2. For the GPU renderer: a Vulkan driver and the Vulkan loader (Arch: `vulkan-icd-loader` plus `vulkan-intel`,
   `vulkan-radeon` or the NVIDIA driver). paraLLEl-GS and Granite are fetched by CMake at configure time. Their
   shaders come precompiled, so no shader compiler is needed.
3. Run `git clone --recurse-submodules https://github.com/boubou666/TMB_Recomp.git`. `PS2Recomp` is the fork
   `boubou666/PS2Recomp`, on branch `main`.
4. The game data is not in the repository (never commit it). Put your own ISO of SCES-50360 at the repo root,
   then follow "Build and run" in the README: `extract_iso.sh`, `build_tools.sh`, `recomp.sh`,
   `build_runtime.sh`.
5. On Windows: MSYS2 UCRT64 with its GCC, as in the README's Windows section. Same scripts; run
   `scripts/stage_dlls.sh` after a build to start the runner outside the MSYS2 shell. The perf tooling differs:
   `TMB_TRACE_FRAMES` and `TMB_TRACE_TIMELINE` read thread CPU times through `runtime/tmb_host.cpp`
   (QueryThreadCycleTime there), and there is no `perf`; use the fork's sampling profiler
   (`PS2X_PROFILE=<seconds>`) or a Windows profiler.
6. The first build compiles about 3500 generated files. Later, a change to `ps2_runtime.h`, `ps2_memory.h` or
   `gs_frontend.h` rebuilds all of them (a few minutes with the unity build).

## Current state

- **Renderer.** The CPU rasterizer is still the default. The GPU renderer is opt-in with
  `PS2X_GS_RENDERER=parallel`: paraLLEl-GS, Vulkan compute, in the fork in `gs_parallel.cpp`. It frees about
  100 ms of CPU per frame (the four raster threads) but gives no fps gain yet. Menus, the level loading screen
  and the match look right. Two bugs found by checking screens are fixed: SMODE1/SMODE2 were never set, and the
  PCRTC alpha 0x80 now counts as 1.0. Make it the default once it gives fps.
- **What limits the frame.** `TMB_TRACE_TIMELINE` and `tools/timeline.py` show it. Each frame, the game thread
  waits about 14 ms for the vblank. Then the game, vif1 and gs threads work together for 19 to 25 ms. If that
  fits in 20 ms the frame is one vblank, otherwise two. At two vblanks the game update runs twice (substeps),
  which makes the next frame long too.
- **Which thread is over depends on the scene.**
  - In light scenes it is the game thread. IOP plus SPU2 are about 30% of it (the IOP alone 17% since 989snd's
    voice search runs natively).
  - In busy scenes it is VU1, with 17 to 32 ms of microcode per frame. The VU1 AOT code is already tight: it
    keeps accurate flags and normalization.
- **Sound hang.** The game could freeze in `snd_SendIOPCommandNoWait` while a level loaded, about one run in
  four. The fork has two fixes:
  - an inline RPC server is no longer abandoned when it runs out of budget;
  - an interrupt that arrives while disabled is held until EnableIntr.

  It has not happened in 8 runs since, but the cause is not proven. If it comes back, look for
  `[iop:call] ... still running` lines in the log. They show where the server waits and the SPU2's last DMA
  events.

## Measuring

- **Reaching the match.** Use this pad script; the match starts about 150 s in:
  `S="25:CROSS/300"; for t in $(seq 60 6 140); do S="$S,$t:CROSS/250"; done; PS2X_PAD_SCRIPT="$S" ...`.
  `scripts/bots.sh` does the same, with position tracing.
- **Per-thread CPU.** `TMB_TRACE_FRAMES=1` prints, every 100 frames, the frame split and the CPU ms per frame of
  each host thread.
- **Timeline.** Run with `TMB_TRACE_TIMELINE=/tmp/t.txt`, then `tools/timeline.py /tmp/t.txt [frame] [count]`. It
  prints a per-frame summary and a 1 ms chart of the game, vif1 and gs threads.
- **IOP profile.** `PS2X_IOP_PROFILE=1` prints sampled IOP PCs by module and thread every 10 s.
- **Host profile.** `perf record -g -p <pid>` works for user space (`perf_event_paranoid=2`).
- **Noise.** Whole runs vary from 40 to 47 ms per frame for the same build, because of heat and of what is on
  screen. Compare similar scenes in the timeline, not whole-run averages.

## Next steps

1. **Game thread: speed up the IOP interpreter.** `ps2xIOP/src/emulator/iop_emulator.cpp` runs `step()` on
   every instruction. Each step checks import stubs (`imports.decode` reads memory), interrupts, DMA scheduling,
   SPU2 events and pending callbacks. Pre-decoded basic blocks, with those checks only at block boundaries and
   invalidation on writes to code, could cut the IOP's cost by 2 to 3 times. Then look at SPU2 mixing (10% of
   the game thread: `voiceSample`, `reverb`, `mixCore`).
2. **VU1 in busy scenes.** The real lever would be running independent microprogram batches on two threads.
   That is delicate: VU registers carry over between MSCALs.
3. **Afterwards.** Make the GPU renderer the default. Reduce the GS thread's driver overhead: half of its time
   is in the NVIDIA driver, probably too many flushes (`PS2X_GS_PARALLEL_FLUSH=0` only flushes at FINISH and
   presentation).

## Working conventions

- Commit as `Thomas Martin <304581853+boubou666@users.noreply.github.com>`, never a personal e-mail. End
  commits with the `Co-Authored-By` line used in the history.
- Push named branches only (`main`), never `--all` or `--mirror`.
- Fixes to the runtime, IOP or recompiler go into the fork `boubou666/PS2Recomp`, not into workarounds here.
  Push the fork first, then bump the submodule here.
- Run the fork tests before pushing:
  - `cmake --build build/forktests --target ps2x_tests`, then run `../build/forktests/ps2xTest/ps2x_tests` from
    `PS2Recomp/` (485 tests);
  - the IOP suites: configure `build/ioptests` from `PS2Recomp` with `-DPS2X_IOP_BUILD_TESTS=ON`.
- Investigate a failing or slow run before relaunching it. Long builds go through
  `systemd-run --user --unit=tmb-build ...`.
