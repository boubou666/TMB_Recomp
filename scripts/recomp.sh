#!/usr/bin/env bash
# Runs ps2_recomp with the region's committed config and ps2_vu_aot on the ELF's VU1 microcode; output lands in
# generated_us/ or generated/ (VU1 microcode in its vu1/), with tmb_region.h (tools/tmb_region_map.py: the addresses
# the runtime's hooks use, for this region's ELF).
# Output goes through build/recomp-staging first and only files whose content changed are
# copied over, so ninja rebuilds just the affected unity batches instead of the whole game.
source "$(dirname "$0")/common.sh"
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh first"; exit 1; }
[ -x "$TOOLS_BUILD/ps2xRecomp/ps2_recomp" ] && [ -x "$TOOLS_BUILD/ps2xRecomp/ps2_vu_aot" ] || "$ROOT/scripts/build_tools.sh"
cd "$ROOT"

STAGING="$ROOT/build/recomp-staging-$TMB_REGION"
rm -rf "$STAGING"
mkdir -p "$STAGING" "$GENERATED"
sed "s|^output = .*|output = \"build/recomp-staging-$TMB_REGION/\"|" "$CONFIG" > "$ROOT/build/recomp-staging-$TMB_REGION.toml"
"$TOOLS_BUILD/ps2xRecomp/ps2_recomp" "build/recomp-staging-$TMB_REGION.toml"
# VU1 microcode on the fast path (PCSX2-like floats and block timing; RECOMP_NOTES "Fast VU1 path"): 41-43 against
# 45 fps in play, same pictures. TMB_VU1_EXACT=1 generates the exact model instead (tmb_vu1_replay checks).
if [ -n "${TMB_VU1_EXACT:-}" ]; then
    "$TOOLS_BUILD/ps2xRecomp/ps2_vu_aot" "$ELF" "$STAGING/vu1"
else
    PS2X_VU_AOT_FAST=1 "$TOOLS_BUILD/ps2xRecomp/ps2_vu_aot" "$ELF" "$STAGING/vu1"
fi

python tools/tmb_region_map.py "$PAL_ELF" "$ELF" runtime > "$STAGING/tmb_region.h"
# The textures as a native pack (tools/tmb_texture_pack.py): converted once from the disc's files, nothing read from
# the game's texture records while it runs.
python tools/tmb_texture_pack.py "$GAME_DIR" "$ELF" "$STAGING/tmb_textures.pack"

# -c compares content, not mtimes; --delete drops functions that are no longer generated.
changed=$(rsync -rc --delete --out-format='%n' "$STAGING/" "$GENERATED/" | grep -vc '/$' || true)
echo "Generated $(ls "$GENERATED" | wc -l) files in $GENERATED ($changed changed)"
