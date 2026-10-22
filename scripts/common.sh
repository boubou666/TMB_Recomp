# Sourced by the other scripts. Resolves the repo root and tool paths.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="$HOME/.local/share/mise/shims:$PATH"

GAME_DIR="$ROOT/game"
ELF="$GAME_DIR/SCES_503.60"
CONFIG="$ROOT/recomp/tmb_sces50360.toml"
GENERATED="$ROOT/generated"
TOOLS_BUILD="$ROOT/build/tools"
GAME_BUILD="$ROOT/build/game"
JOBS="${JOBS:-$(nproc)}"
