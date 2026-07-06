// src/dll/dll_main.cpp
//
// Entry point for hook.dll. Follows CCCaster's synchronous approach:
// - DllMain: initializePreLoad() only (fast, no blocking)
// - callback() (first frame): initializePostLoad() + IPC receive + forceGoto
// - callback() (every frame): ChangeMonitor → frameStep → writeGameInput
//
// No extra threads. Everything runs on the game's main thread via callback().
//
// F.3 wiring (see docs/phase-f-execution-plan.md):
//   - NetplayManager netMan owns the FSM, input containers, RngState history.
//   - ChangeMonitor (simplified) watches CC_GAME_MODE_ADDR, CC_GAME_STATE_ADDR,
//     and AsmHacks::roundStartCounter for changes; on change, calls
//     netplayStateChanged() which drives the FSM transitions.
//   - frameStep() each frame:
//       1. netMan.updateFrame() — refresh _indexedFrame from world timer
//       2. drain netplay inbox → netMan.setInputs / setRngState / setRemoteIndex /
//          setRemoteRetryMenuIndex
//       3. read local controller → netMan.setInput(localPlayer, combined)
//       4. (netplay only) send netMan.getInputs(localPlayer) to peer
//       5. process_manager::writeGameInput(p, netMan.getInput(p)) for both players
//       6. (netplay only) send any pending MenuIndex / RngState
//
// Rollback (Etapa F.5), SyncHash (F.4), and checkRoundOver (F.5) are not
// wired yet — those come in their own sub-steps.

#include "constants.hpp"
#include "asm_hacks.hpp"
#include "dll_hacks.hpp"
#include "frame_rate.hpp"
#include "dll_process_manager.hpp"
#include "input_reader.hpp"
#include "ipc_receiver.hpp"
#include "netplay_connector.hpp"
#include "netplay_manager.hpp"
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
#include <cstdint>

namespace {

// ============================================================================
// Global state
// ============================================================================

std::atomic<bool> g_running{false};
bool g_postLoadDone = false;
bool g_ipcDone = false;
bool g_modePatchApplied = false;

// The NetplayManager — brain of the DLL-side netplay engine.
caster::dll::NetplayManager g_netMan;

// Cached IPC config + derived player numbers. Set in doIpcAndModePatch().
caster::common::ipc::config_buffer::Config g_cfg;
bool     g_isNetplay   = false;
uint8_t  g_localPlayer = 1;
uint8_t  g_remotePlayer = 2;
bool     g_isHost      = false;

// ---- ChangeMonitor (simplified) ----
//
// CCCaster's ChangeMonitor is a generic observer-pattern watcher that
// tracks arbitrary memory addresses and fires callbacks on change. We
// only need three watchpoints (game mode, game state, round-start
// counter), so we inline a tiny equivalent: each frame we read the
// watched value, compare to the cached previous value, and call the
// matching handler on change.
//
// The watched values are:
//   - CC_GAME_MODE_ADDR (uint32_t) — game mode transitions drive the
//     FSM via gameModeChanged().
//   - CC_GAME_STATE_ADDR (uint32_t) — game state transitions (intro
//     done, etc.) drive gameStateChanged().
//   - AsmHacks::roundStartCounter (uint32_t) — incremented by the
//     detectRoundStart ASM hack when players can start moving. Drives
//     the InGame transition.
uint32_t g_prevGameMode    = 0xFFFFFFFF;
uint32_t g_prevGameState   = 0xFFFFFFFF;
uint32_t g_prevRoundStart  = 0xFFFFFFFF;

// ---- Controller state ----
caster::common::controller::ControllerMapping g_p1Mapping;
caster::common::controller::ControllerMapping g_p2Mapping;
SDL_Joystick* g_p1Joy = nullptr;
SDL_Joystick* g_p2Joy = nullptr;
bool g_mappingsLoaded = false;

// ---- Retry menu sync state ----
bool g_localRetryMenuIndexSent = false;

// ============================================================================
// Controller mapping loader
// ============================================================================

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

