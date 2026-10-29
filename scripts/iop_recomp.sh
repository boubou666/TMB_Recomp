#!/usr/bin/env bash
# Recompiles every IOP module TMB loads (989SND, LIBSD, PADMAN, SIO2MAN, MTAPMAN, MCMAN, MCSERV, USBD, LGDEV) to native
# C++ in the region's generated folder, iop/
# (run with PS2X_IOP_NATIVE=1). The recompiler works from each module as the IOP loaded it (relocated at its base), so
# this first boots the game long enough for it to load its modules, with PS2X_IOP_DUMP_MODULES, then runs the fork's
# tools/iop_recomp.py on the dumps. Like the rest of the generated code, the output is derived from the game: never
# committed.
#   [TMB_REGION=us|pal] scripts/iop_recomp.sh [seconds of boot, default 40]
source "$(dirname "$0")/common.sh"
cd "$ROOT"
SECONDS_TO_BOOT=${1:-40}
EXE="$GAME_BUILD/PS2Recomp/ps2xRuntime/ps2EntryRunner"
[ -x "$EXE" ] || EXE="$EXE.exe"
DUMPS="build/iopmods-$TMB_REGION"
rm -rf "$DUMPS" && mkdir -p "$DUMPS" "$GENERATED/iop"
PS2X_IOP_DUMP_MODULES="$ROOT/$DUMPS" timeout "$SECONDS_TO_BOOT" "$EXE" "$ELF" > "build/iop_dump-$TMB_REGION.log" 2>&1 || true
ls "$DUMPS" | grep -qi '^989SND.IRX.*\.json$' || { echo "no dump of 989SND (see build/iop_dump-$TMB_REGION.log)"; exit 1; }
for json in "$DUMPS"/*.json; do
    python PS2Recomp/tools/iop_recomp.py "$json" --out "$GENERATED/iop" || exit 1
done
