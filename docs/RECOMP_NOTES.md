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

VU0: TMB runs one VU0 program (two MPG fragments, pairs 0..390, from the ELF through VIF0), about 23000 calls a
second. `ps2_vu_aot` now also builds fragments that fit VU0 code memory as VU0 code, and the runtime uses them:
VU0 went from about a third of the game thread to a sixth. `PS2X_VU0_CHECK=1` runs each call through the
interpreter too and compares (5.36M calls in a match, no difference). The game thread is now led by the IOP
emulation (~25%), then the game code (~17%).

The GS lag never ran in the match until now: it was turned on by the game's vblank handler, which only runs
while movies play, and the last movie left it off. `hier` (each frame of 3D rendering) now turns it on when no
movie plays. Measured in the same run (`TMB_GS_LAG_AB=1`, lag really on in its windows): 26.5 fps against 22.2,
+19%. The earlier +4..10% A/B results were noise. But the controls then lagged visibly: only the VIF1 queue was
bounded to a frame, and the GS thread's queue (up to 32 MB of packets) let the picture fall several frames behind.
Bounded to one presented frame (the sync also waits for the GS to have presented the frame before last), the lag
gains nothing (25.7 against 26.5 fps): the +19% was that latency. Off by default now (`TMB_GS_LAG=1`).

Frame rate in this game is quantized by the vblank wait: a frame takes 1 vblank (50 fps) or 2 (25 fps). With
both main threads around 26 ms per frame most frames take 2; every thread has to fit in 20 ms. Whole-run fps
varies a lot between runs (scene, AI, heat): compare per-thread busy time, or in-run A/B.

The relaxed float mode (`PS2X_VU_AOT_RELAXED=1` when generating; `ps2_vu1_replay --tolerance`) only gains ~12% of
the replay's cycles: float arithmetic is no longer what recompiled blocks spend their time on.

## Menus: the picture layer was 96 lines too high (2026-10-25)
Compared with screenshots of the real game: in the title and vehicle menus the background picture sat too high,
its torn edges cutting through the menu text. The menus use both display circuits (PMODE 0xFF03): the UI layer
(circuit 1, 640x512 at DY 50) over the picture (circuit 2, 640x368 at DY 146). The fork's presentation merged
them both from the top left, ignoring DISPLAY DX/DY; each now starts at its own position.

Still different: the level intro screen leaves out the character's monologue text and moves on to loading after
about 6 s. The real screen probably lasts as long as the narration; with no sound output yet, to be checked with
the sound work.

GS raster (fork, ported from two other forks by a background agent, bit-exact on `gs_match.trace`): depth test
before texture sampling and shading in blocks of eight pixels, 24% fewer cycles for the rasterizers.

## Falling through the ground (2026-10-25)
The player still went through the ground at times, mostly in fast collisions or at some angles, and bots fell
under the arena until the game teleported them back. `TMB_TRACE_POS=<file>` now writes every vehicle's position
(+0x260, z up) with the vblank tick at each `collisCheckHdResults`; `scripts/bots.sh <name>` runs the first match
with the player idle and `tools/falls.py` counts the vehicles going under z -100 (the quarry floor is at -86).
- The cars do not move over the frame time (that is weapons and debris, see above) but over the vblank fields
  since the previous frame: `timerWaitForMinUpdateRate` stores them (gp-0x2BF0, 1 to 15), `vehicleUpdate` copies
  them to vehicle+0x20C8 and `vehicleUpdatePosition` adds the velocity that many times before the collision test,
  done once per frame. The PS2 runs the match at 50 fps (one field); here a frame takes 2 to 4 fields, so each
  step went 2 to 4 times as far and fast cars landing on a slope ended up under it. Baseline: 11 falls in 19371
  vblanks of match.
- `TheGame::Update` (0x12fba8) is overridden to run once per field (at most `TMB_SUBSTEPS`, default 4), with the
  fields, the update rate (gp-0x2BEC) and the frame time split over the steps. Update ends in a tail call and may
  be preempted, so each step returns through the dispatcher to 0x12fba4 (padding before Update), registered to
  start the next step. 0 falls in 25857 vblanks, no measurable cost.