    // Initialize the SDL joystick subsystem before opening any device.
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) < 0) {
        caster::common::logger::err("dll_main: SDL_InitSubSystem(JOYSTICK) failed: {}", SDL_GetError());
    } else {
        caster::common::logger::info("dll_main: SDL joystick subsystem initialized ({} joysticks)",
                                     SDL_NumJoysticks());
    }

    if (g_p1Mapping.device_index >= 0) {
        g_p1Joy = SDL_JoystickOpen(g_p1Mapping.device_index);
        if (!g_p1Joy) {
            caster::common::logger::err("dll_main: SDL_JoystickOpen({}) failed for P1: {}",
                                        g_p1Mapping.device_index, SDL_GetError());
        }
    }
    if (g_p2Mapping.device_index >= 0) {
        g_p2Joy = SDL_JoystickOpen(g_p2Mapping.device_index);
        if (!g_p2Joy) {
            caster::common::logger::err("dll_main: SDL_JoystickOpen({}) failed for P2: {}",
                                        g_p2Mapping.device_index, SDL_GetError());
        }
    }

    g_mappingsLoaded = true;
    caster::common::logger::info("dll_main: mappings loaded (P1 device={} joy={}, P2 device={} joy={})",
                                 g_p1Mapping.device_index, (void*)g_p1Joy,
                                 g_p2Mapping.device_index, (void*)g_p2Joy);
}

// ============================================================================
// First-frame initialization
// ============================================================================

void doPostLoad() {
    if (g_postLoadDone) return;
    g_postLoadDone = true;

    caster::common::logger::info("dll_main: post-load initialization");
    caster::dll::dll_hacks::initializePostLoad();
    loadMappings();
    caster::common::logger::info("dll_main: post-load complete");
}

// ============================================================================
// IPC receive + forceGoto + NetplayConfig setup
// ============================================================================

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
    g_cfg = cfg;

    caster::common::logger::info("dll_main: config flags=0x{:02x} training={} netplay={}",
                                 cfg.flags, cfg.is_training(), cfg.is_netplay());

    // Derive local/remote player numbers and host flag.
    g_isNetplay = cfg.is_netplay();
    g_isHost    = cfg.is_host();
    if (g_isNetplay) {
        if (g_isHost) { g_localPlayer = 1; g_remotePlayer = 2; }
        else          { g_localPlayer = 2; g_remotePlayer = 1; }
    }

    // Populate NetplayManager config from the IPC config.
    // The IPC config is a flat struct optimized for the launcher→DLL
    // handoff; we translate it into the NetplayConfigMsg that the
    // NetplayManager uses internally (and that gets serialized over
    // the wire in spectate mode, which we don't have in v1).
    auto& nc = g_netMan.config;
    nc.mode.value = g_isHost ? caster::dll::ClientMode::Mode::Host
                              : caster::dll::ClientMode::Mode::Client;
    if (cfg.is_training()) {
        nc.mode.flags |= caster::dll::ClientMode::Training;
    }
    if (g_isHost) {
        nc.mode.flags |= caster::dll::ClientMode::GameStarted;  // host always started
    }
    nc.delay         = cfg.delay;
    nc.rollback      = cfg.rollback;
    nc.rollbackDelay = 0;  // F.5 will wire this
    nc.winCount      = cfg.win_count;
    nc.hostPlayer    = cfg.host_player;

    // Names aren't carried by the IPC config in v1 — the launcher's
    // display_name is used for the launcher UI but not synced to the DLL.
    // The DLL doesn't display names anywhere (no overlay), so leaving
    // these empty is fine.
    nc.names[0].clear();
    nc.names[1].clear();
    nc.sessionId.clear();

    if (g_isNetplay) {
        g_netMan.setRemotePlayer(g_remotePlayer);
    } else {
        // Offline: the "remote" player is just the local P2 (so setInput
        // for both players writes to the game). host_player stays as 1
        // (the human player).
        g_netMan.setRemotePlayer(g_remotePlayer);
    }

    caster::common::logger::info(
        "dll_main: netMan config — mode={} host={} training={} delay={} rollback={} "
        "winCount={} hostPlayer={} localPlayer={} remotePlayer={}",
        static_cast<int>(nc.mode.value), g_isHost, nc.mode.isTraining(),
        nc.delay, nc.rollback, nc.winCount, nc.hostPlayer,
        g_localPlayer, g_remotePlayer);

    // Start the netplay transport (no-op if offline). Done before forceGoto
    // so the ENet host is bound early — the peer may connect while we are
    // still navigating menus.
    caster::dll::netplay::start(cfg);

    // Apply forceGoto patch (training vs versus). The patch redirects
    // the mode-select screen's "what mode to enter" decision straight
    // to Training or Versus, bypassing the player having to navigate
    // the menu manually.
    uint8_t orig[4] = {0};
    std::memcpy(orig, (void*)0x42B475, 4);
    caster::common::logger::info("dll_main: original bytes at 0x42B475: {:02x} {:02x} {:02x} {:02x}",
                                 orig[0], orig[1], orig[2], orig[3]);

    uint32_t gameMode = *caster::dll::asU32(caster::dll::CC_GAME_MODE_ADDR);
    caster::common::logger::info("dll_main: game mode before patch: {} ({})",
                                 gameMode, caster::dll::gameModeStr(gameMode));

    if (cfg.is_training()) {
        caster::dll::asm_hacks::forceGotoTraining.write();
        caster::common::logger::info("dll_main: applied forceGotoTraining at 0x42B475");
    } else {
        caster::dll::asm_hacks::forceGotoVersus.write();
        caster::common::logger::info("dll_main: applied forceGotoVersus at 0x42B475");
    }

    uint8_t after[4] = {0};
    std::memcpy(after, (void*)0x42B475, 4);
    caster::common::logger::info("dll_main: bytes after patch: {:02x} {:02x} {:02x} {:02x}",
                                 after[0], after[1], after[2], after[3]);

    // Initial FSM state.
    g_netMan.setState(caster::dll::NetplayState::PreInitial);

    g_modePatchApplied = true;
}

