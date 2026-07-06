# caster — root CMakeLists.txt
#
# Cross-compiled C++23 skeleton. Builds two artifacts against the
# i686-w64-mingw32 toolchain:
#
#   caster.exe  — SDL2+ImGui "Hello World" injector that LoadLibraryA-injects
#                 hook.dll into a hardcoded target process.
#   hook.dll    — Payload: on DllMain spawns two worker threads
#                 (ENet listener on port 7500 + own SDL2/ImGui window).
#
# Dependencies (SDL2, Dear ImGui, ENet) are fetched at configure-time via
# FetchContent and compiled from source — no prebuilt libs, no vcpkg.

cmake_minimum_required(VERSION 3.20)
project(caster
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "Rollback netplay injector + payload skeleton (C++23, MinGW32)")

# ----------------------------------------------------------------------------
# Language / global settings
# ----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Runtime output goes to build/bin so both binaries end up in the same folder
# (caster.exe needs to find hook.dll next to it at runtime).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ----------------------------------------------------------------------------
# Project options
# ----------------------------------------------------------------------------
# Name of the process that caster.exe will inject hook.dll into.
# zzcaster targets MBAACC; we keep the same target. Override at configure
# time with -DCASTER_TARGET_PROCESS=foo.exe.
set(CASTER_TARGET_PROCESS "MBAA.exe"
    CACHE STRING "Process name the injector/launcher will target (default: MBAA.exe)")
set(CASTER_NET_PORT 7500
    CACHE STRING "UDP/TCP port the hook.dll ENet placeholder listener binds to")

# ----------------------------------------------------------------------------
# Dependencies via FetchContent
# ----------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- SDL2 (built as static lib) ---------------------------------------------
# Disable everything we don't need; the more we disable, the fewer native
# deps we have to satisfy on the cross target.
set(SDL_SHARED         OFF CACHE BOOL "" FORCE)
set(SDL_STATIC         ON  CACHE BOOL "" FORCE)
set(SDL_TEST           OFF CACHE BOOL "" FORCE)
set(SDL_OPENGL         ON  CACHE BOOL "" FORCE)
set(SDL_OPENGLES       OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN         OFF CACHE BOOL "" FORCE)
set(SDL_DIRECTX        OFF CACHE BOOL "" FORCE)
set(SDL_X11            OFF CACHE BOOL "" FORCE)
set(SDL_XINPUT         OFF CACHE BOOL "" FORCE)  # avoid conflict with mingw-w64's xinput.h
set(SDL_WAYLAND        OFF CACHE BOOL "" FORCE)
set(SDL_ALSA           OFF CACHE BOOL "" FORCE)
set(SDL_PIPEWIRE       OFF CACHE BOOL "" FORCE)
set(SDL_PULSEAUDIO     OFF CACHE BOOL "" FORCE)
set(SDL_SNDIO          OFF CACHE BOOL "" FORCE)
set(SDL_LIBSAMPLERATE  OFF CACHE BOOL "" FORCE)
set(SDL_DISKAUDIO      OFF CACHE BOOL "" FORCE)
set(SDL_DUMMYAUDIO     OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK       OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC         OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI         OFF CACHE BOOL "" FORCE)
set(SDL_POWER          OFF CACHE BOOL "" FORCE)
set(SDL_TIMERS         ON  CACHE BOOL "" FORCE)
set(SDL_FILESYSTEM     OFF CACHE BOOL "" FORCE)
set(SDL_LOADSO         ON  CACHE BOOL "" FORCE)
set(SDL_CPUINFO        OFF CACHE BOOL "" FORCE)
set(SDL_FILE           OFF CACHE BOOL "" FORCE)
set(SDL_DLOPEN         OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR         OFF CACHE BOOL "" FORCE)
set(SDL_RENDER         OFF CACHE BOOL "" FORCE)  # we render via raw OpenGL3
set(SDL_EVENTS         ON  CACHE BOOL "" FORCE)
set(SDL_ATOMIC         ON  CACHE BOOL "" FORCE)
set(SDL_VIDEO          ON  CACHE BOOL "" FORCE)
set(SDL_THREADS        ON  CACHE BOOL "" FORCE)

FetchContent_Declare(
    sdl2
    URL      https://github.com/libsdl-org/SDL/releases/download/release-2.30.0/SDL2-2.30.0.tar.gz
    URL_HASH SHA256=36e2e41557e0fa4a1519315c0f5958a87ccb27e25c51776beb6f1239526447b0
)
FetchContent_MakeAvailable(sdl2)

# --- Dear ImGui (master, v1.90.7) -------------------------------------------
# ImGui ships no CMakeLists; we add a static library target manually.
# Using FetchContent_Populate (deprecated but works on all CMake 3.x) so we
# can declare the source list ourselves.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui PUBLIC
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    # Don't define IMGUI_IMPL_OPENGL_LOADER_CUSTOM — let ImGui 1.90+ use its
    # built-in imgui_impl_opengl3_loader.h which loads GL symbols at runtime.
)
# ImGui backend needs SDL2 headers + symbols.
target_link_libraries(imgui PUBLIC SDL2::SDL2-static)