- Found on the way, fixed in the fork: the macro VU0 instructions VLQI/VSQI/VLQD/VSQD used EE RAM instead of
  VU0 data memory, and VSQI swapped its fields. TMB's matrix stack (`hierPutMatStackAsm`/`hierGetMatStackAsm`)
  wrote vf10 three times into low EE RAM and bumped vi11/vi12; VDIV by zero gave 0 instead of +-max
  (`bspInterp`).

## Where the frame goes, IOP thread (2026-10-25)
`TMB_TRACE_FRAMES=1` prints, every 100 frames, the host time from the end of the game's real-time processing to
the next TheGame::Update (W) and from there to the end (B), the EE idle time (every guest thread blocked), the time
spent waiting for VIF1/GS, and the CPU time per frame of each host thread (the fork now names them).
- In the match a frame is two vblanks (40 ms, ~24 fps). Per frame, on this laptop and depending on how hot it
  is: game thread 16-28 ms of CPU (Update with its two substeps ~11 ms, IOP + SPU2 ~8 ms in lockstep, VU0 ~2 ms),
  vif1 17-25, gs 20-28 (half of it spinning for the next packet), each of the four gsraster threads 22-30.
  Every thread is over the 20 ms of one vblank, and together they ask for ~150 ms of CPU per frame: 50 fps
  needs that roughly halved, not one thread fixed.
- IOP on its own thread (fork, `PS2X_IOP_THREAD=1`): the SPU2 mixing leaves the game thread, but 989snd answers
  about 20 RPC queries a frame (function 74, NOWAIT, 12-byte reply) that the game spins on
  (`snd_SendIOPCommandNoWait`), so their IOP code still runs on the game thread, and the hand-offs made frames
  slower (45-55 ms). Off by default.
- Run-to-run noise (thermal) is 40-47 ms per frame for the same build; whole-run comparisons cannot see 10%.
- The biggest consumer is software rasterization (4 x ~23 ms of CPU per frame). The fork's `GSRasterBackend`
  interface would take a GPU renderer.

## GPU renderer, frame timeline, sound hang (2026-10-25)
- paraLLEl-GS (fork, `PS2X_GS_RENDERER=parallel`) draws on the GPU: the four raster threads (~100 ms of CPU a
  frame) go. Menus, loading screen and match look as with the CPU rasterizer once SMODE1/SMODE2 are set (TMB's
  `sceGsResetGraph` never set them, scan-out came out 256 lines) and the PCRTC merge takes circuit 1's alpha
  0x80 as 1.0 (the driver portrait on the level loading screen was half transparent).
- `TMB_TRACE_TIMELINE` + `tools/timeline.py`: each frame the game thread waits for the vblank (~14 ms), then
  game, vif1 and gs run together for ~19-25 ms. Under 20 ms the frame is one vblank, over it two; at two the
  update runs twice (substeps), which makes the next one long too. Frame times are bimodal (20 / 40 ms).
  Which thread is over depends on the scene: the game thread (IOP + SPU2 a third of it) in light ones, VU1
  (up to 32 ms of microcode a frame) in busy ones; whole-run averages hide it, compare like scenes.
- 989snd's voice search was two fifths of the IOP's instructions: native now (fork), the IOP's share of the game
  thread went from 22% to 17%.
- The hang in `snd_SendIOPCommandNoWait` (about one run in four, while a level loads): 989snd's command server,
  run inline from `sceSifCallRpc`, used up its 2M-instruction budget polling for a sound RAM transfer and was
  abandoned, its reply (0xFFFFFFFF markers `snd_GotReturns` waits for) never written. The fork now lets the
  other IOP threads run and carries on, and keeps interrupts that come while disabled pending until EnableIntr
  (dropping them is the likely reason the transfer never ended). Not seen in the runs since; if it comes back,
  `[iop:call] ... still running` lines show where the server waits, with the SPU2's last DMA events.