// ============================================================================
// ChangeMonitor — drives FSM transitions from game-side state changes
// ============================================================================
//
// Mirrors CCCaster's DllMain::changedValue() callback (DllMain.cpp:1248).
// Each frame we read the three watched values and dispatch on change.
//
// The mappings game-mode → NetplayState are taken from CCCaster's
// DllMain::gameModeChanged() (DllMain.cpp:1112-1175).

void netplayStateChanged(caster::dll::NetplayState state);

void gameModeChanged(uint32_t previous, uint32_t current) {
    using namespace caster::dll;

    // Pre-game modes don't trigger a state change — we stay in PreInitial
    // or Initial until the game reaches CharaSelect / Loading / InGame.
    if (current == 0 ||
        current == CC_GAME_MODE_STARTUP ||
        current == CC_GAME_MODE_OPENING ||
        current == CC_GAME_MODE_TITLE ||
        current == CC_GAME_MODE_MAIN ||
        current == CC_GAME_MODE_LOADING_DEMO ||
        (previous == CC_GAME_MODE_LOADING_DEMO && current == CC_GAME_MODE_IN_GAME) ||
        current == CC_GAME_MODE_HIGH_SCORES) {
        return;
    }

    if (current == CC_GAME_MODE_CHARA_SELECT) {
        netplayStateChanged(NetplayState::CharaSelect);
        return;
    }
    if (current == CC_GAME_MODE_LOADING) {
        netplayStateChanged(NetplayState::Loading);
        return;
    }
    if (current == CC_GAME_MODE_IN_GAME) {
        // Versus mode in-game starts with character intros; training
        // mode goes straight to InGame.
        if (g_netMan.config.mode.isVersus()) {
            netplayStateChanged(NetplayState::CharaIntro);
        } else {
            netplayStateChanged(NetplayState::InGame);
        }
        return;
    }
    if (current == CC_GAME_MODE_RETRY) {
        netplayStateChanged(NetplayState::RetryMenu);
        return;
    }
    if (current == CC_GAME_MODE_REPLAY) {
        netplayStateChanged(NetplayState::ReplayMenu);
        return;
    }

    caster::common::logger::warn("dll_main: unhandled gameMode {} -> {}",
                                 previous, current);
}

void gameStateChanged(uint32_t /*previous*/, uint32_t current) {
    // CC_GAME_STATE_INTRO_DONE is the signal that the pre-game intro
    // cinematic has finished and the round is about to start. We don't
    // drive a state transition off it directly (roundStartCounter does
    // that), but it's a useful diagnostic.
    if (current == caster::dll::CC_GAME_STATE_INTRO_DONE) {
        caster::common::logger::info("dll_main: gameState INTRO_DONE");
    }
}

void netplayStateChanged(caster::dll::NetplayState state) {
    using namespace caster::dll;

    if (!g_netMan.isValidNext(state)) {
        caster::common::logger::err("dll_main: invalid FSM transition {} -> {}",
                                    netplayStateStr(g_netMan.getState()),
                                    netplayStateStr(state));
        return;
    }

    caster::common::logger::info("dll_main: FSM {} -> {} (idx={})",
                                 netplayStateStr(g_netMan.getState()),
                                 netplayStateStr(state),
                                 g_netMan.getIndex());

    // Entering RetryMenu — reset the "have we sent our local retry menu
    // index" flag (the player needs to select a new option).
    if (state == NetplayState::RetryMenu) {
        g_localRetryMenuIndexSent = false;
    }

    // Apply the state change. setState() handles all the bookkeeping
    // (incrementing _indexedFrame.parts.index, resetting _startWorldTime,
    // garbage-collecting old transition indices on Loading, etc.).
    g_netMan.setState(state);

    // Announce our new transition index to the peer. The peer uses this
    // to advance its remoteIndex (its view of where we are in the FSM),
    // which gates isRemoteInputReady().
    if (g_isNetplay && caster::dll::netplay::connected()) {
        caster::dll::netplay::sendTransitionIndex(g_netMan.getIndex());
    }
}

