# Twisted Metal: Black (EU, SCES-50360) – PS2Recomp bring-up notes

## Layout
- `game/` – full disc contents (ELF `SCES_503.60`, `MOD/*.IRX`, `IOPRP213.IMG`, data folders). The runtime uses the
  ELF's folder as `cdrom0:`.
- `PS2Recomp/` – submodule, fork https://github.com/boubou666/PS2Recomp branch `main`, shared with VP2S and Drakengard.
- `recomp/tmb_sces50360.toml` – **current config**: `ps2_analyzer` output (352 SDK stubs bound by address, 326
  entry points, 1 jump table at `0x4ee6a0`), output redirected to `generated/`.
- `runtime/tmb_overrides.cpp` – game override module, matched on ELF name `SCES_503.60` + entry `0x100008`.
- `scripts/` – Linux shell scripts for every stage; `CMakeLists.txt` links `generated/*.cpp` + `runtime/*.cpp` into
  the fork's `ps2EntryRunner` (the fork's placeholder `register_functions.cpp` is removed from the target).

## Rebuild / run
Tools: `build/tools` (ps2_recomp, ps2_analyzer, Release). Runner: `build/game` (RelWithDebInfo).

    scripts/build_tools.sh        # once
    scripts/recomp.sh             # < 1 s: 3,572 functions, 3,575 files, 72 MB
    scripts/build_runtime.sh      # unity batches of 32 generated files
    scripts/run.sh

## The ELF
- Not stripped: 7,681 symbols, 3,571 `FUNC` entries, `.mdebug` section, C++ mangled names (GCC 2.x style, e.g.
  `uiTitle_Main__Fv`), plus 16 `.DVP.overlay` sections (VU microcode overlays, `.vutext` at `0x22b5e0`).
- Entry `0x100008`, `.text` `0x100000`-`0x22b5dc`, `.data` `0x235f80` (2.8 MB), `.bss` `0x504480` (2.4 MB).
- SDK calls seen by the analyzer: libgraph (`sceGs*`), libdma, libpad, libcdvd, libmc, libsif (`_sceSifLoadModule`),
  libdev, newlib (`printf`, `__sfvwrite`...), libstdc++ 2.x EH (`__throw_*`, `__dynamic_cast`).

## Recompiler results
- 0 errors, 124 `control-flow` warnings: unresolved `JR/JALR` (vtable calls, EH, `_fwalk`, `__sfvwrite`,
  `printfloat`...); each got fallback entries promoted, so they resolve through `runtime->lookupFunction` at run time.

## Fixes applied
Runtime (in the fork):
1. `ps2xRuntime/CMakeLists.txt`: `-march=x86-64-v3` on GCC/Clang x86 (`PS2X_X86_MARCH`). The runtime's SSE4.1/AVX2
   intrinsics only had `/arch:AVX2` on MSVC; on Linux `ps2_memory.cpp` failed with "inlining failed in call to
   always_inline `_mm_extract_epi64`". Applied PUBLIC on `ps2_runtime` so the runner and generated code match.

Game-specific:
2. `runtime/tmb_overrides.cpp`: the CD device gets the disc image (`TMB_ISO`, else the first `.iso` walking up from
   `game/`). Right after boot `fileCdRead` (0x1c4108) calls `sceCdRead(LBN 0x10, 1 sector)`, the ISO primary volume
   descriptor, which the file-backed `cdrom0:` cannot serve ("unresolved LBN 0x10").
3. `recomp/tmb_sces50360.toml`: `ret0` stubs on the Logitech wheel path (`lgDevInit`, `inputLgUpdate__Fv`, the
   `inputSetLg*`/`inputStartLgEffect` force-feedback helpers). See "First boot" below.

