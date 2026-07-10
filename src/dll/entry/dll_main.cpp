// src/dll/entry/dll_main.cpp
//
// Entry point for hook.dll. Follows CCCaster's synchronous approach:
// - DllMain: initializePreLoad() only (fast, no blocking)
// - callback() (first frame): initializePostLoad() + IPC receive + forceGoto
// - callback() (every frame): ChangeMonitor → frameStep → writeGameInput
//
// No extra threads. Everything runs on the game's main thread via callback().
//
// F.3 wiring (see docs/port-status.md):
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

#include "game/addresses.hpp"
#include "hooks/asm_patches.hpp"
#include "lifecycle.hpp"
#include "hooks/frame_limiter.hpp"
#include "game/game_io.hpp"
#include "input/input_reader.hpp"
#include "ipc/receiver.hpp"
#include "netplay/connector.hpp"
#include "netplay/manager.hpp"
#include "netplay/rollback_manager.hpp"
#include "netplay/debug_log.hpp"
#include "../common/ipc/pipe_name.hpp"
#include "../common/logger.hpp"
#include "../common/controller/mapping.hpp"
#include "../common/ipc/config_buffer.hpp"
#include "../common/win32/env.hpp"

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
#include <cstdlib>
#include <list>

namespace {

// ============================================================================
// Global state
// ============================================================================

std::atomic<bool> g_running{false};
bool g_postLoadDone = false;
bool g_ipcDone = false;
bool g_modePatchApplied = false;

// ---- Auto-input mode (for automated testing) ----
//
// When CASTER_AUTO_INPUT=1 is set in the environment, the DLL ignores the
// real controller and generates synthetic input instead: a mash of the
// CONFIRM button (3 frames on, 3 frames off) with neutral direction. This
// lets us run automated netplay tests without a human player — the script
// will confirm through chara-select, start the match, and mash during the
// round.
//
// CONFIRM is used (not CC_BUTTON_A) because the chara-select screen
// requires CC_BUTTON_CONFIRM to advance. In-game, CONFIRM also works as
// an attack button.
bool g_autoInput = false;
uint32_t g_autoInputFrame = 0;

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

// lazyDisconnect — set when entering RetryMenu in netplay mode.
//
// If the peer's socket drops during the RetryMenu state, we don't
// immediately stop the DLL — we wait until the local player has selected
// their retry option (g_localRetryMenuIndexSent becomes true, or rather
// getLocalRetryMenuIndex() returns non-null). This avoids abandoning the
// match mid-selection, which would leave the player staring at a frozen
// retry menu.
//
// When we eventually leave RetryMenu (transition to Loading for rematch,
// or CharaSelect for chara change), if lazyDisconnect is still set and
// the socket is still down, we delayedStop("Disconnected!"). If the
// socket came back, we clear the flag and continue normally.
//
// Matches CCCaster DllMain.cpp:1086-1102.
bool g_lazyDisconnect = false;

// ---- RNG sync state ----
//
// shouldSyncRngState is set whenever we transition into CharaSelect or
// InGame (the moments where the RNG needs to be deterministic between
// peers). The host generates a RngState from the game's RNG addresses
// and sends it to the client; the client waits for it (via
// isRngStateReady) before advancing the frame, then applies it via
// process_manager::setRngState.
//
// This prevents desyncs caused by RNG diverging between host and client
// (which would happen if the client's RNG was seeded differently, or
// if the host's RNG was advanced by an input the client didn't see).
bool g_shouldSyncRngState = false;

// ---- SyncHash / desync detection ----
//
// We maintain two FIFO lists: local SyncHashes we've generated and
// remote SyncHashes we've received from the peer. Each frame we pop
// matching pairs (same indexedFrame) and compare. A mismatch means
// the game state has diverged — we trigger delayedStop("Desync!").
//
// SyncHashes are generated every 5*60 frames (5 seconds) or every 150
// frames (2.5 seconds), whichever comes first — matches CCCaster's
// DllMain.cpp:776 schedule. We skip generation during rollback and
// during the first frame of each transition (frame 0) to avoid
// comparing against states that are about to be rolled back.
std::list<caster::dll::SyncHash> g_localSync;
std::list<caster::dll::SyncHash> g_remoteSync;

// ---- Delayed stop ----
//
// delayedStop is the "soft shutdown" path: instead of immediately
// killing the DLL, we set g_running=false so the next callback() returns
// early. This gives pending ENet packets (like the ErrorMessage we'd
// want to send) time to flush, and matches CCCaster's
// DllMain::delayedStop semantics (without the timer — we just stop).
void delayedStop(const std::string& error);

// ---- Rollback state ----
//
// g_rollMan owns the memory pool of saved game states. Allocated on
// entering InGame (when rollback is enabled), deallocated on leaving.
caster::dll::RollbackManager g_rollMan;

// rollbackTimer counts down from minRollbackSpacing to 0. We only
// allow a new rollback when rollbackTimer == minRollbackSpacing (i.e.
// the timer has fully reset). After a rollback, --rollbackTimer starts
// the cooldown. This prevents back-to-back rollbacks from thrashing
// the game state.
//
// minRollbackSpacing is clamped to [2, 4] based on the rollback window
// (larger window = more spacing needed). Matches CCCaster DllMain.cpp:169.
int g_rollbackTimer = 0;
uint8_t g_minRollbackSpacing = 2;

// roundOverTimer delays the InGame → Skippable transition when rollback
// is enabled. When both players' NO_INPUT_FLAG is set (round over), we
// don't immediately transition — we wait rollback+5 frames to give the
// rollback system time to correct any misprediction at the round boundary.
// Matches CCCaster DllMain.cpp:1200-1245 (ROLLBACK_ROUND_OVER_DELAY=5).
int g_roundOverTimer = -1;

// fastFwdStopFrame is set when we enter rollback rerun mode. When
// non-zero, frameStep calls frameStepRerun instead of the normal path.
// frameStepRerun skips rendering (CC_SKIP_FRAMES_ADDR=1) and stops when
// the indexed frame reaches fastFwdStopFrame.
caster::dll::IndexedFrame g_fastFwdStopFrame = {{0, 0}};

// ---- Resend timer / wait-inputs timeout (sanity #4) ----
//
// When isRemoteInputReady() returns false (we're waiting for the peer's
// input for the current frame), we:
//   - Re-send our last PlayerInputs every ~100ms. The peer may have
//     dropped our previous packet (UNRELIABLE); re-sending ensures they
//     eventually get it.
//   - Give up after 10s and delayedStop("Timed out!"). This catches
//     the case where the peer has crashed or disconnected silently.
//
// IMPORTANT: we use wall-clock time (GetTickCount), NOT frame count.
// During PreInitial the game runs with CC_SKIP_FRAMES_ADDR=1 (render-skip),
// which makes the frame loop run at thousands of FPS instead of 60.
// Frame-based counters would fire in milliseconds.
//
// Matches CCCaster DllMain.cpp:47-50 + 574-579 + 1941-1946.
inline constexpr std::uint32_t RESEND_INPUTS_INTERVAL_MS = 100;    // ~100ms
inline constexpr std::uint32_t MAX_WAIT_INPUTS_INTERVAL_MS = 10000; // 10 seconds

// Spin-lock poll sleep. Mirrors CCCaster's POLL_TIMEOUT (DllMain.cpp:35):
// the number of milliseconds to Sleep() between spin-lock iterations
// while waiting for remote inputs/RngState. Keeps CPU usage sane and
// yields to the OS so ENet can deliver packets.
inline constexpr std::uint32_t POLL_TIMEOUT_MS = 3;  // 3ms

std::uint32_t g_resendLastTick = 0;   // GetTickCount() of last resend
std::uint32_t g_waitStartTick  = 0;   // GetTickCount() when we started waiting

// ---- Initial connect timeout (sanity #7) ----
//
// If the peer never connects within 60 seconds of netplay::start(), we
// delayedStop. Without this, a misconfigured peer address or firewall
// would leave the player staring at a frozen game forever.
//
// IMPORTANT: we use wall-clock time (GetTickCount), NOT frame count.
// During PreInitial the game runs with CC_SKIP_FRAMES_ADDR=1 (render-skip),
// which makes the frame loop run at thousands of FPS instead of 60.
// A frame-based counter would fire the timeout in milliseconds.
//
// Matches CCCaster DllMain.cpp:41 (INITIAL_CONNECT_TIMEOUT = 60000).
inline constexpr std::uint32_t INITIAL_CONNECT_TIMEOUT_MS = 60000;  // 60 seconds
std::uint32_t g_initialConnectStartTick = 0;  // GetTickCount() at first frameStep
bool g_initialConnectDone = false;

// ---- Delayed stop implementation ----
void delayedStop(const std::string& error) {
    if (!error.empty()) {
        caster::common::logger::err("dll_main: delayedStop — {}", error);
    }
    g_running.store(false);
}

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

