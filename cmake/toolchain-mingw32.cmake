# cmake/toolchain-mingw32.cmake
#
# Cross-compile from Linux (host) to Windows 32-bit (target) using MinGW-w64
# with the i686-w64-mingw32 triplet.
#
# Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw32.cmake
#
# Static linkage of libstdc++, libgcc and libwinpthread is enabled by default
# so the resulting caster.exe and hook.dll have no MinGW runtime DLL dependencies.

set(CMAKE_SYSTEM_NAME       Windows)
set(CMAKE_SYSTEM_PROCESSOR  x86)

# ---- Locate the cross-compiler ---------------------------------------------
find_program(MINGW32_C_COMPILER
    NAMES i686-w64-mingw32-gcc i686-w64-mingw32-gcc-posix)
find_program(MINGW32_CXX_COMPILER
    NAMES i686-w64-mingw32-g++ i686-w64-mingw32-g++-posix)
find_program(MINGW32_RC_COMPILER
    NAMES i686-w64-mingw32-windres)

if(NOT MINGW32_C_COMPILER OR NOT MINGW32_CXX_COMPILER)
    message(FATAL_ERROR
        "MinGW-w64 i686 cross-compiler not found.\n"
        "On Debian/Ubuntu: sudo apt-get install mingw-w64\n"
        "On Fedora:        sudo dnf install mingw64-gcc   # provides i686 too\n"
        "On Arch:          sudo pacman -S mingw-w64-gcc")
endif()

set(CMAKE_C_COMPILER         ${MINGW32_C_COMPILER})
set(CMAKE_CXX_COMPILER       ${MINGW32_CXX_COMPILER})
if(MINGW32_RC_COMPILER)
    set(CMAKE_RC_COMPILER    ${MINGW32_RC_COMPILER})
endif()

# Prefer the posix threads variant if available (better std::thread / mutex support).
if(MINGW32_CXX_COMPILER MATCHES "posix")
    set(CASTER_MINGW_THREADS posix)
else()
    set(CASTER_MINGW_THREADS win32)
endif()
message(STATUS "[caster] MinGW threads flavor: ${CASTER_MINGW_THREADS}")

# ---- Sysroot / search paths ------------------------------------------------
# /usr/i686-w64-mingw32 is the standard sysroot on Debian/Ubuntu.
# Fall back to the compiler's own search path if not present.
if(EXISTS /usr/i686-w64-mingw32)
    set(CMAKE_FIND_ROOT_PATH /usr/i686-w64-mingw32)
else()
    execute_process(
        COMMAND ${MINGW32_C_COMPILER} -print-sysroot
        OUTPUT_VARIABLE CASTER_MINGW_SYSROOT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(CASTER_MINGW_SYSROOT)
        set(CMAKE_FIND_ROOT_PATH ${CASTER_MINGW_SYSROOT})
    endif()
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ---- Static linkage of the C++/Win32 runtime --------------------------------
# -static              -> link libgcc, libstdc++, libwinpthread statically into the binary
# -static-libgcc       -> also force libgcc static even if -static fails for some libs
# -static-libstdc++    -> same for libstdc++
# -Wl,--gc-sections    -> drop unused sections (smaller binaries)
# -Wl,--as-needed       -> drop unneeded DT_NEEDED entries
foreach(_lang C CXX)
    set(CMAKE_${_lang}_FLAGS_INIT
        "${CMAKE_${_lang}_FLAGS_INIT} -ffunction-sections -fdata-sections")
endforeach()

set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-static -static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,--as-needed")
set(CMAKE_SHARED_LINKER_FLAGS_INIT
    "-static -static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,--as-needed")
set(CMAKE_MODULE_LINKER_FLAGS_INIT
    "-static -static-libgcc -static-libstdc++")

# Tell SDL2's CMake to look for win32 native tools, not host tools.
set(SDL_WINDOWS_VERSION 0x0601 CACHE STRING "Minimum Windows version (Win7+)" FORCE)
