#!/usr/bin/env bash
# The fork's port regression run for TMB: scripts/regress.sh <build-dir> <out-dir>
# Boot movies, menus, Challenge mode into a Skyscrapers match, driving and firing for about 3 minutes (380 s in all),
# under the box's bench lock (PS2Recomp/tools/bench_lock.sh). Writes <out-dir>/run.log, summary.txt (tools/regress_summary.py)
# and last_frame.png. Exits 0 when the run itself completed; pass or fail is in the summary.
#   REGRESS_SECONDS  run length (default 380)
#   REGRESS_ARENA    RIGHT presses after Suburbs in the arena list (default 4, Skyscrapers)
source "$(dirname "$0")/common.sh"
BUILD="$(cd "${1:?usage: regress.sh <build-dir> <out-dir>}" && pwd)"
OUT="${2:?usage: regress.sh <build-dir> <out-dir>}"
END="${REGRESS_SECONDS:-380}"
BIN="$BUILD/PS2Recomp/ps2xRuntime/ps2EntryRunner"
[ -x "$BIN" ] || [ -x "$BIN.exe" ] || { echo "missing $BIN"; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
PS2RECOMP="$(git -C "$ROOT/PS2Recomp" rev-parse --short=8 HEAD)"

source "$ROOT/PS2Recomp/tools/bench_lock.sh"
trap bench_lock_release EXIT
bench_lock_take tmb-recomp $((END / 60 + 2)) "TMB regression run, PS2Recomp $PS2RECOMP"
START=$(date +%s)
code=0
PS2X_CAPTURE_DIR="$OUT" PS2X_CAPTURE_INTERVAL=4 TMB_TRACE_FRAMES=1 \
  PS2X_PAD_SCRIPT="$(python3 "$ROOT/tools/arena_script.py" "${REGRESS_ARENA:-4}" "$END")" \
  timeout "$END" "$BIN" "$ELF" > "$OUT/run.log" 2>&1 || code=$?
DURATION=$(($(date +%s) - START))
bench_lock_release
trap - EXIT
python3 "$ROOT/tools/regress_summary.py" "$OUT" "$code" "$DURATION" "$PS2RECOMP"
