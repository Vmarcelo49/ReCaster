#!/usr/bin/env bash
# scripts/build.sh
#
# Configure + build caster.exe and hook.dll using the MinGW-w64 i686 toolchain.
#
# Outputs land in build/bin/:
#   caster.exe  — injector (SDL2+ImGui GUI)
#   hook.dll    — payload (ENet listener + own SDL2/ImGui window)

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
BIN_DIR="${BUILD_DIR}/bin"
TOOLCHAIN_FILE="${ROOT_DIR}/cmake/toolchain-mingw32.cmake"

# Allow caller to override the target process name.
TARGET_PROCESS="${CASTER_TARGET_PROCESS:-kof98.exe}"
BUILD_TYPE="${CASTER_BUILD_TYPE:-Release}"
JOBS="${CASTER_BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}"

echo "==> caster: build.sh"
echo "    Source        : $ROOT_DIR"
echo "    Build dir     : $BUILD_DIR"
echo "    Build type    : $BUILD_TYPE"
echo "    Target process: $TARGET_PROCESS"
echo "    Parallel jobs : $JOBS"
echo

# ----------------------------------------------------------------------------
# 1. Verify toolchain presence
# ----------------------------------------------------------------------------
if ! command -v i686-w64-mingw32-g++ >/dev/null 2>&1 \
   && ! command -v i686-w64-mingw32-g++-posix >/dev/null 2>&1; then
    echo "ERROR: i686-w64-mingw32-g++ not found."
    echo "       Run ./scripts/setup-deps.sh first."
    exit 1
fi

if [[ ! -f "$TOOLCHAIN_FILE" ]]; then
    echo "ERROR: toolchain file not found: $TOOLCHAIN_FILE"
    exit 1
fi

# ----------------------------------------------------------------------------
# 2. Configure
# ----------------------------------------------------------------------------
echo "==> Configuring (CMake + MinGW toolchain)..."
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCASTER_TARGET_PROCESS="$TARGET_PROCESS" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -Wno-dev

# ----------------------------------------------------------------------------
# 3. Build
# ----------------------------------------------------------------------------
echo
echo "==> Building (cmake --build, $JOBS parallel jobs)..."
cmake --build "$BUILD_DIR" --parallel "$JOBS"

# ----------------------------------------------------------------------------
# 3b. Strip DWARF debug info from the final binaries.
#
# MinGW GCC embeds DWARF debug sections by default (.debug_info, .eh_frame),
# which bloats the output ~4× even in Release builds (caster.exe: 19MB→5MB,
# hook.dll: 18MB→4MB). The compiler objects (.obj) keep full debug info for
# crash backtraces; we only strip the linked product.
# ----------------------------------------------------------------------------
STRIP="${STRIP:-i686-w64-mingw32-strip}"
if command -v "$STRIP" &>/dev/null; then
    echo
    echo "==> Stripping debug info from final binaries..."
    for bin in caster.exe hook.dll; do
        if [[ -f "$BIN_DIR/$bin" ]]; then
            before=$(stat -c%s "$BIN_DIR/$bin")
            "$STRIP" "$BIN_DIR/$bin"
            after=$(stat -c%s "$BIN_DIR/$bin")
            saved=$(( (before - after) / 1024 / 1024 ))
            printf "    %-12s  %d → %d bytes (%d MB saved)\n" \
                "$bin" "$before" "$after" "$saved"
        fi
    done
fi

# ----------------------------------------------------------------------------
# 4. Report
# ----------------------------------------------------------------------------
echo
echo "==> Build artifacts:"
if [[ -f "$BIN_DIR/caster.exe" ]]; then
    ls -la "$BIN_DIR/caster.exe" | sed 's/^/    /'
    file "$BIN_DIR/caster.exe" 2>/dev/null | sed 's/^/    /' || true
else
    echo "    !! caster.exe missing"
fi
if [[ -f "$BIN_DIR/hook.dll" ]]; then
    ls -la "$BIN_DIR/hook.dll" | sed 's/^/    /'
    file "$BIN_DIR/hook.dll" 2>/dev/null | sed 's/^/    /' || true
else
    echo "    !! hook.dll missing"
fi

echo
echo "==> Done."
echo "    Copy both caster.exe and hook.dll into the same folder on Windows,"
echo "    launch $TARGET_PROCESS, then run caster.exe and click Inject."
