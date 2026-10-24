#!/usr/bin/env bash
# scripts/bots.sh <name> [secs]: menus into the first match, the player idle at the start, every vehicle's position
# traced to build/pos_<name>.txt, then tools/falls.py counts the vehicles falling under the arena. Extra env (e.g.
# TMB_SUBSTEPS=1) passes through.
source "$(dirname "$0")/common.sh"
name=$1; secs=${2:-600}
cd "$ROOT/game" || exit 1
S="25:CROSS/300"; for t in $(seq 60 6 140); do S="$S,$t:CROSS/250"; done
PS2X_PAD_SCRIPT="$S" TMB_TRACE_POS="$ROOT/build/pos_$name.txt" timeout "$secs" "$ROOT/build/game/PS2Recomp/ps2xRuntime/ps2EntryRunner" \
    "$PWD/SCES_503.60" > "$ROOT/build/run_$name.log" 2>&1 || true
python3 "$ROOT/tools/falls.py" "$ROOT/build/pos_$name.txt"
