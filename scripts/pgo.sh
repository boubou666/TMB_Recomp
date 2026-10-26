#!/usr/bin/env bash
# Profile-guided build of the runner (GCC). After scripts/build_runtime.sh: rebuilds the runtime, the IOP and the
# compiled VU1 microcode instrumented, plays into the first match with scripted menu presses for TMB_PGO_SECONDS
# (default 200) while they record a profile, then rebuilds them with it. Experimental: the VU1 microcode replay is
# ~12% faster, frame times in the game showed no measurable gain so far (RECOMP_NOTES).
# scripts/build_runtime.sh afterwards keeps the profile (TMB_PGO stays USE); -DTMB_PGO=OFF goes back to a plain build.
source "$(dirname "$0")/common.sh"
BIN="$GAME_BUILD/PS2Recomp/ps2xRuntime/ps2EntryRunner"
TRAIN_SECONDS="${TMB_PGO_SECONDS:-200}"

rm -rf "$GAME_BUILD/pgo-profile"
cmake -S "$ROOT" -B "$GAME_BUILD" -DTMB_PGO=GENERATE
cmake --build "$GAME_BUILD" --target ps2EntryRunner -j "$JOBS"

# Language, then the title, ONE PLAYER, the vehicle and the level: the match starts about 150 s in.
S="25:CROSS/300"
for t in $(seq 60 6 140); do S="$S,$t:CROSS/250"; done
echo "Training for ${TRAIN_SECONDS} s (the game window opens)"
TMB_PGO_SECONDS="$TRAIN_SECONDS" PS2X_PAD_SCRIPT="$S" "$BIN" "$ELF" > "$GAME_BUILD/pgo-training.log" 2>&1 || true
[ -d "$GAME_BUILD/pgo-profile" ] || { echo "no profile was written (see $GAME_BUILD/pgo-training.log)"; exit 1; }

cmake -S "$ROOT" -B "$GAME_BUILD" -DTMB_PGO=USE
cmake --build "$GAME_BUILD" --target ps2EntryRunner -j "$JOBS"
echo "Profile-guided runner built: $BIN"
