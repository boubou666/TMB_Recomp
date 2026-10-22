#!/usr/bin/env bash
# Launches the recompiled game. Extra args are passed to ps2EntryRunner.
source "$(dirname "$0")/common.sh"
BIN="$GAME_BUILD/PS2Recomp/ps2xRuntime/ps2EntryRunner"
[ -x "$BIN" ] || { echo "missing $BIN, run scripts/build_runtime.sh first"; exit 1; }
exec "$BIN" "$ELF" "$@"