## Windows port, movie skip hang (2026-10-25)
- Native Windows build with MSYS2 UCRT64 GCC 16 (same compiler family and flags as Linux, FFmpeg from pacman).
  Changes: the fork's FFmpeg prebuilt-zip path is for MSVC only (MinGW uses pkg-config); `-march=x86-64-v3` was
  skipped because CMake left `CMAKE_SYSTEM_PROCESSOR` empty (`PROCESSOR_ARCHITECTURE` missing from the MSYS2
  login shell's environment), the fork now asks the compiler (`-dumpmachine`); winmm's `PlaySound` took raylib's
  calls at link time (winmm ahead of raylib); TMB's `/proc` and `clock_gettime` thread CPU sampling moved to
  `runtime/tmb_host.cpp` (Toolhelp32, `GetThreadDescription`, `QueryThreadCycleTime` on Windows).
- With the same pad script the game hung on the copyright screen on Windows: Cross at 60 s fell inside the
  copyright movie there, skipping it. After `videoDecAbort` (state 1), `updateMpegMovieDecoding` loops on
  `switchThread` until the decode thread acknowledges (state 3). `switchThread` is `RotateThreadReadyQueue(1)`,
  written for ezmpeg's main loop at priority 1; TMB's main thread is at priority 0, so the priority-1 decode
  threads never ran. Most likely not Windows-specific (not checked on Linux): there the skip landed elsewhere. `runtime/tmb_overrides.cpp` now
  drops a priority-0 caller to priority 1 for the rotation and restores it when it is scheduled again (through
  the padding word 0x1e06cc after `readMpeg`, like `kSubstepReturn`).
- Windows speed (Ryzen 7 7800X3D, RTX 5070 Ti), same pad script, mean of the `TMB_TRACE_FRAMES` W+B over the
  match: 27 ms per frame with the CPU rasterizer, 24 ms with paraLLEl-GS (unnamed threads ~2 ms of CPU a frame).
  No Windows penalty: the first runs right after the build measured 53 and 74 ms (and 300+ ms a frame of unnamed
  threads on the first Vulkan run), and identical reruns later did not. Most likely background work on the new
  files (Defender and the indexer scanning the 650 MB exe, its DLLs and 3500 generated files) and the driver
  compiling paraLLEl-GS's pipelines once; neither was confirmed. Frames are now W ~10 ms (vblank wait) + B
  ~10-14 ms, so most take one or two vblanks around the 20 ms line.

## sc3-port backports (2026-10-25)
Arzaroth's `sc3/bringup` (Soulcalibur III, same fork) had 153 commits on top of our `fb81320`. 124 of them are now
on the fork's main, picked in order (`-x` keeps their hashes). Left out:
- Their VU1 rework (`f1fcfd3` drops our per-fragment `ps2_vu_aot` for their interpreter, then whole-image AOT on
  16 KiB code memories dumped at run time). TMB uploads its 16 microcode fragments in varying combinations, so
  whole images do not suit it; our AOT stays.
- Their pad script parser (we have one), the cores-2 raster default (TMB sets 4) and an SSE4.2 CMake flag.

What came in: recompiler correctness (64-bit signed branches, MOVZ/MOVN, aligned LQ/SQ, VF00, VCALLMSR, BC2F/T,
MMI lanes and 128-bit HI/LO, wrapping adds, VCLIP, no function in a delay slot, VU0 macro DIV/SQRT/RSQRT and
random ops), kernel (thread ids from Suspend/Resume/Wakeup, ReferThreadStatus, INTC_STAT write-one-to-clear,
every timer event delivered, due deadlines held while an interrupt is pending), DMA (chains to their end tag,
SIGNAL stalls, scratchpad interleave), VIF unpack layouts, the GS (ordered CPU events with a drawing-time
latency, CSR write-one-to-clear, 32-bit packed Z, ZTE, region wrap rows, texture span), FPU ADD.S/SUB.S guard
bit, va_list slots, a CD seek/CAV model, SPU2 MADR/BCR, IOP: no interrupt or preemption while a thread has
interrupts suspended (applied to our `runOthersSlice` too), opt-in wall-clock catch-up once per field
(`PS2X_EE_CATCHUP=field`) and their diagnostics (`PS2X_STATE_DUMP`, `PS2X_IOP_TRACE_PC`, ...).
- 546 fork tests pass on Windows. 1026 generated files changed. TMB reaches the match, 0 falls in 8331 vblanks
  of the bots match. Frame time: the two slower runs right after the build (29.9, 30.5 ms) were the post-build
  skew again; a warm run measured 24.4 ms per frame, as before (25.6, 27.0).
- The backport broke the match picture: only its top half, stretched to the window. `sceGsResetGraph` now really
  sets SMODE2, and TMB runs its matches in interlaced FRAME mode (0x3) on 640x256 buffers (circuit 2, PMODE
  0x8066). The presentation reads (DH+1)/2 buffer lines in that mode, but the backported `sceGsSetDefDispEnv`
  HLE wrote DH = h-1 = 255, so it read 128 lines. DH counts frame lines: 2h-1 in FRAME mode (fork `44ebd56`,
  with a test).
- `PS2X_EE_CATCHUP=field` on TMB: the pad script, timed in host seconds, then lands on other screens and the run
  did not reach the match in 240 s. Not looked into; TMB keeps the full catch-up.

## IOP block interpreter: tried, dropped (2026-10-26)
The handoff's plan was pre-decoded IOP blocks (interrupt, import and DMA checks at block edges instead of every
instruction). Built and tested (all IOP suites, both modes, plus a self-modifying code test), then measured:
- In the match the IOP is idle about 93% of its clock: 2.4-3.2M instructions per emulated second, 34M of the
  36.8M cycles skipped by the scheduler as idle. That is ~55k instructions a frame, about half a millisecond of
  host time: the most a faster interpreter could save.
