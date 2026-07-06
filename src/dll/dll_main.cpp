// src/dll/dll_main.cpp
//
// Entry point for hook.dll. This is the REAL implementation that replaces
// the old skeleton (ENet listener + SDL2/ImGui window).
//
// On DLL_PROCESS_ATTACH:
//   1. Initialize logger
//   2. Apply pre-load ASM hacks (hijackControls, hookMainLoop, hijackMenu, etc.)
//   3. Start IPC receiver thread (reads config from launcher)
//
// The game's main loop calls callback() every frame (via the hookMainLoop
// ASM patch). On the first callback:
//   4. Apply post-load hacks (enableDisabledStages, DX9 hook, WindowProc hook)
//   5. Enable frame rate control
//
// Every frame in callback():
//   6. Read local controller input via input_reader
//   7. Write to game via process_manager::writeGameInput
//
// On DLL_PROCESS_DETACH:
//   8. Deinitialize all hacks
//   9. Cleanup

#include "constants.hpp"
#include "asm_hacks.hpp"
#include "dll_hacks.hpp"
#include "frame_rate.hpp"
#include "dll_process_manager.hpp"
#include "input_reader.hpp"
#include "ipc_receiver.hpp"
#include "net_listener.hpp"
#include "../common/logger.hpp"
#include "../common/controller/mapping.hpp"
#include "../common/ipc/config_buffer.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_joystick.h>

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <thread>

namespace {

// Global state
std::atomic<bool> g_running{false};
std::atomic<bool> g_postLoadDone{false};
std::atomic<bool> g_modePatchApplied{false};
std::thread g_ipcThread;
std::thread g_netListenerThread;

// Controller state
caster::common::controller::ControllerMapping g_p1Mapping;
caster::common::controller::ControllerMapping g_p2Mapping;
SDL_Joystick* g_p1Joy = nullptr;
SDL_Joystick* g_p2Joy = nullptr;
bool g_mappingsLoaded = false;

// Load controller mappings from mapping.ini (same file the GUI uses)
void loadMappings() {
    if (g_mappingsLoaded) return;

    // Resolve mapping.ini path: same dir as the game exe
    char exePath[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string dir(exePath);
    size_t pos = dir.find_last_of("\\/");
    if (pos != std::string::npos) dir = dir.substr(0, pos + 1);

    std::string mappingPath = dir + "caster\\mapping.ini";

    // Start with Xbox defaults
    g_p1Mapping = caster::common::controller::ControllerMapping::default_xbox();
    g_p2Mapping = caster::common::controller::ControllerMapping::default_xbox();

    // Try to load saved mappings
    caster::common::controller::load_mapping(mappingPath, g_p1Mapping, g_p2Mapping);

    // Open joysticks based on device_index
    if (g_p1Mapping.device_index >= 0) {
        g_p1Joy = SDL_JoystickOpen(g_p1Mapping.device_index);
    }
    if (g_p2Mapping.device_index >= 0) {
        g_p2Joy = SDL_JoystickOpen(g_p2Mapping.device_index);
    }

    g_mappingsLoaded = true;
    caster::common::logger::info("dll_main: mappings loaded (P1 device={}, P2 device={})",
                                 g_p1Mapping.device_index, g_p2Mapping.device_index);
}

// First-frame initialization (called from callback() once)
void doPostLoad() {
    if (g_postLoadDone.exchange(true)) return;

    caster::common::logger::info("dll_main: post-load initialization");

    // Apply post-load ASM hacks (enableDisabledStages, DX9 hook, WindowProc, etc.)
    caster::dll::dll_hacks::initializePostLoad();

    // Load controller mappings
    loadMappings();

    caster::common::logger::info("dll_main: post-load complete");
}

// Apply mode-specific patches (forceGotoTraining / forceGotoVersus) once
// the IPC config has been received from the launcher.
void maybeApplyModePatch() {
    if (g_modePatchApplied.load()) return;
    if (!caster::dll::ipc_receiver::is_ready()) return;

    caster::common::ipc::config_buffer::Config cfg;
    if (!caster::dll::ipc_receiver::get_config(cfg)) return;

    // Apply the forceGoto patch based on training flag.
    // The patch is at 0x42B475 and changes the jump destination:
    //   Training:  EB 22 (jmp 0x0042B499)
    //   Versus:    EB 3F (jmp 0x0042B4B6)
    if (cfg.is_training()) {
        caster::dll::asm_hacks::forceGotoTraining.write();
        caster::common::logger::info("dll_main: applied forceGotoTraining patch");
    } else {
        caster::dll::asm_hacks::forceGotoVersus.write();
        caster::common::logger::info("dll_main: applied forceGotoVersus patch");
    }

    g_modePatchApplied.store(true);
}

// Per-frame logic (called every frame by the hooked main loop)
void frameStep() {
    // First frame: do post-load init
    doPostLoad();

    // Apply mode-specific patches once IPC config is received
    maybeApplyModePatch();

    // Check if game is in a state where we should inject input
    // (only when in-game, not in menus/loading)
    uint32_t gameMode = *(uint32_t*)caster::dll::CC_GAME_MODE_ADDR;

    if (gameMode == caster::dll::CC_GAME_MODE_IN_GAME) {
        // Read P1 input and write to game
        if (g_p1Mapping.device_index >= 0 && g_p1Joy) {
            auto input = caster::dll::read_local_input(g_p1Joy, g_p1Mapping);
            caster::dll::process_manager::writeGameInput(1, input.direction, input.buttons);
        } else if (g_p1Mapping.device_index < 0) {
            // Keyboard
            auto input = caster::dll::read_local_input(nullptr, g_p1Mapping);
            caster::dll::process_manager::writeGameInput(1, input.direction, input.buttons);
        }

        // P2 input (only in versus/training with 2 human players or CPU)
        // For now, only P1 is controlled by us. P2 is CPU/default.
        // In netplay, P2 input comes from the remote player.
    }
}

} // namespace

// ---- callback() — the per-frame hook entry point ----
// This is called by the game's main loop via the hookMainLoop ASM patch.
// It must be extern "C" and naked-compatible (the ASM patch jumps to it).
extern "C" void callback() {
    if (!g_running.load()) return;

    try {
        frameStep();
    } catch (...) {
        // Never let an exception escape into the game's code
        caster::common::logger::err("dll_main: exception in callback()");
    }
}

// ---- stopDllMain() — called on fatal error / Alt+F4 ----
namespace caster::dll::dll_hacks {
void stopDllMain(const std::string& error) {
    caster::common::logger::info("dll_main: stopDllMain('{}')", error);
    g_running.store(false);
}
} // namespace caster::dll::dll_hacks

// ---- D3DHook callbacks (global namespace, as required by D3DHook.cc) ----

void PresentFrameBegin(IDirect3DDevice9*) {
    // Pre-Present hook. Will be used for overlay rendering later.
}

void EndScene(IDirect3DDevice9*) {
    // EndScene hook. Will be used for ImGui overlay later.
}

void InvalidateDeviceObjects() {
    // Called on device lost (alt-tab). Release overlay resources when we have them.
}

void PresentFrameEnd(IDirect3DDevice9* device) {
    caster::dll::frame_rate::PresentFrameEnd(device);
}

// ---- DllMain ----

extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            // Initialize logger
            caster::common::logger::init({}, false);
            caster::common::logger::info("=== hook.dll injected ===");

