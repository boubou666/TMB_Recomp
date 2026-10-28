#!/usr/bin/env bash
# Usage: [TMB_REGION=us|pal] scripts/extract_iso.sh <path-to-the-disc.iso>
# Extracts the disc into game_us/ (USA, SCUS-97101, the default) or game/ (PAL, SCES-50360); the runtime mounts
# cdrom0: on the ELF's directory.
source "$(dirname "$0")/common.sh"
ISO="${1:-}"
[ -f "$ISO" ] || { echo "usage: $0 <iso>"; exit 1; }
mkdir -p "$GAME_DIR"
bsdtar -xf "$ISO" -C "$GAME_DIR"
chmod -R u+w "$GAME_DIR"
[ -f "$ELF" ] || { echo "ELF not found after extraction: $ELF (is this the $TMB_REGION disc?)"; exit 1; }
echo "Extracted to $GAME_DIR"