- With blocks the game thread took ~3 ms more per frame in three A/B pairs (24.6-27.0 against 29.1-31.9 ms
  per frame). Its profile showed the EE spinning in `snd_FlushSoundCommands` / `snd_SendIOPCommandNoWait`
  waiting for 989snd: the coarser IOP event timing made the replies come later in emulated time.
Reverted. The IOP is not where the frame goes any more (it was before the IOP time skip and the native voice
search). In the same profile (blocks off) the game thread spends ~29% of its time waiting for the vblank and
~23% spinning on an atomic load (waiting for VIF1/GS), with the IOP, SPU2 and the game's own code small: the
frame is bound by the VIF1/VU1 and GS threads, which is where the next speed work goes.

## Movie sound (2026-10-26)
Sound in the match was fine; at launch it was unbearable. A recording of the SPU2 output (`PS2X_SPU2_WAV`) put
the damage in the logo and intro movies: full-scale noise (clipping, nearly all its energy in sample-to-sample
jumps), then a steady buzz. The time stretcher was not involved: `PS2X_SPU2_STATS=1` (new) showed the emulation
producing 48000 frames a second, rate ~1.0, no drops. The music after the movies was clean.
- Movie audio does play in this player (the old note saying `isAudioOK` short-circuits is out of date):
  pcmCallback copies each PSS audio packet (`ff a0 00 00` sub-header, then 16-bit little-endian PCM in 512-byte
  L/R blocks, after a 40-byte `SShd`/`SSbd` header) into the audioDec ring (48 KiB at 0x683880), and
  audioDecSendToIOP moves it into 989snd's 24 KiB IOP buffer (0x856b0, two 12 KiB halves played by AutoDMA).
  `snd_GetTransStatus` reports the AutoDMA position correctly.
- The bytes reaching the SPU2 input matched the movie for the first five blocks, then sat at an odd offset in
  the 1024-byte L/R period: one packet (4073 bytes) had been dropped. With libmpeg the demuxer stops when a
  callback cannot take a packet; the HLE runs the callbacks later, ignores what they return, and the game's
  pcmCallback copied what fit and dropped the rest. The HLE also took the whole 320 KiB read buffer per call,
  decoding ~24 pictures and delivering ~1 s of audio at once, then nothing for as long (the buzz: the IOP
  looping its two halves).
- Fixes: `runtime/tmb_overrides.cpp` replaces pcmCallback with a host queue that feeds the ring through
  audioDecBeginPut/EndPut's bookkeeping as space frees (before every audioDecSendToIOP): nothing is lost, every
  AutoDMA block lands on the L/R period. The fork's sceMpegDemuxPssRing now takes a CD sector at a time up to
  the pictures-ahead limit, which a game can lower (`setMpegMaxPicturesAhead`; TMB: 6, default 128 for VP2).
  The launch recording is clean (no clipping or noise in 110 s), skips still work, the match is unchanged.

