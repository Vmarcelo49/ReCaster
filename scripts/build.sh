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

# 0. Stale-cache guard.
#
# CMake silently ignores -DCMAKE_TOOLCHAIN_FILE when CMakeCache.txt already
# exists with a different compiler baked in. This happens if someone (or a
# previous version of this script) ever ran `cmake .` without the toolchain,
# leaving the cache pinned to the host's native GCC. Symptom: caster_common
# is built with /usr/bin/c++ (Linux) instead of i686-w64-mingw32-g++, and
# every TU that does #include <windows.h> fails with "Arquivo não encontrado".
#
# Fix: detect a cache that doesn't point at MinGW and purge it before
# reconfiguring. We only purge in the full-configure path (not on "rebuild").
#
# The check is a substring match on the cached CMAKE_CXX_COMPILER path —
# robust to absolute vs relative, and to /usr/bin vs /usr/local/bin.
if [[ "${1:-}" != "rebuild" && -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    CACHED_CXX="$(grep -E '^CMAKE_CXX_COMPILER:FILEPATH=' "${BUILD_DIR}/CMakeCache.txt" | cut -d= -f2- || true)"
    if [[ -n "$CACHED_CXX" && "$CACHED_CXX" != *mingw* ]]; then
        echo "build.sh: stale CMakeCache.txt detected (CMAKE_CXX_COMPILER='${CACHED_CXX}', expected a MinGW binary)." >&2
        echo "build.sh: purging ${BUILD_DIR} to force a clean reconfigure with the MinGW toolchain." >&2
        rm -rf "$BUILD_DIR"
    fi
fi

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
