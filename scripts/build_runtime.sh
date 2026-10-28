#!/usr/bin/env bash
# Builds ps2EntryRunner with the region's generated code linked in -> build/game_us (or build/game for PAL)/PS2Recomp/
# ps2xRuntime/ps2EntryRunner
# Usage: scripts/build_runtime.sh [Release|RelWithDebInfo|Debug]
source "$(dirname "$0")/common.sh"
BUILD_TYPE="${1:-RelWithDebInfo}"
[ -f "$GENERATED/register_functions.cpp" ] || { echo "no generated code, run scripts/recomp.sh first"; exit 1; }
cmake -S "$ROOT" -B "$GAME_BUILD" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DTMB_GENERATED_DIR="$GENERATED"
cmake --build "$GAME_BUILD" --target ps2EntryRunner -j "$JOBS"
# Windows (MSYS2): the DLLs next to the runner, so it starts outside the MSYS2 shell too.
case "$(uname -s)" in MINGW* | MSYS*) "$ROOT/scripts/stage_dlls.sh" ;; esac
