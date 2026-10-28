#!/bin/bash
# Recompiles TMB's IOP sound modules (989SND.IRX, LIBSD.IRX) to native C++ in generated/iop/ (run with
# PS2X_IOP_NATIVE=1). The recompiler works from each module as the IOP loaded it (relocated at its base), so this
# first boots the game long enough for it to load its modules, with PS2X_IOP_DUMP_MODULES, then runs the fork's
# tools/iop_recomp.py on the dumps. Like generated/, the output is derived from the game: never committed.
#   scripts/iop_recomp.sh [seconds of boot, default 40]
set -euo pipefail
cd "$(dirname "$0")/.."
SECONDS_TO_BOOT=${1:-40}
EXE=build/game/PS2Recomp/ps2xRuntime/ps2EntryRunner.exe
DUMPS=build/iopmods
rm -rf "$DUMPS" && mkdir -p "$DUMPS" generated/iop
PS2X_IOP_DUMP_MODULES="$(pwd)/$DUMPS" timeout "$SECONDS_TO_BOOT" "$EXE" "$(pwd)/game/SCES_503.60" > build/iop_dump.log 2>&1 || true
for module in 989SND LIBSD; do
    json=$(ls "$DUMPS" | grep -i "^$module.IRX.*\.json$" | head -1)
    [ -n "$json" ] || { echo "no dump of $module (see build/iop_dump.log)"; exit 1; }
    python PS2Recomp/tools/iop_recomp.py "$DUMPS/$json" --out generated/iop
done
