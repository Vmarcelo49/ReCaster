#!/usr/bin/env bash
# scripts/setup-deps.sh
#
# Installs the MinGW-w64 cross-compiler (i686-w64-mingw32 triplet) and the
# build tools needed to cross-compile caster.exe + hook.dll from a Linux host.
#
# Run once on a fresh dev box. Re-running is idempotent.
#
# Note: SDL2, Dear ImGui and ENet are NOT installed by this script — they
# are fetched at CMake configure-time via FetchContent (see CMakeLists.txt).

set -euo pipefail

echo "==> caster: setup-deps.sh"
echo "    Host OS: $(uname -srm)"

# ----------------------------------------------------------------------------
# 1. Detect package manager
# ----------------------------------------------------------------------------
PKG_MANAGER=""
if command -v apt-get >/dev/null 2>&1; then
    PKG_MANAGER=apt
elif command -v dnf >/dev/null 2>&1; then
    PKG_MANAGER=dnf
elif command -v pacman >/dev/null 2>&1; then
    PKG_MANAGER=pacman
elif command -v zypper >/dev/null 2>&1; then
    PKG_MANAGER=zypper
else
    echo "ERROR: unsupported distro — install mingw-w64 (i686) and cmake manually."
    exit 1
fi
echo "    Package manager: $PKG_MANAGER"

# ----------------------------------------------------------------------------
# 2. Install packages
# ----------------------------------------------------------------------------
case "$PKG_MANAGER" in
    apt)
        sudo apt-get update
        sudo apt-get install -y \
            mingw-w64 \
            mingw-w64-tools \
            cmake \
            build-essential \
            git \
            pkg-config \
            wget \
            ca-certificates
        ;;
    dnf)
        # Fedora's mingw packages default to 64-bit; we need the 32-bit triplet
        # which is provided by mingw32-gcc.
        sudo dnf install -y \
            mingw32-gcc \
            mingw32-gcc-c++ \
            mingw32-binutils \
            mingw32-winpthreads-static \
            mingw32-headers \
            cmake \
            make \
            git \
            pkgconfig \
            wget \
            ca-certificates
        ;;
    pacman)
        sudo pacman -S --needed \
            mingw-w64-gcc \
            cmake \
            make \
            git \
            pkgconf \
            wget \
            ca-certificates
        ;;
    zypper)
        sudo zypper install -y \
            mingw32-cross-gcc-c++ \
            mingw32-cross-binutils \
            mingw32-cross-headers \
            mingw32-cross-winpthreads-devel \
            cmake \
            make \
            git \
            wget
        ;;
esac

# ----------------------------------------------------------------------------
# 3. Verify the cross-compiler is on PATH
# ----------------------------------------------------------------------------
CXX=i686-w64-mingw32-g++
CC=i686-w64-mingw32-gcc
RC=i686-w64-mingw32-windres

# Some distros ship only the posix threads flavor — pick whichever exists.
if ! command -v "$CXX" >/dev/null 2>&1; then
    CXX=i686-w64-mingw32-g++-posix
    CC=i686-w64-mingw32-gcc-posix
fi

for tool in "$CC" "$CXX" "$RC" cmake git; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: required tool not on PATH: $tool"
        exit 1
    fi
done

echo
echo "==> Tool versions:"
"$CC"  --version | head -1 | sed 's/^/    /'
"$CXX" --version | head -1 | sed 's/^/    /'
"$RC"  --version | head -1 | sed 's/^/    /'
cmake  --version | head -1 | sed 's/^/    /'

# ----------------------------------------------------------------------------
# 4. Verify the C++ standard library is available for i686
# ----------------------------------------------------------------------------
SYSROOT_CXX=$("$CXX" -print-file-name=libstdc++.a 2>/dev/null || true)
if [[ -z "$SYSROOT_CXX" || ! -f "$SYSROOT_CXX" ]]; then
    echo "WARN: libstdc++.a not found for i686-w64-mingw32."
    echo "      Static C++ runtime linkage may fail. Install mingw-w64 C++ runtime."
else
    echo "    libstdc++.a : $SYSROOT_CXX"
fi

SYSROOT_PTHREAD=$("$CXX" -print-file-name=libwinpthread.a 2>/dev/null || true)
if [[ -z "$SYSROOT_PTHREAD" || ! -f "$SYSROOT_PTHREAD" ]]; then
    echo "WARN: libwinpthread.a not found for i686-w64-mingw32."
    echo "      std::thread in the DLL may fail to link."
else
    echo "    libwinpthread.a : $SYSROOT_PTHREAD"
fi

echo
echo "==> setup-deps.sh: OK"
echo "    Next: ./scripts/build.sh"
