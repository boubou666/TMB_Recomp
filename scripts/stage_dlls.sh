#!/usr/bin/env bash
# Windows (MSYS2 UCRT64): copies the DLLs ps2EntryRunner.exe needs (GCC runtime, FFmpeg and their dependencies)
# from the MSYS2 prefix next to it, so it also runs outside the MSYS2 shell (Explorer, PowerShell).
source "$(dirname "$0")/common.sh"
BIN_DIR="$GAME_BUILD/PS2Recomp/ps2xRuntime"
EXE="$BIN_DIR/ps2EntryRunner.exe"
[ -f "$EXE" ] || { echo "missing $EXE, run scripts/build_runtime.sh first"; exit 1; }
PREFIX="${MINGW_PREFIX:-/ucrt64}"
# ldd lists every DLL the loader resolves; keep the ones from the MSYS2 prefix (not Windows' own). DLLs staged by
# an earlier run resolve next to the exe instead: they are refreshed from the prefix too.
count=0
for name in $(ldd "$EXE" | awk '$3 ~ /\.dll$/ {print $1}' | sort -u); do
    if [ -f "$PREFIX/bin/$name" ]; then
        cp -u "$PREFIX/bin/$name" "$BIN_DIR/"
        count=$((count + 1))
    fi
done
echo "$count DLLs from $PREFIX/bin staged in $BIN_DIR"
