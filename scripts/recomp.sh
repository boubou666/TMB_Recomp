#!/usr/bin/env bash
# Runs ps2_recomp with the committed config; output lands in generated/.
# Output goes through build/recomp-staging first and only files whose content changed are
# copied over, so ninja rebuilds just the affected unity batches instead of the whole game.
source "$(dirname "$0")/common.sh"
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh first"; exit 1; }
[ -x "$TOOLS_BUILD/ps2xRecomp/ps2_recomp" ] || "$ROOT/scripts/build_tools.sh"
cd "$ROOT"

STAGING="$ROOT/build/recomp-staging"
rm -rf "$STAGING"
mkdir -p "$STAGING" "$GENERATED"
sed 's|^output = .*|output = "build/recomp-staging/"|' "$CONFIG" > "$ROOT/build/recomp-staging.toml"
"$TOOLS_BUILD/ps2xRecomp/ps2_recomp" "build/recomp-staging.toml"

# -c compares content, not mtimes; --delete drops functions that are no longer generated.
changed=$(rsync -rc --delete --out-format='%n' "$STAGING/" "$GENERATED/" | grep -vc '/$' || true)
echo "Generated $(ls "$GENERATED" | wc -l) files in $GENERATED ($changed changed)"