## Where the frame goes now; GPU renderer by default (2026-10-26)
- The timeline (`TMB_TRACE_TIMELINE`) was missing two things since the sc3 backport: the GS thread is now
  `GSFrontend`, and the vblanks (`B`) only came from the movie-time vblank handler. It now marks every vblank from
  the scheduler's tick.
- Quiet run, GPU renderer: 25.9 ms per frame on average, 79% of frames 18-23 ms, the rest 34-43 ms. From the
  vblank, VIF1 (the VU1 microcode) and the GS thread work in lockstep for 15-16 ms; the game thread is busy only
  ~9 ms and waits ~8 ms in `sceGsSyncPath` for them. A frame whose VIF1/GS work runs past ~19 ms misses the
  vblank and takes two. So VIF1/VU1 is the critical path; the GS thread mostly follows it (on the GPU path a
  third of its time is Vulkan submission; `PS2X_GS_PARALLEL_FLUSH=0` made no measurable difference).
- The GPU renderer is now TMB's default (`runtime.gsForPresentation().requestParallelRenderer()` in the
  override): 20-23 ms per frame against 24-27 with the CPU rasterizer. `PS2X_GS_RENDERER=cpu` keeps the CPU
  rasterizer; without a Vulkan device the GS falls back to it.
- VU1 in the match (6000 runs recorded with `PS2X_VU1_RECORD`, replayed exactly by `tmb_vu1_replay`): ~1.25M VU
  cycles a frame at 9.6 ns each; one 616k-cycle program (pc 0x1d40, a vertex loop) is a third of it. Inside the
  compiled blocks the time is the timing/flag bookkeeping around the math (`blockReady`, `advance`, `leave`, a
  reload of up to 23 VF registers per 12-pair loop block). `tmb_vu1_replay` now takes `PS2X_PROFILE`.