    // Check for auto-input mode (automated testing).
    if (caster::common::win32::env::get("CASTER_AUTO_INPUT") == "1") {
        g_autoInput = true;
        caster::common::logger::info("dll_main: AUTO-INPUT mode active (mash CONFIRM)");
    }

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
        // Initialize the structured netplay debug logger (separate file
        // for host vs joiner so logs don't interleave).
        caster::dll::netplay_debug::init(g_isHost);
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
    // When rollback is enabled, rollbackDelay is the effective input delay
    // used during rollback rerun (setInput writes to frame + rollbackDelay).
    // In CCCaster, the host's UI prompt sets this to the same value as delay
    // when rollback > 0 (MainUi.cpp:1859). Since ReCaster always uses rollback
    // (default 4), the negotiated `delay` IS the rollback delay.
    nc.rollbackDelay = (cfg.rollback > 0) ? cfg.delay : 0;
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

    // Set rollback spacing based on the rollback window. Larger windows
    // need more spacing to prevent thrashing. Clamped to [2, 4].
    // Matches CCCaster DllMain.cpp:1886.
    if (nc.rollback >= 2) {
        g_minRollbackSpacing = (nc.rollback < 4) ? nc.rollback : 4;
    } else {
        g_minRollbackSpacing = 2;
    }
    g_rollbackTimer = g_minRollbackSpacing;

    // Rollback-specific game hacks (sanity-check fix #2 + #6).
    //
    // hijackIntroState: NOP out the game's auto-write to CC_INTRO_STATE_ADDR
    //   so we can control it manually during rollback rerun (fix #3 below
    //   in frameStep). Without this, the game's write would overwrite our
    //   forced 0, causing the intro cinematic to re-execute during rerun.
    //
    // CC_STAGE_ANIMATION_OFF_ADDR = 1: stage animations (background
    //   animals, etc.) are stateful and can diverge between host and
    //   client during rollback. Disabling them avoids visual desync.
    //
    // Matches CCCaster DllMain.cpp:1896-1907.
    if (nc.rollback > 0) {
        caster::dll::asm_hacks::hijackIntroState.write();
        *caster::dll::asU8(caster::dll::CC_STAGE_ANIMATION_OFF_ADDR) = 1;
        caster::common::logger::info("dll_main: applied rollback hacks (hijackIntroState + stageAnimOff)");
    }