## First boot (2026-10-22)
Log of the first run (`build/run1.log`): ELF loaded, override applied, `SifInitRpc`, then the game loads its IRX
modules through `sceSifLoadModule` in this order: SIO2MAN, MTAPMAN, MCMAN, MCSERV, PADMAN, LIBSD, 989SND, USBD,
LGDEV. All run in the IOP emulator; 989SND prints its banner. `USBD.IRX` and `LGDEV.IRX` return start=1
(NO_RESIDENT_END: no USB host controller in the emulator), so LGDEV never registers RPC server **0x046d**
(the Logitech vendor id). `lgDevInit` (0x210b08) then loops forever: `sceSifBindRpc(cd, 0x046d)` returns >= 0 but
`cd->server` (offset 0x24) stays 0, so it delays 0x10000 iterations and binds again. Observed as `pc=0x210b30`
on every `[run:tick]` with one active thread. Only `inputInit__Fv` calls it and ignores the result; `inputLgUpdate__Fv`
is the only other entry into the lgDev API. Stubbing both (and the force-feedback helpers) removes the wheel support
until the IOP side exists. A proper fix would be OHCI emulation in `ps2xIOP` so `USBD.IRX` starts.

With the stubs the boot continues: the game loads SIO2MAN..LGDEV, inflates its shell data (`zipInflateCodes`),
enters `uiMain__Fv` and draws 38 textured sprites per frame to `fbp=351`. The screen stayed black.

## Black screen: libgraph image stubs (2026-10-22)
`PS2X_GS_DUMP` showed the sprites sampling a 128x128 PSMT4 texture at `tbp0=8959` (CLUT `cbp=8991`) that decoded
to all zeros, while VRAM only had a PSMT4 upload at block 6136. `fontDmaFontData__Fv` uploads the font with
`sceGsSetDefLoadImage` + `sceGsExecLoadImage` (both bound to runtime HLE by the analyzer). Two bugs in the HLE:
- `sceGsExecLoadImage` scaled `vram_addr` by 2048/256 as if it were a FRAME.FBP page index; libgraph's `vbp` is
  already the BITBLTBUF unit (64 words). 8959 x 8 mod 16384 = 6136, exactly where the font landed.
- `sceGsSetDefLoadImage` wrote a private 12-byte record into the guest `sceGsLoadImage`. The real struct is a
  6-quadword GIF packet (giftag0, BITBLTBUF, TRXPOS, TRXREG, TRXDIR, giftag1) and the game relies on it:
  `blitToFrameBuffer` rewrites giftag1 at +0x50 and DMAs the packet itself. `fontInit` spaces its two structs
  0x60 bytes apart, which pins the size (a first attempt with 0x70 clobbered the glyph UV table that follows).
Fixed in the fork (`Kernel/Stubs/GS.cpp`, `Helpers/Support.h`): real layouts for load and store, Exec stubs read
BITBLTBUF/TRXPOS/TRXREG back from the packet, no unit conversion. Result: the PAL language selection screen
(ENGLISH / FRANCAIS / ESPANOL / DEUTSCH / ITALIANO) renders correctly and waits for input (`inputAnyKey__Fi`).

Also in the fork: `PS2X_GS_LOG_LIMIT=<n>` raises the 48/128 caps on the `[gs:gif]`/`[gs:reg]` logs.

## Copyright movie hang (2026-10-22)
With `PS2X_PAD_SCRIPT="25:CROSS/300,..."` the language choice is accepted and the game plays the English
copyright notice, which is a movie: `MOV/COPYRTE.PSS` (streamed with `sceCdStStart` at LBN 0x95a3;
`tools/iso_lbn.py <iso> <lbn>` maps an LBN to its file). The stream stopped after 992 of 2889 sectors and the
main thread then looped in `updateMpegMovieDecoding__Fb` on `videoDecFlush` (pc 0x1e00f8/0x1e0100), with the
last language-screen frame left on display.

