#!/usr/bin/env bash
# Launches the game as a native PC port: the 3D drawn by the native renderer from the game's own data (TMB_NATIVE),
# the sound driver's IOP modules as recompiled code (PS2X_IOP_NATIVE, when scripts/iop_recomp.sh has generated them)
# and the sound and IOP on the PC's clock. Extra args are passed to ps2EntryRunner.
source "$(dirname "$0")/common.sh"
BIN="$GAME_BUILD/PS2Recomp/ps2xRuntime/ps2EntryRunner"
[ -x "$BIN" ] || { echo "missing $BIN, run scripts/build_runtime.sh first"; exit 1; }
export TMB_NATIVE=1 PS2X_SPU2_WALLCLOCK=1 PS2X_IOP_WALLCLOCK=1 PS2X_GS_RENDERER="${PS2X_GS_RENDERER:-native}"
if compgen -G "$GENERATED/iop/*.cpp" > /dev/null; then
    export PS2X_IOP_NATIVE=1
else
    echo "no ${GENERATED#$ROOT/}/iop/: the sound modules run interpreted (scripts/iop_recomp.sh recompiles them)"
fi
exec "$BIN" "$ELF" "$@"
