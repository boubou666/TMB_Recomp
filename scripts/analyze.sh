#!/usr/bin/env bash
# Regenerates config/tmb_sces50360.toml from the ELF with ps2_analyzer.
# WARNING: this overwrites hand edits in the TOML; diff before committing.
source "$(dirname "$0")/common.sh"
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh first"; exit 1; }
cd "$ROOT"
"$TOOLS_BUILD/ps2xAnalyzer/ps2_analyzer" "game/SCES_503.60" "config/tmb_sces50360.toml"
sed -i 's|^output = .*|output = "generated/"|' "$CONFIG"