// ============================================================================
// Netplay inbox drain — peer → NetplayManager
// ============================================================================

void drainNetplayInbox() {
    if (!g_isNetplay) return;

    // PlayerInputs — the high-frequency per-frame input batch from the
    // remote player. Each batch covers NUM_INPUTS frames ending at the
    // peer's current indexedFrame. setInputs stores them with divergence
    // detection (the rollback trigger).
    while (auto pi = caster::dll::netplay::recvPlayerInputs()) {
        g_netMan.setInputs(g_remotePlayer, *pi);
    }

    // TransitionIndex — the peer's announcement that its FSM advanced.
    // setRemoteIndex pre-allocates space in the remote player's
    // InputsContainer so future setInputs calls don't have to resize.
    while (auto idx = caster::dll::netplay::recvTransitionIndex()) {
        g_netMan.setRemoteIndex(*idx);
    }

    // MenuIndex — the peer's retry-menu selection. setRemoteRetryMenuIndex
    // stores it; getRetryMenuInput will then auto-navigate once both
    // sides have selected.
    while (auto mi = caster::dll::netplay::recvMenuIndex()) {
        g_netMan.setRemoteRetryMenuIndex(mi->menuIndex);
    }

    // RngState — the host's RNG snapshot, sent at the start of each
    // round. The client applies it via process_manager::setRngState so
    // both sides start the round with identical RNG (preventing desync
    // in things like crit, dust hitboxes, etc.).
    while (auto rs = caster::dll::netplay::recvRngState()) {
        g_netMan.setRngState(*rs);
    }
}

// ============================================================================
// Per-frame logic
// ============================================================================

