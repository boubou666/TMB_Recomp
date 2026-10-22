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

Game-specific: none yet.

## Open
- First boot not attempted yet (runner build in progress at the time of writing).