The player is Sony's ezmpeg sample (`videoDec`, `viBuf`, `audioDec`, `strFile`, `readBuf`). The runtime's libmpeg
HLE decodes video itself from `sceMpegDemuxPss*` with FFmpeg, and also forwards each M2V packet to the game's
`videoCallback`, which copies it into `viBuf`, the IPU input ring. On hardware the IPU drains that ring through
DMA channel 4, kicked by `viBufAddDMA` from the `mpegNodata` libmpeg callback; the HLE never calls `mpegNodata`,
so the ring only fills. `videoDecFlush` needs 4 free bytes to append the sequence-end code, never gets them, and
spins forever.

Fix (game side, `recomp/tmb_sces50360.toml`): `ret1@0x001ED4D8` makes `videoCallback` accept and drop the packet.
The copy has no reader with the HLE. A generic runtime fix would either skip M2V stream callbacks or emulate
the IPU-side drain, but other ports rely on the current behaviour, so it is left to the fork later.

`scripts/recomp.sh` now generates into `build/recomp-staging` and rsyncs by content into `generated/`, so a
config change rebuilds only the touched unity batches instead of the whole game (~25 min).

## Movies end, title screen reached (2026-10-22)
Two more movie problems, both in the runtime's libmpeg HLE (fixed in the fork):
- **End of movie.** `sceMpegIsEnd` only trusted a runtime CD stream EOF (`sceCdStStop` or the end LBN given to
  `sceCdStStart`). TMB opens movie streams with endLbn 0xFFFFFFFF, stops reading at the file size, and calls
  `sceCdStStop` (`strFileClose` in `termAll`) only after the movie ended, so the HLE never ended it. With
  `TMB_TRACE_MOVIE=1` the HLE summary showed the whole PSS demuxed, 296 pictures served, `ended=1 eof=0`, while
  the decode thread spun in `decBs0` waiting for a free `voBuf` slot. The MPEG program end code (0x000001B9) now
  counts as producer EOF.
- **Skip.** `ps2_stubs::abortMpegPlayback` ends a playback on demand; `runtime/tmb_overrides.cpp` calls it from a
  `videoDecAbort` (0x1ff920) override, because the ezmpeg abort protocol (sequence_end_code into the IPU ring)
  never reaches the host decoder.

The movie display itself works: the game's `vblankHandler__Fi` (INTC VBLANK) uploads `voBuf` frames with
`loadImage`. After `COPYRTE.PSS` the game streams `INTROPIB.PSS` and reaches the title screen (Sweet Tooth,
ONE PLAYER menu, "x select"), then starts the attract demo ("LOADING DEMO").

`TMB_TRACE_MOVIE=1` (in `runtime/tmb_overrides.cpp`) logs the player's thread teardown (`termAll`), PCM callbacks,
`audioDecSendToIOP` state, and every 100th displaying VBlank with the HLE summary. Movie audio is off in this
player (`isAudioOK` short-circuits), so no PCM reaches `pcmCallback`.

## Clean title screen (2026-10-22)
The shell uses both display circuits (PMODE 0xFF03): circuit 2 shows the movie layer (fbp 160, 640x368), circuit
1 a UI layer (fbp 351) blended over it by its per-pixel alpha. The language menu text stayed in the UI layer
because the layer was never cleared. Three runtime problems, all fixed in the fork:
- `sceGsSetDefClear` was an empty stub: the clear packet in the game's `ieGsDBuffDc` stayed garbage.
- `sceGsPutDrawEnv` applied a fixed 8 register pairs read from the giftag address (the giftag itself as a
  register, TEST_1 dropped, clear never run). `ieGsPutDrawEnv` patches NLOOP to 16 or 22 (drawenv + clear) and
  relies on libgraph DMAing giftag + NLOOP quadwords; the HLE now does exactly that.
- With the layer cleared every frame the UI vanished: the host loop latched the presented frame whenever it saw
  a new vblank tick, i.e. in the middle of the next guest frame (cleared, not yet redrawn; `PS2X_GS_SYNC=1`
  hid it). The frame is now latched in `EeScheduler::processEvent` at the guest vblank, only when a thread
  waits on it (frame complete), with a fallback every 3 vblanks.
