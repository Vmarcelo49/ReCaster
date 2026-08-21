#!/usr/bin/env bash
# deploy — build caster + hook.dll and drop the binaries in the game folder.
#
# Usage:
#   ./scripts/deploy.sh            # incremental build (no reconfigure) + deploy
#   ./scripts/deploy.sh full       # reconfigure from scratch + build + deploy
#   ./scripts/deploy.sh only       # skip build; deploy existing binaries
#
# Env:
#   RECASTER_GAME_DIR   game folder (default: <repo>/MBAACC)
#   FORCE_DEPLOY=1      deploy even if a caster/MBAA instance is running
#
# Output:
#   <game dir>/caster.exe  <game dir>/hook.dll
#   <game dir>/d3d9.dll    (DXVK; only copied if not already present)

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME_DIR="${RECASTER_GAME_DIR:-${ROOT_DIR}/MBAACC}"
BIN_DIR="${ROOT_DIR}/build/bin"

mode="${1:-quick}"

# --- sanity checks -----------------------------------------------------------
if [ ! -d "$GAME_DIR" ]; then
    echo "deploy: game folder not found: $GAME_DIR" >&2
    echo "        set RECASTER_GAME_DIR or create it" >&2
    exit 1
fi

# Overwriting a running exe/dll under Wine misbehaves — refuse politely.
# Wine lists exe paths Windows-style (Z:\...\MBAACC\caster.exe), so match
# both path separators.
if [ "${FORCE_DEPLOY:-0}" != "1" ] \
   && pgrep -f "$(basename "$GAME_DIR")[\\\\/](caster|MBAA)\\.exe" >/dev/null 2>&1; then
    echo "deploy: a caster.exe / MBAA.exe instance is running — close it first." >&2
    echo "        (or FORCE_DEPLOY=1 to override)" >&2
    exit 1
fi

# --- build -------------------------------------------------------------------
case "$mode" in
    quick) "$ROOT_DIR/scripts/build.sh" rebuild ;;
    full)  "$ROOT_DIR/scripts/build.sh" ;;
    only)  echo "deploy: skipping build (--only)" ;;
    *)     echo "usage: $0 [quick|full|only]" >&2; exit 2 ;;
esac

for f in caster.exe hook.dll; do
    if [ ! -f "${BIN_DIR}/${f}" ]; then
        echo "deploy: build output missing: ${BIN_DIR}/${f}" >&2
        exit 1
    fi
done

# --- deploy ------------------------------------------------------------------
cp -v "${BIN_DIR}/caster.exe" "${BIN_DIR}/hook.dll" "$GAME_DIR/"

if [ ! -f "${GAME_DIR}/d3d9.dll" ] && [ -f "${BIN_DIR}/d3d9.dll" ]; then
    cp -v "${BIN_DIR}/d3d9.dll" "$GAME_DIR/"
    echo "deploy: d3d9.dll (DXVK) added"
fi

echo "deploy: done -> ${GAME_DIR}"
