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

## Open
- A band of corrupted texels at the bottom of the title screen (y 370-450) and two small artifacts near the logo.
- Menu navigation, car/level select, first match.
- `timer1Handler` pacing (T0 COUNT read + up to 16 `sceGsSyncV` catch-ups per frame) to be checked against the
  real frame rate once there is motion on screen.
- USBD/LGDEV: a proper fix would be OHCI emulation in `ps2xIOP`.
