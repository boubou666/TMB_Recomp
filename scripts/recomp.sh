#!/usr/bin/env bash
# Runs ps2_recomp with the committed config; output lands in generated/.
source "$(dirname "$0")/common.sh"
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh first"; exit 1; }
[ -x "$TOOLS_BUILD/ps2xRecomp/ps2_recomp" ] || "$ROOT/scripts/build_tools.sh"
cd "$ROOT"
rm -rf "$GENERATED"
mkdir -p "$GENERATED"
"$TOOLS_BUILD/ps2xRecomp/ps2_recomp" "recomp/tmb_sces50360.toml"
echo "Generated $(ls "$GENERATED" | wc -l) files in $GENERATED"