            g_running.store(true);

            // Apply pre-load ASM hacks (hookMainLoop, hijackControls, hijackMenu, etc.)
            // These patches the game's code BEFORE the main loop starts.
            caster::dll::dll_hacks::initializePreLoad();
            caster::common::logger::info("dll_main: pre-load hacks applied");

            // Start IPC receiver thread (reads config from launcher)
            g_ipcThread = std::thread([] {
                // Try to receive config for up to 10 seconds
                caster::dll::ipc_receiver::receive(10000);
            });

            // Start ENet listener thread (placeholder — will be used for netplay)
            g_netListenerThread = std::thread([] {
                std::atomic<bool> dummy_running{true};
                caster::dll::start_net_listener(dummy_running);
            });

            break;
        }
        case DLL_PROCESS_DETACH: {
            caster::common::logger::info("dll_main: DLL_PROCESS_DETACH");

            g_running.store(false);

            // Stop net listener
            caster::dll::stop_net_listener();

            // Join threads
            if (g_ipcThread.joinable()) g_ipcThread.join();
            if (g_netListenerThread.joinable()) g_netListenerThread.join();

            // Close joysticks
            if (g_p1Joy) { SDL_JoystickClose(g_p1Joy); g_p1Joy = nullptr; }
            if (g_p2Joy) { SDL_JoystickClose(g_p2Joy); g_p2Joy = nullptr; }

            // Deinitialize hacks (revert ASM patches, unhook DX9, unhook WindowProc)
            caster::dll::dll_hacks::deinitialize();

            caster::common::logger::info("=== hook.dll detached ===");
            caster::common::logger::shutdown();
            break;
        }
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
        default:
            break;
    }
    return TRUE;
}
