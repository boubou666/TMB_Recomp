# Sourced by the other scripts. Resolves the repo root and tool paths.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="$HOME/.local/share/mise/shims:$PATH"

# TMB_REGION=us (the default: the USA release, SCUS-97101, NTSC) or pal (the European one, SCES-50360): each has its
# own disc folder, recompiler config, generated code and build.
TMB_REGION="${TMB_REGION:-us}"
case "$TMB_REGION" in
    us)
        GAME_DIR="$ROOT/game_us"
        ELF="$GAME_DIR/SCUS_971.01"
        CONFIG="$ROOT/recomp/tmb_scus97101.toml"
        GENERATED="$ROOT/generated_us"
        GAME_BUILD="$ROOT/build/game_us"
        ;;
    pal)
        GAME_DIR="$ROOT/game"
        ELF="$GAME_DIR/SCES_503.60"
        CONFIG="$ROOT/recomp/tmb_sces50360.toml"
        GENERATED="$ROOT/generated"
        GAME_BUILD="$ROOT/build/game"
        ;;
    *)
        echo "TMB_REGION must be us or pal (got $TMB_REGION)" >&2
        exit 1
        ;;
esac
# The PAL executable, the reference the runtime's hooks were written against (tools/tmb_region_map.py maps them).
PAL_ELF="$ROOT/game/SCES_503.60"
TOOLS_BUILD="$ROOT/build/tools"
JOBS="${JOBS:-$(nproc)}"