Result: logo, ONE PLAYER / MULTI-PLAYER / OPTIONS / CREDITS / LOAD GAME, button legend; no leftover text.

Debug notes: the GS dump directory mode starts with `PS2X_GS_DUMP_AFTER=<s>` (not `PS2X_GS_DUMP_AT`, which only
drives the single-file dump); `tools/gs_fb_decode.py` decodes a PSMCT32 buffer from `vram_after.bin` (RGB + alpha).

The band of texture garbage under the title movie was circuit 2 showing 2048 lines: `setPMODE__F12PMODESetting`
builds DISPLAY2 from the movie's `sceMpeg.width/height` (0x4c7a30), which the HLE `sceMpegReset` zeroed
(DH = height - 1 = -1). Fixed in the fork: reset keeps the size, like libmpeg.

## Menus to the first match (2026-10-23)
Driving the title with Cross presses (`PS2X_PAD_SCRIPT`), ONE PLAYER plays `TTL_GM.PSS` then `GM_CS1.PSS`, the
level select (Yorke Bros. Scrap & Salvage, level 1 of 8), the loading screen, then the match. Four blockers:
- **Movies that never end.** Every PSS is N sectors + 4 bytes, the 4 bytes being the program end code. The
  player treats a stream as complete with < 5 bytes left, never reads them, and calls `videoDecFlush` (sequence
  end code into the IPU ring). The first two movies only ended because the runtime read past their end.
  `runtime/tmb_overrides.cpp` overrides `videoDecFlush` (0x1ff9e0) to call the fork's `endMpegPlaybackInput`.
- **Level load hang in 989snd.** `snd_SendIOPCommandAndWait` waited forever for the reply to command 3 (load a
  sound bank by LBN, here `SND/EXPLOS.BNK`). `PS2X_IOP_TRACE_RPC=1` showed the 989snd RPC function exhausting
  its 2M instruction budget at IOP 0x5f870 (`tools/irx_disasm.py`): it records `GetThreadId()` and
  `SleepThread()`s until its loader thread wakes it, which an inline call with no thread cannot do. The fork now
  runs RPC server functions as their `sceSifRpcLoop` thread and runs the other threads while it is blocked.
- **Match frozen in `particleDraw`.** It waits for two flags (gp-0x32F8/-0x32F4) that `hierFlushObjQ` clears from
  the VIF1 DMA completion handler. `hier` masks VIF1 around each kick (DisableDmac/EnableDmac) and the runtime
  completes the transfer inside the kick, so the completion was dropped. The fork keeps masked DMAC completions
  pending and raises them when the channel is enabled again.
- **Segfault in the lens flare Z test.** `sceGsStoreImage` is 0x70 bytes (vifcode[4] first); the fork read it at
  the 0x60 layout and copied a huge image. Fixed, and the copy is clamped.

The match renders: car, arena, HUD (radar, weapons, health, "CAR TELEPORTED"). The frame is 640x256 (field
rendering), shown squashed.

New bring-up switches in `runtime/tmb_overrides.cpp`: `TMB_TRACE_THREADS=<s>` (guest threads, MPEG HLE summary,
GS display registers; `TMB_TRACE_IOP=1` adds IOP threads and SPU2), `TMB_TRACE_SND=1` (989snd commands),
`TMB_TRACE_HIER=1` (VIF1 completion handler). `tools/irx_disasm.py` disassembles IRX code at its IOP address.
IOP stacks cannot be sampled by attaching (ptrace_scope=1): run the game under `gdb -batch` and send SIGINT.

## Match display and speed (2026-10-23)
In the match the game displays circuit 2 only (PMODE 0x8002): PSMCT24 buffers at fbp 80 and 160 flipped per
frame, 640x256 progressive (SMODE2 0, DH 255), a real 256-line mode that a PAL TV shows full height. The host
window kept square pixels and showed it squashed; the fork now presents every frame at 4:3 (`PS2X_PIXEL_ASPECT=1`
restores square pixels). Captures (`PS2X_CAPTURE_DIR`) stay at the raw frame size.