    // Match config — set damage level, timer speed, win count (sanity fix #5).
    //
    // These are the game's match-config addresses. The game has defaults
    // (damage=2, timer=2, winCount=2) but if either side has edited
    // System/_App.ini they could be different — which would cause the
    // two sides to play with different rules. CCCaster sets them
    // explicitly from the NetplayConfig to guarantee both sides match.
    //
    // Matches CCCaster DllMain.cpp:1889-1891.
    *caster::dll::asU32(caster::dll::CC_DAMAGE_LEVEL_ADDR) = 2;
    *caster::dll::asU32(caster::dll::CC_TIMER_SPEED_ADDR)  = 2;
    *caster::dll::asU32(caster::dll::CC_WIN_COUNT_VS_ADDR) =
        (nc.winCount ? nc.winCount : 2);
    caster::common::logger::info("dll_main: match config — damage=2 timer=2 winCount={}",
                                 nc.winCount ? nc.winCount : 2);

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

    // Initial FSM state — only set if not already set (avoid invalid
    // PreInitial → PreInitial transition).
    if (g_netMan.getState() != caster::dll::NetplayState::PreInitial) {
        g_netMan.setState(caster::dll::NetplayState::PreInitial);
    }

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
    // index" flag (the player needs to select a new option) and arm the
    // lazyDisconnect flag (if the socket drops during RetryMenu, we
    // don't stop until the player has selected).
    if (state == NetplayState::RetryMenu) {
        g_localRetryMenuIndexSent = false;
        if (g_isNetplay) {
            g_lazyDisconnect = true;
            caster::common::logger::info("dll_main: armed lazyDisconnect for RetryMenu");
        }
    } else if (g_lazyDisconnect) {
        // Leaving RetryMenu (or any state where lazyDisconnect was armed)
        // — clear the flag. If the socket is already down, stop now.
        g_lazyDisconnect = false;
        if (g_isNetplay && !caster::dll::netplay::connected()) {
            caster::common::logger::info("dll_main: lazyDisconnect triggered on state change");
            delayedStop("Disconnected!");
            return;
        }
    }

    // Entering CharaSelect OR entering InGame — enable RNG sync. The
    // host will generate a RngState and send it to the client on the
    // next frame; the client will wait for it (via isRngStateReady)
    // before advancing.
    if ((state == NetplayState::CharaSelect || state == NetplayState::InGame)
        && g_isNetplay) {
        caster::common::logger::info("dll_main: enabling RNG sync for {}",
                                     netplayStateStr(state));
        g_shouldSyncRngState = true;
    }

    // Entering InGame — allocate rollback state pool.
    // Leaving InGame — deallocate (frees the ~14MB memory pool).
    if (state == NetplayState::InGame && g_netMan.getRollback()) {
        g_rollMan.allocateStates();
    }
    if (g_netMan.getState() == NetplayState::InGame && g_netMan.getRollback()) {
        g_rollMan.deallocateStates();
    }

    // Apply the state change. setState() handles all the bookkeeping
    // (incrementing _indexedFrame.parts.index, resetting _startWorldTime,
    // garbage-collecting old transition indices on Loading, etc.).
    const auto prevState = g_netMan.getState();
    g_netMan.setState(state);

    // Log the state transition to the structured debug log.
    caster::dll::netplay_debug::log_event_str("state-transition",
        std::format("{}->{} idx={}", netplayStateStr(prevState),
                    netplayStateStr(state), g_netMan.getIndex()));

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
    // remote player.
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

    // SyncHash — periodic desync-detection snapshots. Stored in
    // g_remoteSync for comparison against locally-generated ones.
    while (auto sh = caster::dll::netplay::recvSyncHash()) {
        g_remoteSync.push_back(*sh);
    }
}

// ============================================================================
// checkRoundOver — detect end of round and drive InGame → Skippable
// ============================================================================
//
// Mirrors CCCaster's DllMain::checkRoundOver (DllMain.cpp:1200-1245).
// Each frame during InGame, we check if both players' NO_INPUT_FLAG is
// set — that's the game's signal that the round is over (one player's
// health reached 0 and the win pose has started).
//
// With rollback enabled, we don't transition immediately — we wait
// `rollback + 5` frames (ROLLBACK_ROUND_OVER_DELAY) to give the rollback
// system time to correct any misprediction at the round boundary. Without
// this delay, a rollback at the round-boundary frame could transition to
// Skippable prematurely, then roll back to InGame, causing FSM chaos.
//
// Without rollback (training mode, or rollback=0), we transition
// immediately.
//
// Puppet handling (P3/P4 for tag battles) is simplified for v1 — we
// only check P1/P2 directly. Tag battles are not in scope for v1.

void checkRoundOver() {
    using namespace caster::dll;

    // Check if both players' "no input" flag is set — the game sets
    // this when the round is over (health reached 0, win pose started).
    const bool p1_over = *asU8(CC_P1_NO_INPUT_FLAG_ADDR) != 0;
    const bool p2_over = *asU8(CC_P2_NO_INPUT_FLAG_ADDR) != 0;
    const bool isOver = p1_over && p2_over;

    if (g_netMan.getRollback()) {
        // Rollback mode — delayed transition.
        if (isOver) {
            if (g_roundOverTimer == 0) {
                // Timer expired — transition now.
                g_roundOverTimer = -1;
                netplayStateChanged(NetplayState::Skippable);
            } else if (g_roundOverTimer < 0) {
                // First detection — start the timer.
                g_roundOverTimer = g_netMan.getRollback() + 5;  // ROLLBACK_ROUND_OVER_DELAY
            }
        } else {
            // Round not over — reset timer.
            g_roundOverTimer = -1;
        }
    } else if (isOver) {
        // No rollback — transition immediately.
        netplayStateChanged(NetplayState::Skippable);
    }
}