void frameStep() {
    using namespace caster::dll;

    doPostLoad();
    doIpcAndModePatch();

    // 1. Refresh the indexed frame from the world timer.
    g_netMan.updateFrame();

    // 2. Poll ENet and drain the inbox into the NetplayManager.
    netplay::poll();
    drainNetplayInbox();

    // 3. ChangeMonitor — check the three watched values and dispatch
    //    state transitions on change. Done AFTER updateFrame + inbox
    //    drain so the FSM sees the freshest remote state when deciding
    //    whether to advance.
    const uint32_t gameMode   = *asU32(CC_GAME_MODE_ADDR);
    const uint32_t gameState  = *asU32(CC_GAME_STATE_ADDR);
    const uint32_t roundStart = asm_hacks::roundStartCounter;

    if (gameMode != g_prevGameMode) {
        if (g_prevGameMode != 0xFFFFFFFF) {  // skip the initial bogus read
            gameModeChanged(g_prevGameMode, gameMode);
        }
        g_prevGameMode = gameMode;
    }
    if (gameState != g_prevGameState) {
        if (g_prevGameState != 0xFFFFFFFF) {
            gameStateChanged(g_prevGameState, gameState);
        }
        g_prevGameState = gameState;
    }
    if (roundStart != g_prevRoundStart) {
        if (g_prevRoundStart != 0xFFFFFFFF) {
            // roundStartCounter incremented — players can move now.
            // Drive the InGame transition (CharaIntro → InGame, or
            // Skippable → InGame for round 2+).
            caster::common::logger::info("dll_main: roundStart {} -> {}",
                                         g_prevRoundStart, roundStart);
            netplayStateChanged(NetplayState::InGame);
        }
        g_prevRoundStart = roundStart;
    }

    // 4. Skip rendering during PreInitial/Initial/AutoCharaSelect so
    //    the Startup/Opening/Title screens whip by in milliseconds.
    //    Mirrors CCCaster's frameStepNormal() (DllMain.cpp:196-201).
    //    This also bypasses the FPS limiter (limitFPS() checks
    //    CC_SKIP_FRAMES_ADDR).
    const NetplayState state = g_netMan.getState();
    if (state == NetplayState::PreInitial ||
        state == NetplayState::Initial ||
        state == NetplayState::AutoCharaSelect) {
        *asU32(CC_SKIP_FRAMES_ADDR) = 1;
    } else {
        *asU32(CC_SKIP_FRAMES_ADDR) = 0;
    }

    // 5. Read the local controller and feed it into the NetplayManager.
    //    Only do this in states where the local player actually has
    //    control (CharaSelect, InGame, RetryMenu, Skippable, ReplayMenu).
    //    In PreInitial/Initial/AutoCharaSelect/Loading/CharaIntro the
    //    FSM synthesizes its own inputs (mash Confirm, etc.) and the
    //    local controller is ignored.
    if (state == NetplayState::CharaSelect ||
        state == NetplayState::InGame ||
        state == NetplayState::RetryMenu ||
        state == NetplayState::Skippable ||
        state == NetplayState::ReplayMenu) {
        GameInput input;
        if (g_p1Mapping.device_index >= 0 && g_p1Joy) {
            input = read_local_input(g_p1Joy, g_p1Mapping);
        } else if (g_p1Mapping.device_index < 0) {
            input = read_local_input(nullptr, g_p1Mapping);
        }

        // Combine direction + buttons into the packed uint16_t format
        // the InputsContainer uses (matches CCCaster's COMBINE_INPUT).
        const uint16_t combined = combine_input(input);

        // Store the local player's input for the current frame. This
        // also marks it as "due" — getInput(localPlayer) will return
        // it (possibly filtered by the FSM).
        g_netMan.setInput(g_localPlayer, combined);

        // 6. (Netplay only) Send the local player's input batch to the
        //    peer. The peer will setInputs() it into their remote
        //    container and apply it via getInput(remotePlayer).
        if (g_isNetplay && netplay::connected()) {
            auto pi = g_netMan.getInputs(g_localPlayer);
            if (pi) {
                netplay::sendPlayerInputs(*pi);
            }

            // Retry menu sync: send our local retry menu index once
            // (when the player confirms a selection). The peer will
            // auto-navigate to match once both sides have selected.
            if (state == NetplayState::RetryMenu && !g_localRetryMenuIndexSent) {
                auto mi = g_netMan.getLocalRetryMenuIndex();
                if (mi) {
                    netplay::sendMenuIndex(*mi);
                    g_localRetryMenuIndexSent = true;
                }
            }
        }
    }

    // 7. Write both players' inputs to the game. The FSM decides what
    //    each player's input should be (synthesized for menu states,
    //    real controller for gameplay states, predicted last-known for
    //    remote player when their input hasn't arrived yet — which is
    //    what the InputsContainer.get() returns via lastInputBefore).
    const uint16_t localInput  = g_netMan.getInput(g_localPlayer);
    const uint16_t remoteInput = g_netMan.getInput(g_remotePlayer);

    // Unpack the combined uint16_t back into the split direction/buttons
    // format that writeGameInput expects.
    auto unpack = [](uint16_t combined) -> GameInput {
        return { static_cast<uint16_t>(combined & 0x000F),
                 static_cast<uint16_t>((combined & 0xFFF0) >> 4) };
    };

    const GameInput li = unpack(localInput);
    const GameInput ri = unpack(remoteInput);

    process_manager::writeGameInput(g_localPlayer,  li.direction, li.buttons);
    process_manager::writeGameInput(g_remotePlayer, ri.direction, ri.buttons);
}

} // namespace

// ============================================================================
// callback() — per-frame hook entry point
// ============================================================================

extern "C" void callback() {
    if (!g_running.load()) return;
    try {
        frameStep();
    } catch (...) {
        caster::common::logger::err("dll_main: exception in callback()");
    }
}

// ============================================================================
// stopDllMain() — called on fatal error / Alt+F4
// ============================================================================

namespace caster::dll::dll_hacks {
void stopDllMain(const std::string& error) {
    caster::common::logger::info("dll_main: stopDllMain('{}')", error);
    g_running.store(false);
}
} // namespace caster::dll::dll_hacks

// ============================================================================
// D3DHook callbacks (global namespace)
// ============================================================================

void PresentFrameBegin(IDirect3DDevice9*) {}
void EndScene(IDirect3DDevice9*) {}
void InvalidateDeviceObjects() {}
void PresentFrameEnd(IDirect3DDevice9* device) {
    caster::dll::frame_rate::PresentFrameEnd(device);
}

// ============================================================================
// DllMain
// ============================================================================

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

            caster::dll::netplay::shutdown();

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