The match ran at about 4.4 game frames per second (buffer flips per 50 Hz vblank). The runtime's aggressive logs
also enabled `PS2_FUNCTION_LOG_TRACKER`, which appends and flushes a line in `ps2_log.txt` on every recompiled
function entry and exit (1.7 GB written in the working directory). The fork now keeps it behind
`PS2X_ENABLE_FUNCTION_LOG_TRACKER` (off); the other diagnostic logs stay. The match now runs at about 26 game
frames per second.

## Speed: the real numbers, VU1 microcode compiled ahead of time (2026-10-23)
The "26 fps" above was measured on a near-empty view: the car had fallen through the arena ("CAR TELEPORTED").
In the normal match view the game ran at 7-8 frames per second. Two threads were saturated: the EE thread, 83% of
it in the VU1 interpreter (about 840,000 instruction pairs and 4,000 microprogram runs per frame), and the GS
thread in the software rasterizer (about 900,000 pixels per frame).

`perf` works without root (Arch package extracted to `~/.local/opt/perf`, `perf_event_paranoid` 2 allows user-space
sampling of own processes); a wrapper is in `~/.local/bin/perf`.

VU1, all exact against 8000 runs recorded in the match (`PS2X_VU1_RECORD`, replayed by `ps2_vu1_replay`):
- the interpreter now shares one inline implementation with compiled code (`ps2_vu1_inline.h`, `stepPair`);
- FMAC flags: SIMD fast path, exact results of zero/denormal/near-FLT_MAX lanes as doubles;
- `ps2_vu_aot` (fork, built by `scripts/build_tools.sh`) finds the VIF MPG uploads in the ELF (16 fragments, 3605
  pairs for TMB, both microcode images) and writes C++ to `generated/vu1/`, one function per pair calling
  `stepPair` with a constexpr decode. Built at -O3 without debug info and outside the unity batches.
- 76 ns per VU cycle before, 55 interpreted, about 30 compiled.

The EE thread now runs at about half its former load; the GS thread is the limit. Its existing multi-threaded
mode (each worker draws the pixels of its VRAM blocks) is now on for TMB with 4 workers: 8.8 fps on the GS thread
alone, 12.3 with 4 workers, 10.4 with 8 (every worker sets up each of the many small triangles), measured with the
car idle at the spawn point (`build/bench.sh`, presses stopped once the match starts).

## Cars flying off and sinking through the arena (2026-10-23)
In about half the runs the player's car, idle at the spawn point, was flung kilometres away or sank through the
ground ("CAR TELEPORTED"), and AI cars printed "Skid plate collision returned negative distance ... of -nan".
- The "-nan" was the printf HLE: it read a double passed in a3 from t0/t1 (O32 slot rule). The EE passes each
  argument in one 64-bit register; fixed in the fork. The distances were ordinary negative numbers.
- `TMB_TRACE_BLACKBOX=<file>` (runtime/tmb_overrides.cpp) keeps the player vehicle (0x259c70, 0x2200 bytes) of the
  last 64 frames and writes them once its position goes past 50000. The first bad field was the third row of its
  rotation matrix (+0xF0): (0.149, -0.989, 537200) instead of about (0, 0, 1), i.e. a row divided by 1.86e-6.
- `mathfNormalize` computes the length with newlib's inline `sqrt.s $f1, $f12` after summing x*x + y*y in $f0. On
  the EE, SQRT.S takes its operand from ft; the recompiler took fs, so the length lacked the z term. Fixed in the
  fork (33 sites in TMB). Six runs since: no blow-up.
- The game also integrates its physics over the measured frame time (T0 at 576 ticks per ms, `timerGetFrameTime`)
  without a limit: 80-100 ms per frame at the current speed instead of 20. `timerEndOfRealTimeProcessing` is
  overridden to cap it at 40 ms (`TMB_MAX_FRAME_MS`): below 25 fps the game runs in slow motion.