// ============================================================================
// frameStepRerun — fast-forward mode during rollback re-run
// ============================================================================
//
// After a rollback, the game state has been restored to a past frame.
// We then need to re-run the simulation from that frame to the current
// frame, applying the corrected inputs. During this re-run:
//   - We skip rendering (CC_SKIP_FRAMES_ADDR=1) so the fast-forward
//     is invisible to the player.
//   - We DON'T save rollback states (the inputs are being replayed,
//     not generated fresh — saving them would be wasteful and could
//     confuse the next rollback).
//   - We stop when netMan.getIndexedFrame() reaches fastFwdStopFrame.
//
// SFX mute during rerun is NOT implemented in v1 (accepts audio glitch).
// See docs/port-status.md (blockers da Fase F).

void frameStepRerun() {
    using namespace caster::dll;

    // Check if we've reached the target frame.
    if (g_netMan.getIndexedFrame().value >= g_fastFwdStopFrame.value) {
        // Done — stop fast-forwarding.
        g_fastFwdStopFrame.value = 0;
        *asU32(CC_SKIP_FRAMES_ADDR) = 0;
        caster::common::logger::info("rollback: rerun complete at [idx={},frame={}]",
                                     g_netMan.getIndex(), g_netMan.getFrame());
        caster::dll::netplay_debug::log_event_str("rollback-rerun-done",
            std::format("idx={} frm={}", g_netMan.getIndex(), g_netMan.getFrame()));
    } else {
        // Still catching up — skip rendering.
        *asU32(CC_SKIP_FRAMES_ADDR) = 1;
    }
}

// ============================================================================
// Per-frame logic
// ============================================================================

