// src/dll/dll_main.cpp
//
// Entry point for hook.dll. Follows CCCaster's synchronous approach:
// - DllMain: initializePreLoad() only (fast, no blocking)
// - callback() (first frame): initializePostLoad() + IPC receive + forceGoto
// - callback() (every frame): read input + write to game
//
// No extra threads. Everything runs on the game's main thread via callback().

#include "constants.hpp"
#include "asm_hacks.hpp"
#include "dll_hacks.hpp"
#include "frame_rate.hpp"
#include "dll_process_manager.hpp"
#include "input_reader.hpp"
#include "ipc_receiver.hpp"
#include "../common/ipc/pipe_name.hpp"
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

namespace {

// Global state
std::atomic<bool> g_running{false};
bool g_postLoadDone = false;
bool g_ipcDone = false;
bool g_modePatchApplied = false;

// Diagnostics: track game-mode progression over time
uint64_t g_frameCount = 0;
uint32_t g_lastLoggedGameMode = 0xFFFFFFFF;

// Controller state
caster::common::controller::ControllerMapping g_p1Mapping;
caster::common::controller::ControllerMapping g_p2Mapping;
SDL_Joystick* g_p1Joy = nullptr;
SDL_Joystick* g_p2Joy = nullptr;
bool g_mappingsLoaded = false;

// Load controller mappings from mapping.ini
void loadMappings() {
    if (g_mappingsLoaded) return;

    char exePath[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string dir(exePath);
    size_t pos = dir.find_last_of("\\/");
    if (pos != std::string::npos) dir = dir.substr(0, pos + 1);

    std::string mappingPath = dir + "caster\\mapping.ini";

    g_p1Mapping = caster::common::controller::ControllerMapping::default_xbox();
    g_p2Mapping = caster::common::controller::ControllerMapping::default_xbox();
    caster::common::controller::load_mapping(mappingPath, g_p1Mapping, g_p2Mapping);

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

// First-frame initialization
void doPostLoad() {
    if (g_postLoadDone) return;
    g_postLoadDone = true;

    caster::common::logger::info("dll_main: post-load initialization");
    caster::dll::dll_hacks::initializePostLoad();
    loadMappings();
    caster::common::logger::info("dll_main: post-load complete");
}

// IPC receive + forceGoto — synchronous, runs in callback()
void doIpcAndModePatch() {
    if (g_modePatchApplied) return;

    if (!g_ipcDone) {
        g_ipcDone = true;

        std::string pipe_name = caster::common::ipc::pipe_name::from_env();
        caster::common::logger::info("dll_main: IPC pipe name from env: '{}'",
                                     pipe_name.empty() ? "(empty - CASTER_PIPE not set)" : pipe_name);

        caster::common::logger::info("dll_main: receiving IPC config...");
        caster::dll::ipc_receiver::receive(10000);
        caster::common::logger::info("dll_main: IPC ready={}, status={}",
                                     caster::dll::ipc_receiver::is_ready(),
                                     caster::dll::ipc_receiver::status_string());
    }

    if (!caster::dll::ipc_receiver::is_ready()) return;

    caster::common::ipc::config_buffer::Config cfg;
    if (!caster::dll::ipc_receiver::get_config(cfg)) return;

    caster::common::logger::info("dll_main: config flags=0x{:02x} training={} netplay={}",
                                 cfg.flags, cfg.is_training(), cfg.is_netplay());

    // Read original bytes at 0x42B475 before patching (diagnosis)
    uint8_t orig[4] = {0};
    memcpy(orig, (void*)0x42B475, 4);
    caster::common::logger::info("dll_main: original bytes at 0x42B475: {:02x} {:02x} {:02x} {:02x}",
                                 orig[0], orig[1], orig[2], orig[3]);

    // Log game mode before patching
    uint32_t gameMode = *(uint32_t*)caster::dll::CC_GAME_MODE_ADDR;
    caster::common::logger::info("dll_main: game mode before patch: {} ({})",
                                 gameMode, caster::dll::gameModeStr(gameMode));

    if (cfg.is_training()) {
        caster::dll::asm_hacks::forceGotoTraining.write();
        caster::common::logger::info("dll_main: applied forceGotoTraining at 0x42B475");
    } else {
        caster::dll::asm_hacks::forceGotoVersus.write();
        caster::common::logger::info("dll_main: applied forceGotoVersus at 0x42B475");
    }

    // Verify patch was written
    uint8_t after[4] = {0};
    memcpy(after, (void*)0x42B475, 4);
    caster::common::logger::info("dll_main: bytes after patch: {:02x} {:02x} {:02x} {:02x}",
                                 after[0], after[1], after[2], after[3]);

    g_modePatchApplied = true;
}

// Per-frame logic
void frameStep() {
    doPostLoad();
    doIpcAndModePatch();

    // Periodic diagnostics: log whenever the game mode changes, so we can
    // confirm the menu navigation drives the game into the target mode.
    ++g_frameCount;
    const uint32_t gameMode = *(uint32_t*)caster::dll::CC_GAME_MODE_ADDR;
    if (gameMode != g_lastLoggedGameMode) {
        caster::common::logger::info(
            "frameStep: frame={} mode={} ({}) — mode CHANGED",
            g_frameCount, gameMode, caster::dll::gameModeStr(gameMode));
        g_lastLoggedGameMode = gameMode;
    }

    // === Menu navigation (port of CCCaster's getPreInitialInput/getInitialInput) ===
    //
    // forceGoto at 0x42B475 only executes once the game reaches the mode-select
    // screen (CC_GAME_MODE_MAIN = 25). To get there we must drive the game past
    // Startup → Opening → Title → Main Menu by injecting the Confirm button,
    // exactly like CCCaster does in DllNetplayManager.cpp:88-112.
    //
    // The mash STOPS once the game has progressed past the menu flow — i.e.
    // when it reaches CharaSelect (forceGoto "took effect"), In-game, or Retry.
    // From there the local controller takes over (getCharaSelectInput/getInGameInput).
    //
    // mashConfirm pulses on even frames (press) / off on odd frames (release),
    // matching CCCaster's RETURN_MASH_INPUT macro. menuConfirmState=2 is
    // required so hijackMenu lets the injected confirm actually advance menus.
    const bool inMenuFlow = (gameMode == caster::dll::CC_GAME_MODE_STARTUP
                             || gameMode == caster::dll::CC_GAME_MODE_OPENING
                             || gameMode == caster::dll::CC_GAME_MODE_TITLE
                             || gameMode == caster::dll::CC_GAME_MODE_MAIN
                             || gameMode == caster::dll::CC_GAME_MODE_LOADING_DEMO
                             || gameMode == caster::dll::CC_GAME_MODE_HIGH_SCORES);
    if (g_modePatchApplied && inMenuFlow) {
        // Skip rendering during menu navigation so the Startup/Opening/Title/Main
        // screens whip by invisibly in milliseconds instead of being shown.
        // Mirrors CCCaster's frameStepNormal() PreInitial/Initial/AutoCharaSelect
        // case (DllMain.cpp:196-201). This also bypasses the FPS limiter
        // (limitFPS() checks CC_SKIP_FRAMES_ADDR), so frames run at full CPU speed.
        *(uint32_t*)caster::dll::CC_SKIP_FRAMES_ADDR = 1;

        caster::dll::asm_hacks::menuConfirmState = 2;
        const bool mashConfirm = ((g_frameCount % 2) == 0);
        const uint16_t buttons = mashConfirm ? caster::dll::CC_BUTTON_CONFIRM : 0;
        caster::dll::process_manager::writeGameInput(1, /*direction=*/0, buttons);
        return;
    }

    // === Past the menu (CharaSelect/InGame/Retry): read local controller ===
    // Ensure render-skip is off and menuConfirmState is 0 so the player's own
    // confirms work.
    *(uint32_t*)caster::dll::CC_SKIP_FRAMES_ADDR = 0;
    caster::dll::asm_hacks::menuConfirmState = 0;
    if (gameMode == caster::dll::CC_GAME_MODE_IN_GAME
        || gameMode == caster::dll::CC_GAME_MODE_CHARA_SELECT
        || gameMode == caster::dll::CC_GAME_MODE_RETRY) {
        if (g_p1Mapping.device_index >= 0 && g_p1Joy) {
            auto input = caster::dll::read_local_input(g_p1Joy, g_p1Mapping);
            caster::dll::process_manager::writeGameInput(1, input.direction, input.buttons);
        } else if (g_p1Mapping.device_index < 0) {
            auto input = caster::dll::read_local_input(nullptr, g_p1Mapping);
            caster::dll::process_manager::writeGameInput(1, input.direction, input.buttons);
        }
    }
}

} // namespace

// ---- callback() — per-frame hook entry point ----
extern "C" void callback() {
    if (!g_running.load()) return;
    try {
        frameStep();
    } catch (...) {
        caster::common::logger::err("dll_main: exception in callback()");
    }
}

// ---- stopDllMain() ----
namespace caster::dll::dll_hacks {
void stopDllMain(const std::string& error) {
    caster::common::logger::info("dll_main: stopDllMain('{}')", error);
    g_running.store(false);
}
} // namespace caster::dll::dll_hacks

// ---- D3DHook callbacks (global namespace) ----
void PresentFrameBegin(IDirect3DDevice9*) {}
void EndScene(IDirect3DDevice9*) {}
void InvalidateDeviceObjects() {}
void PresentFrameEnd(IDirect3DDevice9* device) {
    caster::dll::frame_rate::PresentFrameEnd(device);
}

// ---- DllMain ----
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            caster::common::logger::init({}, false);
            caster::common::logger::info("=== hook.dll injected ===");

            g_running.store(true);

            // Apply pre-load ASM hacks ONLY (fast, no blocking)
            caster::dll::dll_hacks::initializePreLoad();
            caster::common::logger::info("dll_main: pre-load hacks applied");
            break;
        }
        case DLL_PROCESS_DETACH: {
            caster::common::logger::info("dll_main: DLL_PROCESS_DETACH");
            g_running.store(false);

            if (g_p1Joy) { SDL_JoystickClose(g_p1Joy); g_p1Joy = nullptr; }
            if (g_p2Joy) { SDL_JoystickClose(g_p2Joy); g_p2Joy = nullptr; }

            caster::dll::dll_hacks::deinitialize();

            caster::common::logger::info("=== hook.dll detached ===");
            caster::common::logger::shutdown();
            break;
        }
        default:
            break;
    }
    return TRUE;
}