## GS rasterizer and VU flags, where the time goes now (2026-10-23)
`PS2X_GS_RECORD=<file>` (fork) records the GS command stream in the game; `ps2_gs_replay <file> [repeat]` times
it and prints a hash of the final VRAM, so a rasterizer change can be checked bit for bit. On 96 match frames
(`build/gs_match.trace`):
- 4 workers 4.35 s -> 2.1 s, serial 8.8 s -> about 6 s, VRAM identical throughout.
- The workers sat idle 1.5 s of the 4.35: TMB keeps its 8-bit textures (T8H, T4HL) in the top byte of its Z24 and
  CT24 buffers, which 24-bit writes never touch, but the overlap checks counted those primitives as sampling what
  they draw and drew them serially. The checks now know which byte each format reads and writes.
- 248,000 CLUT loads (one per primitive, 92% repeating the previous one) were made by every worker and dropped
  the cached palettes: repeats are now skipped and CLUT serials identify contents.
- Workers own whole pages (a bounding box test skips most small triangles).

In the game this changed nothing: the workers were already half idle. The EE thread is the limit, 75% of it in
VU microcode:
- The SIMD flag fast path had lost its handling of zero results (an edit gone wrong), so every zero result took
  the scalar long double path: 15% of the thread. Restored; the VU1 replay went from 40.9 to 28.3 ns per VU cycle.
- VU0 microprograms ran with a 4096-cycle limit taken for a time slice, so each pair kept the previous value of
  the registers it writes (7%). Now a run-to-completion limit.
- The VU1 microcode compiled ahead of time was built inside two unity batches at -O2 with debug info (20 minutes
  each): source properties are scoped by directory and `ps2EntryRunner` is created under PS2Recomp. Set with
  `TARGET_DIRECTORY` now: -O3 without debug info, a few minutes for the whole rebuild.

Match with the car idle: 14-17 fps (12-14 before). The EE thread is still saturated: VU1 about 65% of it, the IOP
12%, the game code 4%. Running VU1 on its own thread, as the hardware does, is the next step.

## VIF1 and VU1 on their own thread (2026-10-24)
The VIF1 DMA transfers (unpacks, VU1 microprograms, PATH1/PATH2 packets) now run on a worker thread (fork,
`PS2Memory::setVif1Threaded`, on for TMB; `TMB_VIF1_SERIAL=1` keeps them on the game thread). A transfer completes at
once for the game, its data copied when it starts; whatever observes VIF1, VU1 or the GS waits for the queue first.

Measured before writing it, with counters on every such access in the match: about 3,200 VIF1 transfers per second
(the `hier` renderer kicks one per object; `hierDmaHandler`, the channel 1 interrupt, kicks the next), the EE never
reads VU1 memory, and the only accesses that would wait are the VIF/GIF status reads of `sceGsSyncPath`, which `hier`
calls at the start of each frame for the previous one. A simulation of the timeline predicted the game thread at 0.71
of its time.

Result: about 15% more frames, counted in the same run with the worker switched on and off every 2 seconds (in-game
fps from run to run varies too much: the scene, the AI's arrival, and this laptop's temperature, 84 C under load).
The worker is now the limit (about 80% busy, the game thread waits 40% of its time in `sceGsSyncPath`): VU1 itself
costs about 60% of a frame, and runs about 10% slower on the worker than on the game thread.

Build note: a `GAME_DONE` marker in a log reused between builds can be read before the new build truncates it; wait on
the build process or unit, not on the log.

## Where VU1's time goes (2026-10-24)
On the 8000 recorded match runs (`tmb_vu1_replay`): 2.33M instruction pairs, 3.01M VU cycles, about 30 ns per pair
once the tool's own copies are left out (it copies 48 KB of VU memory per run, 8-11% of a pass).
- Experiments that drop part of the model (no stalls, no timing, no flags) looked 37-52% faster, but are not
  measurements: TMB's microprograms run timing-dependent loops, and without stalls the same runs executed 1.07M pairs
  instead of 2.33M. Compare pair counts before trusting such an experiment.
