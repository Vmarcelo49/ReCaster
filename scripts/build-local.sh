#!/usr/bin/env bash
# scripts/build-local.sh
#
# Build caster.exe + hook.dll in this sandbox using the user-space MinGW-w64
# toolchain installed under /home/z/mingw-prefix/.
#
# This script exists because the sandbox doesn't have `sudo` to install
# mingw-w64 via apt. In a normal Debian/Ubuntu dev box, you'd just run
# `./scripts/setup-deps.sh` (which calls apt-get install) and then
# `./scripts/build.sh`.
#
# Usage:
#   ./scripts/build-local.sh           # configure + build
#   ./scripts/build-local.sh rebuild   # build only (skip configure)

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MINGW_PREFIX="${MINGW_PREFIX:-/home/z/mingw-prefix}"
CMAKE_BIN="${CMAKE_BIN:-/home/z/.venv/bin/cmake}"

# Put MinGW binaries + pip-installed cmake on PATH
export PATH="${MINGW_PREFIX}/usr/bin:$(dirname "${CMAKE_BIN}"):${PATH}"

# Verify tools are available
for tool in i686-w64-mingw32-g++ i686-w64-mingw32-gcc cmake; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: $tool not found on PATH"
        echo "       MINGW_PREFIX=${MINGW_PREFIX}"
        echo "       CMAKE_BIN=${CMAKE_BIN}"
        exit 1
    fi
done

BUILD_DIR="${ROOT_DIR}/build"
TOOLCHAIN_FILE="${ROOT_DIR}/cmake/toolchain-mingw32.cmake"

# Default args
TARGET_PROCESS="${CASTER_TARGET_PROCESS:-MBAA.exe}"
BUILD_TYPE="${CASTER_BUILD_TYPE:-Release}"
JOBS="${CASTER_BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}"

echo "==> caster: build-local.sh"
echo "    Source         : $ROOT_DIR"
echo "    Build dir      : $BUILD_DIR"
echo "    Toolchain      : $TOOLCHAIN_FILE"
echo "    Build type     : $BUILD_TYPE"
echo "    Target process : $TARGET_PROCESS"
echo "    Parallel jobs  : $JOBS"
echo "    MinGW prefix   : $MINGW_PREFIX"
echo

# 1. Configure (skip if `rebuild` arg given and cache exists)
if [[ "${1:-}" != "rebuild" || ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    echo "==> Configuring..."
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCASTER_TARGET_PROCESS="$TARGET_PROCESS"
fi

# 2. Build
echo
echo "==> Building..."
cmake --build "$BUILD_DIR" --parallel "$JOBS"

# 3. Report
echo
echo "==> Build artifacts:"
ls -la "${BUILD_DIR}/bin/" | sed 's/^/    /'
echo
file "${BUILD_DIR}/bin/caster.exe" | sed 's/^/    /'
file "${BUILD_DIR}/bin/hook.dll"  | sed 's/^/    /'

echo
echo "==> Done. Copy caster.exe + hook.dll to a Windows box to run."
