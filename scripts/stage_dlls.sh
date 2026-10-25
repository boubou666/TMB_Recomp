#!/usr/bin/env bash
# Windows (MSYS2 UCRT64): copies the DLLs ps2EntryRunner.exe needs (GCC runtime, FFmpeg and their dependencies)
# from the MSYS2 prefix next to it, so it also runs outside the MSYS2 shell (Explorer, PowerShell).
source "$(dirname "$0")/common.sh"
BIN_DIR="$GAME_BUILD/PS2Recomp/ps2xRuntime"
EXE="$BIN_DIR/ps2EntryRunner.exe"
[ -f "$EXE" ] || { echo "missing $EXE, run scripts/build_runtime.sh first"; exit 1; }
PREFIX="${MINGW_PREFIX:-/ucrt64}"
# ldd lists every DLL the loader resolves; keep the ones from the MSYS2 prefix (not Windows' own).
count=0
for dll in $(ldd "$EXE" | awk '{print $3}' | grep -i "^$PREFIX/bin/.*\.dll$" | sort -u); do
    cp -u "$dll" "$BIN_DIR/"
    count=$((count + 1))
done
echo "Staged $count DLLs from $PREFIX/bin in $BIN_DIR"