- Measured without changing semantics: 56,928 stalls per pass, about 22 ns each (1.3 ms in total); readiness
  bookkeeping about 10%, flags about 25% (computing them; queueing them only 7%), dispatch from `run` about 6%,
  the rest spread over exact float handling and execution. `-fno-stack-protector` on the compiled pairs: no change.
- 82% of blocks (72% of pairs) never stall on a write from before their entry, so stalls could be scheduled when
  the microcode is compiled, but that only removes the bookkeeping part.
- Full speed (50 fps) needs about 80M VU1 cycles per second, 2.2 s of work at the current speed: per-pair exact
  emulation at about 100 host cycles cannot get there. That takes a real VU recompiler (flag liveness, timing
  computed per block, values kept in host registers), a project of its own.

## VU1 compiled per block (2026-10-24)
`ps2_vu_aot` now cuts the microcode into straight-line blocks (fork, `VU1Interpreter::planBlock` / `runBlock`).
At compile time `planBlock` works out the cycle each pair of a block issues at, assuming every value from before the
block is ready, and which FMAC flag writes nothing in the block reads before the next one lands. At run time the
block checks those values once: one read by the first pair only that is late moves the whole schedule (the loop
case: the first pair reads what the previous iteration wrote), any other late value falls back to the per-pair path.
Results are exact on the 8000 recorded runs (same values, same cycles).

`tmb_vu1_replay`, per pass: 82 ms before, 84-90 ms with blocks alone (their checks cost what they saved), 78 ms with
dead flag writes dropped, 71 ms with the first-pair delay (most blocks fell back without it), 63 ms with one vector
blend per destination mask and the remaining slow flag cases (products outside the normal range, sums above
FLT_MAX) classified in the fast path. Match with the car idle: about 22 fps (the last 300 flips of `build/bench.sh`).

What a pair still costs (about 200 host instructions): normalizing the operands (denormals to zero, exponent 255 to
FLT_MAX), the PS2 addition (operand alignment without guard bits), flags, and the per-pair bookkeeping (readiness
tables, cycle, pc). Each is a few percent.

Build: the microcode is an object library both `ps2EntryRunner` and `tmb_vu1_replay` link, at most 4 files at once
(about 26 GB at peak, 10 minutes). Inlining whole blocks into one function took GCC 9.7 GB per file and got the build
killed by systemd-oomd, hence one function per pair body. After a fork update, regenerate with `scripts/recomp.sh`
(after `scripts/build_tools.sh`) so the microcode files match the planner.

## The GS a frame behind (2026-10-24)
With VIF1 and VU1 on their thread, no thread was busy all the time (profiled in the match: VIF1/VU1 worker about
70%, game thread 60%, GS thread 70%): each frame was still serialized, the game waiting in `sceGsSyncPath` for the
worker to finish the frame and the worker then waiting for the next one. `sceGsSyncPath` is called three times
per frame (`hier`, `particleDraw`, `Hud::Update`), and the one in `particleDraw` waited most (about a quarter of
the time).

The fork's GS lag (`PS2Memory::setGsLag`, on for TMB, `TMB_GS_LAG=0` turns it off) lets the worker finish a frame
while the game runs the next: `sceGsSyncPath` only waits for the frame before, PATH3 packets and the display
register writes (`ieGsPutDispEnv` writes DISPFB each frame) join the worker's queue in order instead of draining
it, and the worker latches the presented frame once it has drawn it. Two traps found on the way:
- CSR has to stay live. The vblank handler reads it (FIELD) and writes it back; queued, that write put back the old
  FIELD after the next vblank had flipped it.
- The movie player froze at the end of a movie. `updateMpegMovieDecoding` waits for the decoder in a loop that
  calls `switchThread` (`RotateThreadReadyQueue(1)`) at priority 0, so the priority-1 decode threads only run when
  the main thread blocks. Held back by the GS, it used to block often enough for them to queue their last
  pictures; running ahead, it reached the loop first and spun forever. The lag is off while a movie plays
  (`displaying`, gp-0x340C, read by the vblank handler override).