# --- ENet 1.3.x (placeholder netplay listener) ------------------------------
FetchContent_Declare(
    enet
    URL      https://github.com/lsalzman/enet/archive/refs/tags/v1.3.18.tar.gz
    URL_HASH SHA256=28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd
)
FetchContent_GetProperties(enet)
if(NOT enet_POPULATED)
    FetchContent_Populate(enet)
endif()

add_library(enet STATIC
    ${enet_SOURCE_DIR}/callbacks.c
    ${enet_SOURCE_DIR}/compress.c
    ${enet_SOURCE_DIR}/host.c
    ${enet_SOURCE_DIR}/list.c
    ${enet_SOURCE_DIR}/packet.c
    ${enet_SOURCE_DIR}/peer.c
    ${enet_SOURCE_DIR}/protocol.c
    ${enet_SOURCE_DIR}/unix.c
    ${enet_SOURCE_DIR}/win32.c
)
target_include_directories(enet PUBLIC ${enet_SOURCE_DIR}/include)
if(WIN32)
    target_link_libraries(enet PUBLIC winmm ws2_32)
endif()

# ----------------------------------------------------------------------------
# Common library: shared between caster.exe and hook.dll
# ----------------------------------------------------------------------------
add_library(caster_common STATIC
    src/common/gui_window.cpp
    src/common/ini.cpp
    src/common/logger.cpp
    src/common/config.cpp
    src/common/ui_theme.cpp
    src/common/embedded_font.cpp
    src/common/win32/process.cpp
    src/common/win32/memory.cpp
    src/common/win32/pe_parser.cpp
    src/common/win32/env.cpp
    src/common/win32/window.cpp
    src/common/ipc/pipe_name.cpp
    src/common/ipc/config_buffer.cpp
    src/common/ipc/ipc_server.cpp
    src/common/ipc/ipc_client.cpp
)
target_include_directories(caster_common PUBLIC src/common)
target_link_libraries(caster_common PUBLIC
    imgui
    SDL2::SDL2-static
    SDL2::SDL2main
)
if(WIN32)
    target_link_libraries(caster_common PUBLIC opengl32 gdi32 user32)
endif()
target_compile_features(caster_common PUBLIC cxx_std_23)

# ----------------------------------------------------------------------------
# caster.exe — the injector
# ----------------------------------------------------------------------------
add_executable(caster WIN32
    src/exe/main.cpp
    src/exe/injector.cpp
    src/exe/cli_args.cpp
    src/exe/pages/main_menu.cpp
    src/exe/pages/header.cpp
    src/exe/pages/sidebar.cpp
    src/exe/pages/play_page.cpp
    src/exe/pages/config_page.cpp
    src/exe/pages/controllers_page.cpp
    src/exe/launcher/launcher.cpp
    src/exe/launcher/game_runner.cpp
)
target_link_libraries(caster PRIVATE caster_common)
target_compile_definitions(caster PRIVATE
    CASTER_TARGET_PROCESS="${CASTER_TARGET_PROCESS}"
    CASTER_NET_PORT=${CASTER_NET_PORT}
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
if(WIN32)
    target_link_libraries(caster PRIVATE psapi)
endif()
set_target_properties(caster PROPERTIES
    OUTPUT_NAME "caster"
    # WIN32 executable -> uses WinMain entry from SDL2main (no console window)
)

# ----------------------------------------------------------------------------
# hook.dll — the payload
# ----------------------------------------------------------------------------
add_library(hook SHARED
    src/dll/dll_main.cpp
    src/dll/net_listener.cpp
    src/dll/ipc_receiver.cpp
)
target_link_libraries(hook PRIVATE
    caster_common
    enet
)
target_compile_definitions(hook PRIVATE
    CASTER_NET_PORT=${CASTER_NET_PORT}
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    CASTER_HOOK_DLL=1
)
if(WIN32)
    target_link_libraries(hook PRIVATE ws2_32 winmm)
endif()
set_target_properties(hook PROPERTIES
    OUTPUT_NAME "hook"
    PREFIX ""          # produce hook.dll, not libhook.dll
    SUFFIX ".dll"
)

# ----------------------------------------------------------------------------
# Configure-time summary
# ----------------------------------------------------------------------------
message(STATUS "============================================================")
message(STATUS " caster ${PROJECT_VERSION}")
message(STATUS "   C++ standard      : ${CMAKE_CXX_STANDARD}")
message(STATUS "   Toolchain         : ${CMAKE_TOOLCHAIN_FILE}")
message(STATUS "   Build type        : ${CMAKE_BUILD_TYPE}")
message(STATUS "   Target process    : ${CASTER_TARGET_PROCESS}")
message(STATUS "   ENet port         : ${CASTER_NET_PORT}")
message(STATUS "   Binaries out dir  : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "============================================================")
