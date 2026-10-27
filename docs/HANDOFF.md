# Handoff (2026-10-27)

Where the work stands and how to pick it up on another machine. Details and history are in `RECOMP_NOTES.md`.

## Goal

At least **50 fps in the match**. The first arena now runs at about 48 fps on average (50 in light scenes, ~44 in the
slowest tenth of frames; Windows, Ryzen 7 7800X3D) since the vblank hold (RECOMP_NOTES "Slow frames: the vblank
held while a frame finishes") and sc3-port's faster VU1 AOT on the fork's main.

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

- **Playable.** Boot, movies, menus, loading, the match (driving, weapons, AI, sound, HUD), deaths, respawns and
  the game-over menu (REPLAY restarts the match). No freeze in ~10 scripted play runs since the 989snd fix
  (RECOMP_NOTES "Freeze while playing"). Only the first (rainy) arena has been played since 2026-10-26; the memory
  card and the rest of story mode are not checked recently.
- **Renderer.** The GPU renderer (paraLLEl-GS, Vulkan) is TMB's default; `PS2X_GS_RENDERER=cpu` for the CPU
  rasterizer.
- **VU1.** All microcode is compiled ahead of time on the fast path (`scripts/recomp.sh`, PCSX2-like floats,
  `TMB_VU1_EXACT=1` for the exact model). The rain's loop is native code (`runtime/vu1/tmb_vu1_rain.cpp`, on the
  fork's lifted entries). Every VU1 change is checked with `tmb_vu1_replay` on `build/vu1_match.trace` (6000
  runs) and `build/vu1_rain_long.trace` (200 rain runs): results identical.
- **What limits the frame.** A frame's work starts at a vblank; one that overran its field used to wait a whole
  second field (40 ms). The vblank hold (`TMB_VBLANK_HOLD`, default 6 ms) lets it take only the overrun. What is
  left are frames whose VU1 work (the VIF1 thread) goes well past the field. The EE only idles on the end-of-frame vblank; skipping mid-frame waits, the GS lag and VU1 on two
  threads were measured and gain nothing (RECOMP_NOTES).
- **Known gaps.** A small striped rectangle under "BLACK" on the title logo. The game-over menu fix came with the
  fork update of 2026-10-27; which commit fixed it is not known.
- **Arenas.** Challenge mode reaches the eight unlocked arenas (Freeway is locked): scratchpad-style runs are in
  RECOMP_NOTES "Arenas"; one match each was being measured on 2026-10-27.
- **The room (c2c).** Other ports share the fork's main; their generic fixes reach main after a TMB check (fork
  tests, `tmb_vu1_replay` on both traces, a scripted play run). Equal-priority EE time slicing is a runtime switch
  now (`setEqualPrioritySlicing`, `TMB_SLICE_AB=1` for an in-run A/B); whether to turn it off by default is open.
- **Testing aids.** `tools/play_script.py` (a pad script that plays), `TMB_PLAYER_HEALTH_CAP=5` (game over within
  minutes), `PS2X_SHOW_FPS` (F3), `TMB_TRACE_FRAMES`, `TMB_RAIN_LIFT_AB` (in-run A/B pattern to copy).

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
- **Windows: discard the first runs after a build.** Right after a build, the first runs measured twice as slow
  (53 against 27 ms per frame, same command), probably Defender and the indexer scanning the new files. The first
  `PS2X_GS_RENDERER=parallel` run also pays for the driver compiling its pipelines. Measure on warm reruns.

## Next steps

1. **~~Game thread: speed up the IOP interpreter.~~** Tried on 2026-10-26 and dropped: in the match the IOP is
   idle ~93% of its clock (~55k instructions a frame), so there is about 0.5 ms to win, and the block version
   made the EE wait longer for 989snd (RECOMP_NOTES, "IOP block interpreter"). The original plan, kept for
   reference: `ps2xIOP/src/emulator/iop_emulator.cpp` runs `step()` on
   every instruction. Each step checks import stubs (`imports.decode` reads memory), interrupts, DMA scheduling,
   SPU2 events and pending callbacks. Pre-decoded basic blocks, with those checks only at block boundaries and
   invalidation on writes to code, could cut the IOP's cost by 2 to 3 times. Then look at SPU2 mixing (10% of
   the game thread: `voiceSample`, `reverb`, `mixCore`).
2. **VU1 in busy scenes.** The fast VU1 path is the default (`scripts/recomp.sh`; RECOMP_NOTES "Fast VU1 path"):
   45 fps against 41-43 in play. Two threads are not worth it (RECOMP_NOTES "VU1 on two threads"). Next: lift
   the hot microprograms to native code: the rain is done (`runtime/vu1/tmb_vu1_rain.cpp`, RECOMP_NOTES "The rain,
   lifted": ~4x cheaper, +1.5-2 fps in heavy rain); next the per-object program at pc 0x1ad0 (a third of VU1's
   cycles, every arena), with the same method: `PS2X_VU1_RECORD_PC`, a probe for its cycles, the replay.
   `tools/vu_disasm.py` reads the code images `tmb_vu1_replay <trace> dumpcode <dir>` writes.
3. **Afterwards.** Measure the other arenas. Reduce the GS thread's driver overhead: half of its time
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
    `PS2Recomp/` (564 tests);
  - the IOP suites: configure `build/ioptests` from `PS2Recomp` with `-DPS2X_IOP_BUILD_TESTS=ON`.
- Investigate a failing or slow run before relaunching it. Long builds go through
  `systemd-run --user --unit=tmb-build ...`.