Measured in the match with the lag switched on and off every 100 vblanks (`TMB_GS_LAG_AB=1`): +10%, +6% and +4%
in three runs. With the lag the only wait left on the game thread is the frame before (about 15% of its time) and
the worker is about 90% busy, a tenth of it latching the presented frame (the backend flush and the copy of the
display buffer). The next limits are the worker itself and the GS thread behind it.

Follow-ups: the presentation latch now runs on the GS thread (the worker only takes the display registers, in
queue order), and compiled VU1 blocks read operands an FMAC instruction of the block wrote without normalizing
them again (2.7% fewer instructions on the replay, exact). Lag A/B after the latch move: +5% (the machine was
throttling and busy with other programs that day: compare runs only within one run).

Measuring on this laptop: after a long build it throttles (2.5 GHz instead of 4) and pages the swap back in for a
while; `perf stat` cycles and instructions compare builds better than milliseconds then.

## VU1 recompiler (2026-10-24)
Target: 50 fps in the match, so every thread under 20 ms per frame. At about 20 fps the VIF1/VU1 worker took
~43 ms per frame, the game thread ~28 ms (VU0 interpreter 38%, IOP emulation 19%, game code 11%), the GS raster
workers ~20 ms. Micro-optimizing the per-pair emulation had run out (a few percent per change: flags were already
mostly skipped by the compiler once dead, removing instructions no longer removed cycles).

The fork's `ps2_vu_aot` now compiles each block into one function with the VU registers in locals and one inline
helper per instruction (`ps2_vu1_jit.h`), still bit-exact: the recorded runs match, and so do 30000 runs recorded
in the game with the recompiler and replayed through the interpreter (`PS2X_VU_AOT=0` turns all compiled
microcode off). Replay 1.5x faster than the per-pair code, 4x the interpreter; match about 27 fps. A microcode
file builds in ~20 s (pairs inside recompiled blocks are no longer compiled one by one), the whole microcode in
under 3 minutes instead of 25.

Next for VU1: pairs that still run alone through the interpreter when a block cannot start (5% of the replay), and
a relaxed float mode (no PS2 operand truncation or operand normalization, results clamped) checked with a
tolerance on the replay's PATH1 packets. Then VU0 through the same recompiler, the IOP on its own thread, and the
GS raster workers.

Lighting flicker reported in the match (surfaces lighting up with the camera angle): the GS drew what it got, the
vertex colours switched to 255 by whole VU1 batches. Cause: the VU EFU opcodes were off by one in the fork
(interpreter and recompiler alike, so the bit-exact check could not see it): 0x79 is ERSQRT, run as ESIN. The
lighting microprogram (pc 0x1AD0) takes 1/distance of lights 1 and 3 with ERSQRT; the sine polynomial on a squared
distance gave ~FLT_MAX and every vertex in front of a headlight clamped to white. Fixed; saturated vertices with a
real spotlight in slot 1: 29% -> 0. The replay reference is now `build/vu1_match2.trace` (20000 runs recorded with
the fix and the recompiler, matching the interpreter); `vu1_match.trace` predates the fix.

The relaxed float mode (`PS2X_VU_AOT_RELAXED=1` when generating; `ps2_vu1_replay --tolerance`) only gains ~12% of
the replay's cycles: float arithmetic is no longer what recompiled blocks spend their time on.

## Open
- A small striped rectangle under "BLACK" on the title logo.
- Speed (VU1 on its thread is the limit), sound output, controls in the match, memory card.
- `timer1Handler` pacing (T0 COUNT read + up to 16 `sceGsSyncV` catch-ups per frame) to be checked against the
  real frame rate once there is motion on screen.
- USBD/LGDEV: a proper fix would be OHCI emulation in `ps2xIOP`.
