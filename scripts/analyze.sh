#!/usr/bin/env bash
# Regenerates the region's recompiler config (recomp/tmb_scus97101.toml or recomp/tmb_sces50360.toml) from its ELF with
# ps2_analyzer, then adds the hand-written part shared by both regions (recomp/tmb_manual.toml, by symbol name).
# WARNING: this overwrites hand edits in the TOML outside the shared part; diff before committing.
source "$(dirname "$0")/common.sh"
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh first"; exit 1; }
cd "$ROOT"
"$TOOLS_BUILD/ps2xAnalyzer/ps2_analyzer" "${ELF#$ROOT/}" "${CONFIG#$ROOT/}"
sed -i "s|^output = .*|output = \"${GENERATED#$ROOT/}/\"|" "$CONFIG"
python tools/tmb_manual_toml.py "$ELF" recomp/tmb_manual.toml "$CONFIG"