void frameStep() {
    using namespace caster::dll;

    doPostLoad();
    doIpcAndModePatch();

    // 0. Initial connect timeout (sanity fix #7).
    //
    // If we're in netplay mode and the peer hasn't connected within
    // 60 seconds of netplay::start(), give up. We use wall-clock time
    // (GetTickCount) because during PreInitial the game runs with
    // CC_SKIP_FRAMES_ADDR=1 which makes the frame loop run at thousands
    // of FPS — a frame-based counter would fire in milliseconds.
    //
    // Once connected, g_initialConnectDone is set so we don't re-trigger.
    if (g_isNetplay && !g_initialConnectDone) {
        if (caster::dll::netplay::connected()) {
            g_initialConnectDone = true;
            caster::common::logger::info("dll_main: initial connect established");
        } else {
            if (g_initialConnectStartTick == 0) {
                g_initialConnectStartTick = GetTickCount();
            }
            std::uint32_t elapsed = GetTickCount() - g_initialConnectStartTick;
            if (elapsed >= INITIAL_CONNECT_TIMEOUT_MS) {
                delayedStop("Initial connect timeout — peer never connected");
                return;
            }
        }
    }

    // 1. Refresh the indexed frame from the world timer.
    g_netMan.updateFrame();

    // 1a. Clear lastChangedFrame BEFORE draining the inbox.
    //
    // This matches CCCaster's order (DllMain.cpp:537-538): clear happens
    // BEFORE the poll loop that receives new inputs. The clear wipes any
    // stale divergence from the previous frame's setInputs, so that only
    // NEW divergences (from inputs arriving in this frame's drain) are
    // visible to the rollback trigger check later.
    //
    // If clear is done AFTER the drain (as it was before this fix), the
    // freshly-detected divergence is immediately wiped, and the rollback
    // trigger never fires.
    if (g_rollbackTimer == g_minRollbackSpacing) {
        g_netMan.clearLastChangedFrame();
    }

    // 2. Poll ENet and drain the inbox into the NetplayManager.
    netplay::poll();
    drainNetplayInbox();

    // 2a. Detect initial peer connection → transition PreInitial → Initial.
    //
    // In CCCaster, this transition is fired by socketAccepted/socketConnected
    // callbacks (DllMain.cpp:1322, 1343). We don't have callbacks — instead
    // we poll for connected() each frame. Once the ENet peer connects, we
    // fire the transition once.
    //
    // For OFFLINE mode (training/versus), we transition immediately —
    // there's no peer to wait for. Matches CCCaster DllMain.cpp:1879
    // (ipcRead → NetplayConfig → netplayStateChanged(Initial) for offline).
    static bool g_initialTransitionDone = false;
    if (!g_initialTransitionDone && g_modePatchApplied) {
        if (!g_isNetplay || caster::dll::netplay::connected()) {
            netplayStateChanged(NetplayState::Initial);
            g_initialTransitionDone = true;
            caster::common::logger::info("dll_main: PreInitial → Initial (connected={})",
                                         caster::dll::netplay::connected());
        }
    }

    // 3. ChangeMonitor — check the three watched values and dispatch
    //    state transitions on change. Done AFTER updateFrame + inbox
    //    drain so the FSM sees the freshest remote state when deciding
    //    whether to advance.
    const uint32_t gameMode   = *asU32(CC_GAME_MODE_ADDR);
    const uint32_t gameState  = *asU32(CC_GAME_STATE_ADDR);
    const uint32_t roundStart = asm_hacks::roundStartCounter;

    // Debug: log gameMode every time it changes, plus the current FSM state
    // and the forceGoto patch status.
    static uint32_t g_lastLoggedMode = 0xFFFFFFFF;
    if (gameMode != g_lastLoggedMode) {
        // Check if the forceGoto patch at 0x42B475 is still intact.
        uint8_t patchBytes[2] = {0, 0};
        std::memcpy(patchBytes, (void*)0x42B475, 2);
        caster::common::logger::info(
            "dll_main: gameMode {} -> {} ({}) | FSM={} | forceGoto bytes={:02x} {:02x}",
            g_lastLoggedMode, gameMode, gameModeStr(gameMode),
            netplayStateStr(g_netMan.getState()),
            patchBytes[0], patchBytes[1]);
        g_lastLoggedMode = gameMode;
    }

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

    // Get current FSM state — used by all subsequent steps.
    const NetplayState state = g_netMan.getState();

    // 3-pre-a. checkRoundOver — detect end of round (InGame only).
    //
    // Runs EVERY frame, including during rollback rerun — exactly as
    // CCCaster does (DllMain.cpp:971). If the round ends mid-rerun, we
    // must still count down roundOverTimer so the Skippable transition
    // fires at the right time. Previously this was after the spin-lock
    // and skipped during rerun (early return), causing roundOverTimer
    // timing divergence.
    if (g_netMan.isInGame()) {
        checkRoundOver();
    }

    // 3-pre-b. Force intro state to 0 during rollback rerun.
    //
    // Runs EVERY frame, including during rerun — matches CCCaster
    // (DllMain.cpp:974-976). CC_INTRO_STATE_ADDR counts down 2→1→0
    // during the pre-game intro cinematic; during rerun we must keep
    // it at 0 or the game re-executes the cinematic.
    if (g_netMan.isInRollback()
        && g_netMan.getFrame() > caster::dll::CC_PRE_GAME_INTRO_FRAMES
        && *caster::dll::asU8(caster::dll::CC_INTRO_STATE_ADDR)) {
        *caster::dll::asU8(caster::dll::CC_INTRO_STATE_ADDR) = 0;
    }

    // 3-pre-c. Rollback rerun path.
    //
    // If we're in fast-forward mode (g_fastFwdStopFrame set), the game
    // state has been restored to a past frame and we're re-running the
    // simulation forward with corrected inputs. During rerun we:
    //   - Run frameStepRerun() to manage CC_SKIP_FRAMES + stop condition
    //   - Write both players' stored inputs to the game
    //   - SKIP setInput, sendPlayerInputs, the spin-lock gate, rollback
    //     save/trigger (those are frameStepNormal's job)
    //
    // Mirrors CCCaster's frameStep() structure (DllMain.cpp:957-997):
    // ChangeMonitor + checkRoundOver + intro-force run unconditionally,
    // THEN the rerun/normal branch, THEN writeGameInput.
    if (g_fastFwdStopFrame.value != 0) {
        frameStepRerun();

        const uint16_t localInput  = g_netMan.getInput(g_localPlayer);
        const uint16_t remoteInput = g_netMan.getInput(g_remotePlayer);
        auto unpack = [](uint16_t combined) -> GameInput {
            return { static_cast<uint16_t>(combined & 0x000F),
                     static_cast<uint16_t>((combined & 0xFFF0) >> 4) };
        };
        const GameInput li = unpack(localInput);
        const GameInput ri = unpack(remoteInput);
        process_manager::writeGameInput(g_localPlayer,  li.direction, li.buttons);
        process_manager::writeGameInput(g_remotePlayer, ri.direction, ri.buttons);
        return;
    }

    // 3a. CC_SKIP_FRAMES — MUST be set BEFORE the ready gate.
    //
    // During PreInitial/Initial/AutoCharaSelect, skip rendering so the
    // Startup/Opening/Title screens whip by in milliseconds. This also
    // bypasses the FPS limiter.
    //
    // For all other states (CharaSelect, InGame, etc.) set to 0 so the
    // game runs at normal 60fps. This is CRITICAL: if CC_SKIP_FRAMES
    // stays at 1 when we enter CharaSelect, the game runs at thousands
    // of FPS and the local frame counter runs away from the remote,
    // making isRemoteInputReady() permanently false.
    if (state == NetplayState::PreInitial ||
        state == NetplayState::Initial ||
        state == NetplayState::AutoCharaSelect) {
        *asU32(CC_SKIP_FRAMES_ADDR) = 1;

        // Re-apply forceGoto patch every frame during PreInitial/Initial.
        if (g_modePatchApplied) {
            if (g_cfg.is_training()) {
                caster::dll::asm_hacks::forceGotoTraining.write();
            } else {
                caster::dll::asm_hacks::forceGotoVersus.write();
            }
        } else {
            caster::dll::asm_hacks::forceGotoVersus.write();
        }
    } else {
        *asU32(CC_SKIP_FRAMES_ADDR) = 0;
    }

    // 3b. Read local controller + setInput + sendPlayerInputs.
    //
    // CRITICAL: This MUST happen BEFORE the ready gate.
    // The CCCaster frameStepNormal does setInput + send BEFORE the poll
    // loop (DllMain.cpp:469-507).
    if (state == NetplayState::CharaSelect ||
        state == NetplayState::InGame ||
        state == NetplayState::RetryMenu ||
        state == NetplayState::Skippable ||
        state == NetplayState::ReplayMenu) {
        GameInput input;
        if (g_autoInput) {
            // Auto-input: generate inputs to drive the game through menus
            // and matches without a human player.
            //
            // In menus (CharaSelect, RetryMenu, Skippable): mash CONFIRM
            // (3 frames on, 3 frames off) to confirm selections.
            //
            // In InGame: generate DIFFERENT inputs per player to force
            // rollback divergence. Host mashes A+B, joiner mashes C+D
            // with different directions. With simulated lag, the remote
            // input arrives late → prediction differs from reality →
            // rollback fires.
            input.direction = 0;
            input.buttons = 0;
            if (state == NetplayState::InGame) {
                // Different pattern per player to force divergence.
                if (g_isHost) {
                    // Host: alternate between neutral+A and forward+B
                    if ((g_autoInputFrame % 8) < 4) {
                        input.direction = 6;  // forward (right)
                        input.buttons = caster::dll::CC_BUTTON_A;
                    } else {
                        input.direction = 2;  // down
                        input.buttons = caster::dll::CC_BUTTON_B;
                    }
                } else {
                    // Joiner: different pattern — alternate back+C and up+D
                    if ((g_autoInputFrame % 7) < 3) {
                        input.direction = 4;  // back (left)
                        input.buttons = caster::dll::CC_BUTTON_C;
                    } else {
                        input.direction = 8;  // up
                        input.buttons = caster::dll::CC_BUTTON_D;
                    }
                }
            } else {
                // Menus: mash CONFIRM (3 on, 3 off).
                if ((g_autoInputFrame % 6) < 3) {
                    input.buttons = caster::dll::CC_BUTTON_CONFIRM;
                }
            }
            ++g_autoInputFrame;
        } else if (g_p1Mapping.device_index >= 0 && g_p1Joy) {
            input = read_local_input(g_p1Joy, g_p1Mapping);
        } else if (g_p1Mapping.device_index < 0) {
            input = read_local_input(nullptr, g_p1Mapping);
        }

        const uint16_t combined = combine_input(input);
        g_netMan.setInput(g_localPlayer, combined);

        // (Netplay only) Send messages to the peer.
        if (g_isNetplay) {
            if (state == NetplayState::RetryMenu) {
                auto mi = g_netMan.getLocalRetryMenuIndex();
                if (mi && !caster::dll::netplay::connected()) {
                    if (g_lazyDisconnect) {
                        g_lazyDisconnect = false;
                        delayedStop("Disconnected!");
                        return;
                    }
                    return;
                }
                if (mi && !g_localRetryMenuIndexSent) {
                    caster::dll::netplay::sendMenuIndex(*mi);
                    g_localRetryMenuIndexSent = true;
                }
            } else if (caster::dll::netplay::connected()) {
                auto pi = g_netMan.getInputs(g_localPlayer);
                if (pi) {
                    caster::dll::netplay::sendPlayerInputs(*pi);
                }
            }
        }
    }

    // 3b-bis. Host: generate + send RngState — MUST be BEFORE the gate.
    //
    // The host generates the RngState and sends it to the client. The
    // client's isRngStateReady() check in the gate depends on receiving
    // this RngState. If we put this AFTER the gate, the client blocks
    // forever waiting for the RngState that the host never sends
    // (because the host is also blocked on the gate).
    //
    // Matches CCCaster DllMain.cpp:514-527 (inside frameStepNormal,
    // BEFORE the poll loop at line 540).
    if (g_isNetplay && g_isHost && g_shouldSyncRngState) {
        caster::dll::RngState rs = caster::dll::process_manager::getRngState(
            g_netMan.getIndex());
        g_netMan.setRngState(rs);
        caster::dll::netplay::sendRngState(rs);
        caster::common::logger::info(
            "dll_main: host sent RngState for index {}", g_netMan.getIndex());
    }

    // 3c. Spin-lock ready gate (netplay only): isRngStateReady + isRemoteInputReady.
    //
    // Mirrors CCCaster's frameStepNormal poll loop (DllMain.cpp:540-581).
    // BLOCKS the game's main thread until BOTH conditions are true:
    //
    //   - isRngStateReady: the client has received the host's RngState
    //     for the current transition index. Without it, the client's RNG
    //     would diverge from the host's (different seeds → different
    //     crit/dust/knife outcomes → desync).
    //
    //   - isRemoteInputReady: we have at least one remote input at or
    //     beyond our current (index, frame). Without this, both peers
    //     apply the same inputs at different frames → immediate desync
    //     (no rollback exists in CharSelect to correct it).
    //
    // This is the critical fix for the CharSelect desync: previously the
    // gate was non-blocking (a single poll with timeout=0, then a flag).
    // That let the frame advance before the remote input arrived, so the
    // two sides applied inputs at different frames and diverged the
    // moment a button was pressed. Now we BLOCK — pausing the game's
    // simulation until the peer's input is in hand — exactly as CCCaster
    // does. Both peers advance in lockstep.
    //
    // Architecture: ReCaster is single-threaded. netplay::poll() services
    // ENet on this same thread (no background network thread), so calling
    // it inside the spin-loop IS what receives packets. The Sleep(POLL_TIMEOUT_MS)
    // yields to the OS so ENet's internal delivery can progress.
    //
    // While waiting, we re-send our last PlayerInputs every ~100ms
    // (RESEND_INPUTS_INTERVAL_MS) in case the peer dropped our previous
    // packet (UNRELIABLE). After 10s (MAX_WAIT_INPUTS_INTERVAL_MS) we
    // delayedStop("Timed out!") — peer is gone.
    uint32_t spin_ms = 0;
    if (g_isNetplay) {
        const uint32_t spin_start = GetTickCount();
        bool first_poll = true;
        for (;;) {
            // Service the socket: drain ENet events into the inbox queues,
            // then drain the inboxes into the NetplayManager. Both must
            // run each iteration so isRemoteInputReady/isRngStateReady
            // see the freshest remote state.
            caster::dll::netplay::poll();
            drainNetplayInbox();

            // Ready to advance?
            const bool rngReady   = g_netMan.isRngStateReady(g_shouldSyncRngState);
            const bool inputReady = g_netMan.isRemoteInputReady();
            if (rngReady && inputReady) {
                // Reset wait timers — we're back in lockstep.
                g_waitStartTick = 0;
                g_resendLastTick = 0;
                break;
            }

            // Still waiting — use wall-clock for resend + timeout (NOT
            // frame count: while blocked the world timer doesn't advance,
            // but even if it did, we want real elapsed time here).
            std::uint32_t now = GetTickCount();
            if (g_waitStartTick == 0) {
                g_waitStartTick = now;
                g_resendLastTick = now;
            }

            // Re-send our last PlayerInputs periodically (ENet is unreliable;
            // a dropped packet must eventually be re-sent).
            if ((now - g_resendLastTick) >= RESEND_INPUTS_INTERVAL_MS) {
                g_resendLastTick = now;
                if (caster::dll::netplay::connected()) {
                    auto pi = g_netMan.getInputs(g_localPlayer);
                    if (pi) {
                        caster::dll::netplay::sendPlayerInputs(*pi);
                    }
                }
            }

            // Timeout — peer is gone or severely lagging.
            if ((now - g_waitStartTick) >= MAX_WAIT_INPUTS_INTERVAL_MS) {
                delayedStop("Timed out!");
                return;
            }

            // Yield to the OS so ENet can deliver packets without us
            // burning 100% CPU. On the first poll iteration, use a
            // very short sleep (1ms) — the remote input is likely
            // already in the ENet buffer and just needs one more poll
            // to be delivered. Only do longer sleeps (3ms) if we've
            // been waiting for a while.
            if (first_poll) {
                Sleep(1);
                first_poll = false;
            } else {
                Sleep(POLL_TIMEOUT_MS);
            }
        }
        spin_ms = GetTickCount() - spin_start;
        if (spin_ms > 10) {
            caster::dll::netplay_debug::log_event("spin-block",
                "ms", spin_ms, "state", netplayStateStr(g_netMan.getState()),
                "idx", g_netMan.getIndex(), "frame", g_netMan.getFrame());
        }
    }



    // Steps below run only after the spin-lock has confirmed readiness
    // (or in offline mode, where there's no gate).

    // 3c-1. Apply pending RngState (netplay client only).
    //
    // If shouldSyncRngState is set and we have a RngState for the
    // current index, apply it to the game's RNG addresses. The host
    // generates; the client applies. This is the "consume" half of
    // the shouldSyncRngState flag — the "generate" half is in step 8
    // below for the host.
    if (g_shouldSyncRngState) {
        auto rngState = g_netMan.getRngState();
        if (rngState) {
            // Only apply on the client side — the host already has
            // this RNG in its own memory (it generated it). Applying
            // it on the host would be a no-op but also a waste.
            if (!g_isHost) {
                process_manager::setRngState(*rngState);
                caster::common::logger::info(
                    "dll_main: applied RngState for index {}",
                    g_netMan.getIndex());
            }
            g_shouldSyncRngState = false;
        }
    }

    // 3e. Rollback timer countdown.
    //
    // The rollback timer enforces a minimum spacing between rollbacks
    // (prevents thrashing). It counts down from minRollbackSpacing;
    // when it reaches 0, we're allowed to rollback again.
    //
    // NOTE: clearLastChangedFrame was previously done here (when timer
    // == full), but that wiped the divergence detected by drainNetplayInbox
    // before the trigger check could see it. The clear is now done at
    // the top of frameStep (step 1a), BEFORE the drain — matching
    // CCCaster's order (DllMain.cpp:537-538 before the poll loop, 583
    // countdown after).
    if (g_rollbackTimer < g_minRollbackSpacing) {
        --g_rollbackTimer;
        if (g_rollbackTimer < 0)
            g_rollbackTimer = g_minRollbackSpacing;
    }

    // 7. Write both players' inputs to the game. Runs once per frame,
    //    after the spin-lock has confirmed both remote inputs and (if
    //    needed) the host's RngState are available. getInput() returns
    //    the stored input for the current frame, or the last-known input
    //    via lastInputBefore as a prediction when the exact frame's input
    //    is beyond what we have (the rollback path corrects any
    //    misprediction during InGame).
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
    
    // 7b. Rollback: save state + trigger on divergence.
    //
    // Two things happen here:
    //
    // (a) Save state: if we're InGame with rollback enabled, save the
    //     current game state + NetplayManager state into the rollback
    //     pool. This happens EVERY frame (the pool is a ring buffer of
    //     NUM_ROLLBACK_STATES states). Also decrement roundOverTimer
    //     (used by checkRoundOver's delayed transition).
    //
    // (b) Trigger rollback: if the remote input diverged from our
    //     prediction (getLastChangedFrame < getIndexedFrame), and the
    //     rollback timer has reset (we haven't rolled back too
    //     recently), load the saved state at getLastChangedFrame.
    //     This restores the game to that frame; subsequent frames will
    //     re-run with the corrected inputs (via the frameStepRerun path
    //     at the top of frameStep).
    //
    //     fastFwdStopFrame is set to the current indexedFrame so the
    //     rerun knows when to stop.
    //
    // Matches CCCaster DllMain.cpp:203-212 (saveState) and
    // DllMain.cpp:591-621 (rollback trigger).
    if (g_netMan.isInGame() && g_netMan.getRollback()) {
        // (a) Save state every frame during InGame.
                g_rollMan.saveState(g_netMan);
        
        // Decrement roundOverTimer (checkRoundOver uses it).
        if (g_roundOverTimer > 0) {
            --g_roundOverTimer;
        }
    }

    if (g_netMan.isInRollback()
        && g_rollbackTimer == g_minRollbackSpacing
        && g_netMan.getLastChangedFrame().value < g_netMan.getIndexedFrame().value) {

        const IndexedFrame target = g_netMan.getLastChangedFrame();

        caster::common::logger::info(
            "dll_main: ROLLBACK — target=[idx={},frame={}] current=[idx={},frame={}]",
            target.parts.index, target.parts.frame,
            g_netMan.getIndex(), g_netMan.getFrame());

        caster::dll::netplay_debug::log_event("rollback-trigger",
            "target_idx", target.parts.index, "target_frm", target.parts.frame,
            "cur_idx", g_netMan.getIndex(), "cur_frm", g_netMan.getFrame());

        // Indicate we're re-running to the current frame.
        g_fastFwdStopFrame = g_netMan.getIndexedFrame();

        // Load the saved state. This restores game memory AND updates
        // netMan._state/_startWorldTime/_indexedFrame to the saved
        // values, so the FSM resumes from the restored frame.
        if (g_rollMan.loadState(target, g_netMan)) {
            // Start fast-forwarding now.
            *asU32(CC_SKIP_FRAMES_ADDR) = 1;

            // Clear the divergence flag so we don't immediately
            // re-trigger. The timer countdown (step 3e) will re-arm
            // it after minRollbackSpacing frames.
            g_netMan.clearLastChangedFrame();
            --g_rollbackTimer;

            caster::dll::netplay_debug::log_event_str("rollback-load-ok",
                std::format("rerun_to idx={} frm={}",
                    g_fastFwdStopFrame.parts.index, g_fastFwdStopFrame.parts.frame));
            return;
        }

        caster::common::logger::warn("dll_main: rollback loadState FAILED");
    }

    // 9. SyncHash exchange + desync detection (netplay only).
    //
    // Every 5*60 frames (5 seconds at 60fps) or 150 frames (2.5s),
    // whichever comes first, we generate a SyncHash from the current
    // game state and send it to the peer. We also store it locally
    // for comparison against the peer's SyncHashes.
    //
    // We skip generation during rollback (the state is about to be
    // rolled back, so the hash would be misleading) and during the
    // first frame of each transition (frame 0 — the state is mid-
    // transition and not yet stable).
    //
    // Matches CCCaster's DllMain.cpp:776 schedule.
    if (g_isNetplay && netplay::connected()) {
        const NetplayState s = g_netMan.getState();
        const bool hashable_state =
            (s == NetplayState::CharaSelect ||
             s == NetplayState::InGame ||
             s == NetplayState::RetryMenu);
        const bool right_time =
            (g_netMan.getFrame() % (5 * 60) == 0) ||
            (g_netMan.getFrame() % 150 == 0);

        if (hashable_state && right_time &&
            !g_netMan.isInRollback() &&
            g_netMan.getFrame() != 0) {
            SyncHash sh;
            sh.readFromGame(g_netMan.getIndexedFrame());
            netplay::sendSyncHash(sh);
            g_localSync.push_back(sh);
        }

        // Compare matching local/remote SyncHashes. We pop pairs where
        // the indexedFrames match and compare them; mismatches trigger
        // delayedStop("Desync!"). Older entries on either side (where
        // the peer never sent a matching hash) are discarded to keep
        // the lists bounded.
        while (!g_localSync.empty() && !g_remoteSync.empty()) {
            const auto& local = g_localSync.front();
            const auto& remote = g_remoteSync.front();

            // Discard remote hashes older than our oldest local hash —
            // we never generated a matching local one (probably because
            // we were in rollback when the schedule fired).
            if (remote.indexedFrame.value < local.indexedFrame.value) {
                g_remoteSync.pop_front();
                continue;
            }
            // Discard local hashes older than the oldest remote hash —
            // symmetric case.
            if (local.indexedFrame.value < remote.indexedFrame.value) {
                g_localSync.pop_front();
                continue;
            }

            // Same indexedFrame — compare.
            if (local != remote) {
                caster::common::logger::err(
                    "dll_main: DESYNC detected at indexedFrame=[idx={},frame={}]",
                    local.indexedFrame.parts.index,
                    local.indexedFrame.parts.frame);
                caster::common::logger::err("  local  hash: (xxHash mismatch)");
                caster::common::logger::err("  remote hash: (xxHash mismatch)");
                g_localSync.clear();
                g_remoteSync.clear();
                delayedStop("Desync!");
                return;
            }

            // Match — discard both and continue.
            g_localSync.pop_front();
            g_remoteSync.pop_front();
        }
    }

        {
        static uint32_t s_frameTick = 0;
        ++s_frameTick;
        const auto lcf = g_netMan.getLastChangedFrame();
        std::string_view rb_action = "none";
        if (g_fastFwdStopFrame.value != 0) rb_action = "rerun";
        else if (g_netMan.isInGame() && g_netMan.getRollback()) rb_action = "save";
        const bool force = (g_fastFwdStopFrame.value != 0) || (spin_ms > 10);
        caster::dll::netplay_debug::log_frame(
            s_frameTick,
            netplayStateStr(g_netMan.getState()),
            g_netMan.getIndex(), g_netMan.getFrame(),
            g_netMan.getIndex(), 0, // remote idx/frame — TODO: expose
            lcf.parts.index, lcf.parts.frame,
            0, 0, // p1/p2 inputs — TODO: wire from writeGameInput
            rb_action, spin_ms, force);
    }
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
