# Handoff (2026-10-29)

Where the work stands and how to pick it up on another machine. Details and history are in `RECOMP_NOTES.md`.

## Goal

The game smooth (its own 60 fps on the USA release, 50 on the European one) and fully playable as a true PC program:
no PS2 hardware simulated where the PC can do the job itself. Reached for the matches played so far:

| Release | Path | fps mean | slowest tenth |
|---|---|---|---|
| USA (`SCUS-97101`, NTSC) | native (`scripts/run_native.sh`, 2x resolution) | 58.9 | 58.5 |
| Europe (`SCES-50360`, PAL) | native, 2x resolution | 49.4 | 49.3 |

Windows, Ryzen 7 7800X3D, RTX 5070 Ti; scripted play through the menus into the first arena (`tools/play_script.py`).

## Setting up a new machine

1. Install the requirements in the README (CMake, Ninja, GCC 13+, FFmpeg dev libs, X11/Wayland + OpenGL headers,
   bsdtar, an AVX2 CPU). The native port draws with OpenGL 3.3. A Vulkan driver is only needed for the PS2 path's
   paraLLEl-GS (`scripts/run.sh`), fetched by CMake with its shaders precompiled.
2. `git clone --recurse-submodules https://github.com/boubou666/TMB_Recomp.git`. `PS2Recomp` is the fork
   `boubou666/PS2Recomp`; TMB pins a commit of its `native/renderer` branch (the native work, merged into `main` as it
   settles).
3. The game data is not in the repository (never commit it). Put your own ISO at the repo root, then follow "Build and
   run" in the README: `extract_iso.sh`, `build_tools.sh`, `recomp.sh`, `build_runtime.sh`, optionally
   `iop_recomp.sh`. `TMB_REGION=pal` builds the European release (`game/`, `generated/`, `build/game`) instead of the
   USA one (`game_us/`, `generated_us/`, `build/game_us`).
4. On Windows: MSYS2 UCRT64 with its GCC, as in the README's Windows section; `scripts/stage_dlls.sh` after a build
   to start the runner outside the MSYS2 shell. No `perf`: use the fork's sampling profiler (`PS2X_PROFILE=<s>`).
5. The first build compiles about 3500 generated files. A change to `ps2_runtime.h`, `ps2_memory.h` or
   `gs_frontend.h` rebuilds all of them.

## Current state

- **Playable.** Boot, movies (the USA release's story cinematics too), menus, loading, matches (driving, weapons, AI,
  sound, HUD, split screen), deaths, respawns, game over, saving and loading. Story mode: level 1 of 8 checked.
- **The native path** (`TMB_NATIVE=1`, `scripts/run_native.sh`), every layer of the PS2 replaced:
  - frames on the PC's clock (`timer1HandlerNative`, the fork's `FramePacer`; `TMB_NATIVE_FRAMES=0` for the vblanks);
  - the 3D from the game's own data (`runtime/tmb_native.cpp`), the other VU1 programs in the VU1 sandbox;
  - the rest of the GS stream (menus, HUD, fonts, movies) translated into OpenGL by the fork's GS native back end,
    the 3D drawn into the same targets at its markers; presentation on the GPU; internal resolution 2x
    (`PS2X_GS_SCALE`); no paraLLEl-GS;
  - the sound driver (989snd, libsd) recompiled, the SPU2 and IOP on the PC's clock.
- **Controls.** Rebindable to keys, controllers and the mouse: F2 in the game, saved to `input.ini`.
- **Known gaps.** Not checked yet: every arena and story level to the endings. Widescreen and frame rates above the
  PS2's are not started (the game's update steps in whole fields).

## Measuring

- **Check runs.** A pad script reaching the match about 150 s in: `PS2X_PAD_SCRIPT="$(tools/play_script.py 230)"`,
  with `TMB_TRACE_FRAMES=1` (frame split and CPU per host thread every 100 frames; under native frames also the game's
  fields per real second, 50.00 on PAL when it runs at its own speed). `PS2X_CAPTURE_DIR` saves the presented picture
  every few seconds, `PS2X_CAPTURE_WINDOW_DIR` the window as shown, `PS2X_WINDOW_SIZE=1280x960` a bigger window.
- **Timeline.** `TMB_TRACE_TIMELINE=<file>`, then `tools/timeline.py <file> [frame] [count]`.
- **GS native back end.** `PS2X_NATIVE_GS_LOG=1` / `PS2X_NATIVE_GS_STATS=1` (the native draws at their markers),
  `PS2X_NATIVE_GS_DEBUG=1` (native fragments magenta), `PS2X_GS_NATIVE_LOG=1` (targets, transfers, presents).
- **Shared machine.** Every build and timed run goes through the fork's bench lock (`tools/bench_lock.sh`), builds and
  timed runs alone; discard the first run after a build on Windows (Defender scans the new files).

## Next steps

1. Every arena and story level to the endings, fixing what shows up (the owner's reports first).
2. VP2S is rewriting the GS native back end's target coherence (targets sharing GS memory); check TMB at 2x on their
   branch when it lands.
3. Widescreen; frame rates above the PS2's (interpolating between game updates).

## Working conventions

- Commit as `Thomas Martin <304581853+boubou666@users.noreply.github.com>`, never a personal e-mail. End commits with
  the `Co-Authored-By` line used in the history.
- Fixes to the runtime, IOP or recompiler go into the fork `boubou666/PS2Recomp`, not into workarounds here. Push the
  fork first, then bump the submodule here.
- Run the fork tests before pushing (`ps2x_tests`, 610+ cases), and a TMB check run on both releases.
- Never edit a script a running or queued job will execute; clear a stale bench lock entry if a job dies holding it.