- Profile-guided optimization (`scripts/pgo.sh`, `TMB_PGO=GENERATE|USE` on the runtime, IOP and compiled
  microcode): the VU1 replay is ~12% faster, exact; in the game no measurable gain (25.0 against 25.3 ms per frame
  over three interleaved pairs, with another port's build running). Opt-in, kept for a quiet measurement.
- Next for 50 fps: the VU1 path itself (a cheaper block protocol for loops, or independent microprogram batches
  on two threads, see HANDOFF).

## VU1 compiled blocks made cheaper (2026-10-26)
Measured by line, not guessed: `-DTMB_VU1_AOT_LINES=ON` builds the microcode with line tables (a separate tree,
e.g. `build/prof`), and `tools/vu1_lineprof.py` maps a `PS2X_PROFILE` of `tmb_vu1_replay` to the jit helpers.
Before, on the replay thread: FMAC helpers ~23%, `leave` (end of block) 10%, the tool's own copies ~17%, xgkick
progress 4%, register reload 3%, `blockReady` 3%, `commitFlags` 4%, MinGW's `fegetround`/`fesetround` 3%.

Fork changes, all exact on the 6000 recorded runs (`build/vu1_match.trace`):
- `leave`: `markPairWritesAt` on every pair is folded at compile time into one store per written lane (last writer
  per lane, maxima over the writes). 10% -> 3.5% of the thread.
- `VU1Interpreter::run` switches MXCSR and the x87 control word to round toward zero directly, only when needed.
- Operand normalization as a clamp of the magnitude bits; `commitFlags` checks inline before calling out.
- One-pair blocks (a lone pair between a branch's delay slot and a branch target) are recompiled too.
- Tried and dropped: one branch instead of two in `normalizeUnobserved` (its 4% was sample skid, no gain).

Replay: 33.9 -> 26.7 ms per pass. In the game (three interleaved 200 s pairs into the match): VIF1/VU1 worker
15.0 -> 12.8 ms per frame, W+B 23.8 -> 22.1 ms (noisier). After a fork update like this one, regenerate the
microcode: `scripts/recomp.sh`, or only `build/tools/ps2xRecomp/ps2_vu_aot game/SCES_503.60 <dir>/vu1` and copy
the changed files into `generated/vu1/`.

What is left in the blocks is mostly the FMAC itself (~30% now: the PS2 addition `vuAdd4`, operand normalization,
the unobserved-flag range check) and fixed per-block costs (`blockReady`, register reload, dispatch in `run`).
The fork also gained VU0 macro FMAC flags in the recompiler (another port's fix): TMB's EE code picks them up only
when regenerated, not yet measured here.

## FMAC cost in the compiled blocks (2026-10-26)
FMAC helpers were ~28% of the replay thread: `vuAdd4` 7%, the unobserved-flag range check (`normalizeUnobserved`)
~9%, operand normalization 4%, the result's trip through memory ~2%. Findings, all exact on the 6000 recorded runs:
- Every FMAC result went through the stack: stored before the range check, reloaded after it, because the rare
  fix-up took a pointer. The result now stays in a register; only the fix-up (`Vu1Jit::fixUnobserved`) goes
  through memory. -3 to -4%.
- That fix-up (`fmacFlagsWith`'s exact double path) was inlined into every FMAC: out of line (`PS2X_VU_COLD`) the
  hot loop blocks are 15% smaller, -4%. Code size matters more than instruction latency here: the blocks are
  ~1 KB of machine code per VU pair.
- `vuAdd4` builds each operand's mask directly (`~0 << gap`, sign only 24 or more steps below, all ones when an
  exponent is 0): fewer instructions and a shorter chain. Checked against the scalar `vuAdd` on 80M random and
  edge-case lanes (zeros, denormals, exponent 255, near and far gaps), 0 differences. -1 to -5%.
- Tried and dropped, no gain: one branch instead of two in the range check (again), `fmacFlagsWith`'s out-of-range
  part out of line (slower: few hot blocks read flags), XGKICK copying single qwords on its fast path with a mask
  instead of a division.
- Replay A/B now uses min and median of 10 interleaved runs per build (other programs on this machine move single
  runs by +-1 ms); `tmb_vu1_replay` also prints the thread's own CPU cycles per pass.

Together: replay -4 to -5% against the previous fork build (min 26.9 -> 25.8 ms per pass, median 27.4 -> 26.1).
In the game the difference stays inside the run-to-run noise (three interleaved pairs, one with another port's
build running): no regression, no clear gain to quote.

Frame rate counter: the fork draws the game's fps at the top right (`PS2X_SHOW_FPS=1`, F3 toggles), from frames a
game layer reports (`PS2Runtime::noteGameFrame`, which TMB calls at the end of each frame's real-time processing)
or else display buffer flips. TMB never moves DISPFB between vblanks, hence its own report. On by default here.
In the match: 50 fps in quiet stretches, mid-20s when the scene gets busy.

## Busy scenes: what limits the frame (2026-10-26)
Two 150 s timelines of the match (`TMB_TRACE_TIMELINE_START=300 TMB_TRACE_TIMELINE_SECONDS=150`) and profiles of only
the slow frames and only the fast ones (`PS2X_PROFILE_GATED=1`, TMB opens the gate while frames take two vblanks;
`TMB_PROFILE_FAST_FRAMES=1` inverts it). The match runs at 45-50 fps most of the time and drops to 25-35 fps in
stretches of 20-60 s (more cars and effects on screen).
- No thread is saturated in the slow stretches: VIF1/VU1 62-74% busy, GS thread 58-67%, game thread ~33%. Where
  VIF1 reaches 80% elsewhere, the game still holds 46-48 fps.
- In slow frames VIF1 waits for work 47% of the time, the GS thread 45%, and the game thread waits for VIF1 25%
  and for the next vblank 40% (the frame missed it).
- The frame's work is serialized by two `sceGsSyncPath` calls inside it: `particleDraw` (ra 0x1eac28) waits for
  VIF1 to finish the whole 3D scene `hier` kicked at the vblank (8.5 ms per slow frame, 1.4 ms in quiet play), then
  `Hud::Update` (ra 0x13de5c) waits for the particles (5.0 ms, 0.07 in quiet play). The game, VIF1 and the GS take
  turns instead of overlapping, and the chain runs past the next vblank.
- vif1 and GS look busy in lockstep in the timeline because the GS thread spins (2000 pauses) waiting for packets;
  its real work is mostly Vulkan submission (`submit_batches` ~22% of its samples).
- `TMB_TRACE_FRAMES` makes every 100th frame slow (it enumerates the process's threads): leave it off when
  counting slow frames.
- Tried and dropped: `sceGsSyncPath` returning at once for those two calls (pending transfers flushed, the
  end-of-frame sync kept). In-run A/B over the match, the two modes alternating every 250 vblanks: 38.0 fps with
  the waits skipped, 38.1 with them, 31% slow frames either way (~5200 frames each). The waits did not cause the
  slow frames, they only showed where the game ends up waiting for VIF1: the VU1 work kicked at the vblank (the
  scene, then the particles) still finishes after the next one. In busy scenes the frame is bounded by VIF1/VU1's
  work from the vblank, not by the game taking turns with it.
- GS lag retested (bounded to one presented frame), in-run A/B (`TMB_GS_LAG=1 TMB_GS_LAG_AB=1`, every 100
  vblanks, which now prints per-mode stats and the input latency): 47.3 fps on, 47.4 off over the match; in the busy
  intervals 42.6 on, 43.4 off (17% and 15% slow frames). Latency, from the TheGame::Update whose state a frame shows
  to the first vblank after VIF1 drew it: 2.06 vblanks in both modes (5.6% of frames over 2). No gain, no added
  latency measured: it stays off.
- With the lag on, slow frames (gated profile) show the game thread waiting for VIF1 much less (9% of its samples
  against 25%) and for events more (43% against 40%); VIF1 and the GS thread are still idle about half the time.
  None of the three threads is what the busy frames wait for: the next thing to find is what the EE's
  `waitForEvent` is blocked on in them (a guest semaphore, the IOP, the vblank interrupt's timing).
- The intermittent sound hang (`snd_SendIOPCommandNoWait: BUFFER 0 FULL`, an IOP call that never returns) came
  back three times today: at a level load, late in a match, and in a run with nothing changed but tracing. It is
  not caused by the sync experiments.
- What the EE waits on (`TMB_TRACE_IDLE=1`, the fork's `EeScheduler::setIdleObserver`: each idle wait with the
  blocked guest threads, what they wait on and from where, and what woke the EE): in slow frames all of its idle
  time, 11.8-14 ms per frame, is thread 1 waiting for the vblank at the end of the frame (ra 0x1f7ad8, by
  timerEndOfRealTimeProcessing), woken by VBlankStart. No semaphore, no IOP/SIF wait, no other guest thread: a
  slow frame is one whose processing ended just after a vblank, and the game waits for the next.
- What makes the processing end late, frame by frame (150 s timeline): VIF1's CPU per frame. Under 16 ms, 0.2-2.4%
  of frames are slow; 16-18 ms 16%, 18-20 ms 29%, 20-22 ms 58%, over 24 ms 92%. Busy scenes put 18-24 ms of
  VIF1 work (nearly all VU1 microcode) in a frame. The threshold is 16 ms, not 20: the work is kicked about 1 ms
  after the vblank, and the game still draws the HUD and ends its frame after VIF1 is done. 50 fps in busy scenes
  needs VU1 25-30% cheaper there, or its work split across threads.
- This machine is shared: the same build measured ~45, 38 and 25 fps in three runs of the match within an hour,
  depending on what else ran (other ports' builds; a timeline counts a thread that gets no CPU as idle). Only
  in-run A/B comparisons hold.

## Open
- A small striped rectangle under "BLACK" on the title logo.
- Speed (VU1 on its thread is the limit), sound output, controls in the match, memory card.
- `timer1Handler` pacing (T0 COUNT read + up to 16 `sceGsSyncV` catch-ups per frame) to be checked against the
  real frame rate once there is motion on screen.
- USBD/LGDEV: a proper fix would be OHCI emulation in `ps2xIOP`.
