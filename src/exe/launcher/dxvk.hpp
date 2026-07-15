// src/exe/launcher/dxvk.hpp
//
// DXVK integration — bundles + deploys the prebuilt d3d9.dll from the
// official DXVK release (https://github.com/doitsujin/dxvk) next to
// MBAA.exe at runtime, when Vulkan is available.
//
// Why: MBAACC has very inconsistent frametimes on modern hardware
// (16ms→40ms spikes are common) because the D3D9 driver on Windows
// doesn't optimize for 2012-era games. DXVK translates D3D9 to Vulkan,
// which has lower overhead, better frame pacing, and a driver-level
// frame limiter that's far more precise than our QPC-based limiter.
//
// What this module does (at launch time, before CreateProcessW):
//   1. is_vulkan_available() — checks for vulkan-1.dll + a working
//      vkCreateInstance. If false, we skip DXVK entirely (fallback to
//      native D3D9, which still works fine, just with worse frametimes).
//   2. deploy(game_dir) — copies the bundled d3d9.dll from next to
//      caster.exe into <game_dir>/d3d9.dll. Windows' DLL search order
//      loads d3d9.dll from the exe's folder before system32, so MBAA.exe
//      picks up the DXVK version automatically.
//   3. set_env_vars(game_dir) — sets the optimal DXVK env vars:
//        DXVK_HUD=0                          (we have our own overlay)
//        DXVK_STATE_CACHE=1                  (persistent shader cache)
//        DXVK_STATE_CACHE_PATH=<game_dir>    (cache next to the game)
//        DXVK_FRAME_RATE=60                  (precise driver-level limiter)
//        DXVK_MAX_FRAME_LATENCY=1            (minimum input lag)
//      These are set via SetEnvironmentVariableA so the child process
//      inherits them via CreateProcess.
//   4. cleanup(game_dir) — optional, removes the deployed d3d9.dll and
//      the state cache directory after the game exits. Currently
//      NOT called by default — the state cache is expensive to rebuild
//      (re-compiles all shaders on first run), so we leave it in place
//      across sessions. The deployed d3d9.dll is overwritten on the
//      next launch, not removed.
//
// What this module does NOT do:
//   - Does NOT touch the hook.dll's frame limiter logic. The DLL detects
//     DXVK indirectly: if DXVK is active, the env var DXVK_FRAME_RATE=60
//     is set, and the DLL's frame_limiter.cpp checks for that env var
//     and disables its own limiter (DXVK's driver-level limiter is more
//     precise). See frame_limiter.cpp.
//   - Does NOT modify the overlay. The overlay's vtable swap works
//     identically under DXVK (which implements the same D3D9 COM
//     interface) — confirmed in commit db41601.
//   - Does NOT install DXVK globally. The DLL is copied per-game-dir,
//     so multiple games on the same machine don't interfere.
//
// Threading: all functions are called from the GameRunner worker thread
// (Layer 2 of the threading migration — see docs/threading-migration.md).
// No internal threading, no mutexes.

#pragma once

#include <string>

namespace caster::exe::launcher::dxvk {

// Check if Vulkan is available on this machine.
//
// Tests for two things:
//   1. vulkan-1.dll is loadable (LoadLibraryA).
//   2. vkCreateInstance is exported and callable with a no-op allocator.
//
// The instance is created and immediately destroyed — we just want to
// confirm the Vulkan loader + ICD are functional, not actually use the
// instance for anything. This catches the case where vulkan-1.dll
// exists but no ICD is installed (e.g. fresh Windows install without
// GPU drivers).
//
// Returns true if Vulkan is available. The check is cached after the
// first call (per-process) — repeated calls are O(1).
bool is_vulkan_available();

// Deploy the bundled DXVK d3d9.dll into the game directory.
//
// `bundled_dll_path` is the absolute path to the d3d9.dll that ships
// next to caster.exe (e.g. C:\path\to\caster\d3d9.dll).
// `game_dir` is the absolute path to the directory containing MBAA.exe.
//
// Behavior:
//   - If the bundled DLL doesn't exist, returns false + fills `error`.
//     This shouldn't happen in a normal install (CMakeLists.txt copies
//     d3d9.dll into build/bin/ at configure time, and build.sh zips it
//     into caster.zip). But we handle it defensively.
//   - If <game_dir>/d3d9.dll already exists with the same size, we skip
//     the copy (avoids unnecessary writes + antivirus triggers). If the
//     sizes differ, we overwrite (likely a DXVK version upgrade).
//   - After successful copy, returns true.
//
// Returns false only on hard failure (source missing, dest unwritable).
bool deploy(const std::string& bundled_dll_path,
            const std::string& game_dir,
            std::string& error);

// Set the optimal DXVK environment variables for MBAACC.
//
// `game_dir` is used as the DXVK_STATE_CACHE_PATH so the shader cache
// lives next to the game (portable — no %LOCALAPPDATA% pollution).
//
// These are set via SetEnvironmentVariableA, so the child process
// (MBAA.exe) inherits them via CreateProcess.
//
// Idempotent — calling it multiple times is fine (just re-sets the
// same values).
void set_env_vars(const std::string& game_dir);

// Remove the deployed d3d9.dll and the DXVK state cache directory.
//
// Currently NOT called by default — the state cache is expensive to
// rebuild, so we leave it across sessions. The deployed d3d9.dll is
// overwritten on next launch, not removed. This function exists for
// future use (e.g. an "uninstall DXVK" button in the config UI, or
// a "reset shader cache" troubleshooting option).
//
// Returns true if anything was removed.
bool cleanup(const std::string& game_dir);

} // namespace caster::exe::launcher::dxvk
