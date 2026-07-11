#!/usr/bin/env bash
# Build caster.exe + hook.dll, strip debug info, zip to release/.
#
# Usage:
#   ./scripts/build.sh              # configure + build + strip + zip
#   ./scripts/build.sh rebuild      # skip configure, build only
#
# Output:
#   build/bin/caster.exe
#   build/bin/hook.dll
#   release/caster.zip

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
BIN_DIR="${BUILD_DIR}/bin"
RELEASE_DIR="${ROOT_DIR}/release"
TOOLCHAIN_FILE="${ROOT_DIR}/cmake/toolchain-mingw32.cmake"

BUILD_TYPE="${CASTER_BUILD_TYPE:-Release}"
JOBS="${CASTER_BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}"

# 1. Configure (skip if "rebuild" arg given and cache exists)
if [[ "${1:-}" != "rebuild" || ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -Wno-dev
fi

# 2. Build
cmake --build "$BUILD_DIR" --parallel "$JOBS"

# 3. Strip debug info (DWARF sections bloat ~4x; .obj keep debug for backtraces)
STRIP="${STRIP:-i686-w64-mingw32-strip}"
if command -v "$STRIP" &>/dev/null; then
    "$STRIP" "$BIN_DIR/caster.exe" "$BIN_DIR/hook.dll"
fi

# 4. Zip both binaries to release/ (overwrites previous build)
mkdir -p "$RELEASE_DIR"
ZIP_PATH="${RELEASE_DIR}/caster.zip"
( cd "$BIN_DIR" && zip -j "$ZIP_PATH" caster.exe hook.dll )

echo "$ZIP_PATH"
