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
set(FETCHCONTENT_QUIET ON)
# Suppress CMake CMP0135 timestamp warning on URL-based FetchContent.
set(CMAKE_POLICY_DEFAULT_CMP0135 NEW)

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

# Force DirectInput detection on (mingw-w64 ships dinput.h but CMake's
# check_include_file sometimes fails to find it under cross-compile).
set(HAVE_DINPUT_H ON CACHE BOOL "" FORCE)
# Force XInput OFF — mingw-w64 ships xinput.h but it conflicts with SDL's
# own SDL_xinput.h (redefinition of XINPUT_CAPABILITIES_EX). We provide a
# stub for SDL_XINPUT_GetSteamVirtualGamepadSlot in sdl_xinput_stub.cpp.
set(HAVE_XINPUT_H OFF CACHE BOOL "" FORCE)
set(SDL_ALSA           OFF CACHE BOOL "" FORCE)
set(SDL_PIPEWIRE       OFF CACHE BOOL "" FORCE)
set(SDL_PULSEAUDIO     OFF CACHE BOOL "" FORCE)
set(SDL_SNDIO          OFF CACHE BOOL "" FORCE)
set(SDL_LIBSAMPLERATE  OFF CACHE BOOL "" FORCE)
set(SDL_DISKAUDIO      OFF CACHE BOOL "" FORCE)
set(SDL_DUMMYAUDIO     OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK       ON  CACHE BOOL "" FORCE)  # required for controller mapping
set(SDL_HAPTIC         OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI         OFF CACHE BOOL "" FORCE)
set(SDL_GAMECONTROLLER OFF CACHE BOOL "" FORCE)  # use SDL_Joystick* low-level API like zzcaster
set(SDL_POWER          OFF CACHE BOOL "" FORCE)
set(SDL_VIRTUAL_JOYSTICK OFF CACHE BOOL "" FORCE)  # avoid SDL_XINPUT_GetSteamVirtualGamepadSlot link error
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

# SDL2's own source uses #if on potentially-undefined macros (HAVE_DRIVER_BACKEND,
# HAVE_DXGI_H) which triggers -Wundef under GCC. Suppress it for SDL2-static only.
if(TARGET SDL2-static AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(SDL2-static PRIVATE -Wno-undef)
endif()

# --- Dear ImGui (master, v1.90.7) -------------------------------------------
# ImGui ships no CMakeLists; we add a static library target manually.
# FetchContent_Populate only fetches sources without running subproject
# CMakeLists. Suppress CMP0169 deprecation warning.
set(CMAKE_POLICY_DEFAULT_CMP0169 OLD)
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
# ENet ships its own CMakeLists that creates a conflicting "enet" target, so
# we use FetchContent_Populate (which only fetches sources without running
# the subproject's CMakeLists) and declare our own static lib. Suppress the
# CMP0169 deprecation warning for this specific call.
set(CMAKE_POLICY_DEFAULT_CMP0169 OLD)
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
    src/common/controller/mapping.cpp
    src/common/controller/binder.cpp
    src/common/net/connection_detector.cpp
    src/common/net/enet_transport.cpp
    src/common/net/ip_discovery.cpp
    src/common/net/connection_type.cpp
    src/common/net/relay/relay_protocol.cpp
    src/common/net/relay/relay_config.cpp
    src/common/net/relay/relay_client.cpp
)
target_include_directories(caster_common PUBLIC src/common)
target_link_libraries(caster_common PUBLIC
    imgui
    SDL2::SDL2-static
    SDL2::SDL2main
    enet
)
if(WIN32)
    target_link_libraries(caster_common PUBLIC opengl32 gdi32 user32 iphlpapi wininet ws2_32)
endif()
target_compile_features(caster_common PUBLIC cxx_std_23)

# Standalone object library for the XInput stub — linked directly into
# caster.exe and hook.dll (not via caster_common) so it's always pulled
# in by the linker even though it's only referenced by SDL2's
# SDL_windowsjoystick.c (which comes AFTER caster_common on the link line).
add_library(sdl_xinput_stub OBJECT src/common/sdl_xinput_stub.cpp)
target_compile_features(sdl_xinput_stub PRIVATE cxx_std_23)

# miniz as a separate object library so we can suppress its -Wattributes
# warnings (GCC 16+ requires `inline` alongside `__attribute__((always_inline))`,
# but miniz is 3rd-party code we don't want to patch).
add_library(miniz_obj OBJECT 3rdparty/miniz/miniz.c)
target_include_directories(miniz_obj PRIVATE 3rdparty/miniz)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(miniz_obj PRIVATE -Wno-attributes)
endif()

# ----------------------------------------------------------------------------
# caster.exe — the injector
# ----------------------------------------------------------------------------
add_executable(caster WIN32
    src/exe/main.cpp
    src/exe/injector.cpp
    src/exe/cli_args.cpp
    src/exe/cli.cpp
    src/exe/pages/main_menu.cpp
    src/exe/pages/header.cpp
    src/exe/pages/sidebar.cpp
    src/exe/pages/play_page.cpp
    src/exe/pages/config_page.cpp
    src/exe/pages/controllers_page.cpp
    src/exe/pages/controller_helpers.cpp
    src/exe/pages/waiting_for_peer.cpp
    src/exe/launcher/launcher.cpp
    src/exe/launcher/game_runner.cpp
    src/exe/session/session.cpp
)
target_link_libraries(caster PRIVATE caster_common sdl_xinput_stub)
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
    src/dll/netplay_connector.cpp
    src/dll/netplay_manager.cpp
    src/dll/ipc_receiver.cpp
    src/dll/character_select.cpp
    src/dll/messages.cpp
    src/dll/string_utils.cpp
    src/dll/exceptions.cpp
    src/dll/compression.cpp
    src/dll/protocol.cpp
    src/dll/input_reader.cpp
    src/dll/asm_hacks.cpp
    src/dll/frame_rate.cpp
    src/dll/dll_process_manager.cpp
    src/dll/dll_hacks.cpp
    src/dll/mem_dump.cpp
    src/dll/rollback_manager.cpp
    3rdparty/md5/md5.c
    3rdparty/d3dhook/D3DHook.cc
    3rdparty/d3dhook/CHookJump.cc
    3rdparty/minhook/src/hook.cc
    3rdparty/minhook/src/buffer.cc
    3rdparty/minhook/src/trampoline.cc
    3rdparty/minhook/src/export.cc
    3rdparty/minhook/src/thread.cc
    3rdparty/minhook/src/hde32/hde32.c
)
target_include_directories(hook PRIVATE
    src/dll
    3rdparty/miniz
    3rdparty/md5
    3rdparty/d3dhook
    3rdparty/minhook/include
    3rdparty/minhook/src
    3rdparty/minhook/src/hde32
)
target_link_libraries(hook PRIVATE
    caster_common
    sdl_xinput_stub
    enet
    miniz_obj
)
target_compile_definitions(hook PRIVATE
    CASTER_NET_PORT=${CASTER_NET_PORT}
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    CASTER_HOOK_DLL=1
)
if(WIN32)
    target_link_libraries(hook PRIVATE ws2_32 winmm d3d9 d3dx9)
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
