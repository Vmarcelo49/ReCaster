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
#include "input/air_dash_macro.hpp"
#include "input/input_reader.hpp"
#include "ipc/receiver.hpp"
#include "netplay/connector.hpp"
#include "netplay/manager.hpp"
#include "netplay/rollback_manager.hpp"
#include "netplay/debug_log.hpp"
#include "spec/spectator_manager.hpp"
#include "spec/spectate_client.hpp"
#include "overlay/overlay_ui.hpp"
#include "overlay/keymapper.hpp"
#include "overlay/playername_overlay.hpp"
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
#include <array>
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

// Diagnostics knobs (read once from env in doPostLoad).
//
// CASTER_SYNCHASH_INTERVAL=N — override the SyncHash generation interval.
// Default 150 frames (2.5s). Set to 30 (0.5s) or 1 (every frame) for
// faster desync detection during testing. 0 keeps the default.
//
// CASTER_LOG_REMOTE_INPUTS=1 — log every PlayerInputs batch SENT and
// RECEIVED, with idx/frm/size/first-input. Used to diagnose whether
// remote inputs are actually crossing the wire and being applied.
//
// CASTER_AUTO_INPUT_PATTERN — select the InGame auto-input pattern:
//   diverge (default) : host forward+A/down+B, joiner back+C/up+D
//                       (drives peers apart — never deals damage)
//   collide           : both players walk forward + mash A
//                       (forces collisions — round ends naturally)
//   idle              : neutral + mash A (no movement)
//   random            : pseudo-random direction+button each frame, same
//                       pattern on both peers (forces fast divergence +
//                       collisions — best for stress-testing desync)
int  g_syncHashInterval = 150;
bool g_logRemoteInputs  = false;
int  g_autoInputPattern = 0;  // 0=diverge, 1=collide, 2=idle, 3=random

// The NetplayManager — brain of the DLL-side netplay engine.
caster::dll::NetplayManager g_netMan;

// Cached IPC config + derived player numbers. Set in doIpcAndModePatch().
caster::common::ipc::config_buffer::Config g_cfg;
bool     g_isNetplay   = false;
uint8_t  g_localPlayer = 1;
uint8_t  g_remotePlayer = 2;
bool     g_isHost      = false;
// Phase C / Fase 3: spectator mode. When true, the local client is a
// spectator — it only receives BothInputs + RngState + MenuIndex from
// the host and replays the match. It never sends inputs, never rolls
// back, never blocks on isRemoteInputReady.
bool     g_isSpectator = false;

// Phase C / Fase 3: SpectateClient instance. Only created when
// g_isSpectator is true. Owned by this TU, lives for the duration of
// the netplay session.
std::unique_ptr<caster::dll::spec::SpectateClient> g_spectateClient;

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
std::string g_mappingPath;  // path to caster/mapping.ini (set in loadMappings)

// ---- Air Dash Macro (P1 + P2) ----
//
// Two instances: one for P1 (always used) and one for P2 (offline only,
// since in netplay P2 is the remote peer). The macro is enabled per-player
// from mapping.ini's `air_dash_macro` flag (set in the launcher's
// Controllers tab). The step() call happens in frameStep() between
// read_local_input() and netMan.setInput(), only while the FSM is in
// InGame state. Outside InGame, reset() is called so a stale sequence
// doesn't carry over between rounds or back from CharaSelect.
//
// Originally ported from zzcaster's src/dll/air_dash_macro.zig, then
// redesigned with a simpler state machine: jump_dir for N frames + dash
// pulse for 1 frame, with retrigger when 9AB is held (no lockout).
// See air_dash_macro.hpp for the full spec.
caster::dll::AirDashMacro g_airDashMacroP1;
caster::dll::AirDashMacro g_airDashMacroP2;

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
    // Notify the launcher so it can show the reason to the user.
    // If the pipe is closed (manual injection, launcher already exited),
    // this is a no-op.
    caster::dll::ipc_receiver::notify_launcher("STOPPED|" + error);
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
    g_mappingPath = mappingPath;

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

    // Wire Air Dash Macro enable flag from mapping.ini (per-player toggle
    // set in the launcher's Controllers tab). P1 is always wired; P2 is
    // also wired so offline Versus can use the macro on both sides.
    g_airDashMacroP1.setEnabled(g_p1Mapping.air_dash_macro);
    g_airDashMacroP2.setEnabled(g_p2Mapping.air_dash_macro);
    // Propagate the configurable jump frames (in frames @ 60fps).
    g_airDashMacroP1.setJumpFrames(g_p1Mapping.air_dash_jump_frames);
    g_airDashMacroP2.setJumpFrames(g_p2Mapping.air_dash_jump_frames);
    if (g_p1Mapping.air_dash_macro || g_p2Mapping.air_dash_macro) {
        caster::common::logger::info(
            "dll_main: Air Dash Macro enabled (P1={} j{} P2={} j{})",
            g_p1Mapping.air_dash_macro, g_p1Mapping.air_dash_jump_frames,
            g_p2Mapping.air_dash_macro, g_p2Mapping.air_dash_jump_frames);
    }
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

    // SyncHash interval override (for faster desync detection in tests).
    if (auto v = caster::common::win32::env::get("CASTER_SYNCHASH_INTERVAL"); !v.empty()) {
        try {
            int parsed = std::stoi(v);
            if (parsed > 0) {
                g_syncHashInterval = parsed;
                caster::common::logger::info("dll_main: CASTER_SYNCHASH_INTERVAL override = {}", parsed);
            }
        } catch (...) {}
    }

    // Verbose remote-input logging (for diagnosing protocol / container bugs).
    if (caster::common::win32::env::get("CASTER_LOG_REMOTE_INPUTS") == "1") {
        g_logRemoteInputs = true;
        caster::common::logger::info("dll_main: CASTER_LOG_REMOTE_INPUTS active — logging every PlayerInputs send/recv");
    }

    // Auto-input pattern selection.
    if (auto v = caster::common::win32::env::get("CASTER_AUTO_INPUT_PATTERN"); !v.empty()) {
        if (v == "collide")           g_autoInputPattern = 1;
        else if (v == "idle")         g_autoInputPattern = 2;
        else if (v == "random")       g_autoInputPattern = 3;
        else                           g_autoInputPattern = 0;  // diverge
        caster::common::logger::info("dll_main: CASTER_AUTO_INPUT_PATTERN override = {} ({})",
            v, g_autoInputPattern);
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
    // Phase C / Fase 3: spectator mode flag.
    g_isSpectator = cfg.is_spectator();

    // Initialize the playername overlay from the IPC config flags.
    // The flags (bits 4-5) are set by the launcher from config.ini [overlay].
    {
        const bool pnEnabled = cfg.playername_enabled();
        const bool pnBottom  = cfg.playername_position_bottom();
        caster::dll::overlay::playername::init(pnEnabled, !pnBottom);
    }

    if (g_isNetplay || g_isSpectator) {
        if (g_isHost) { g_localPlayer = 1; g_remotePlayer = 2; }
        else          { g_localPlayer = 2; g_remotePlayer = 1; }
        // Initialize the structured netplay debug logger (separate file
        // for host vs joiner so logs don't interleave).
        caster::dll::netplay_debug::init(g_isHost);
    }

    // Phase C / Fase 3: create SpectateClient if we're a spectator.
    if (g_isSpectator) {
        g_spectateClient = std::make_unique<caster::dll::spec::SpectateClient>(&g_netMan);
        caster::common::logger::info("dll_main: SpectateClient created");
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

    // Names: carried by the IPC config (v4). nc.setNames assigns
    // local_name to the local player's slot and remote_name to the
    // remote player's slot, based on host_player.
    if (!cfg.local_name.empty() || !cfg.remote_name.empty()) {
        nc.setNames(cfg.local_name, cfg.remote_name);
    } else {
        nc.names[0].clear();
        nc.names[1].clear();
    }
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

    // Phase C / Fase 2.5: if we're the host, initialize the
    // SpectatorManager so the NetworkThread starts accepting spectator
    // connections. No-op for clients/spectators/offline.
    caster::dll::netplay::initSpectatorManager(&g_netMan);

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

    // Leaving Skippable or CharaIntro — reset state variables.
    // Matches CCCaster DllMain.cpp:1036-1042. The roundOverTimer is
    // armed during InGame (checkRoundOver) to delay the transition to
    // Skippable by a few frames (so rollback can correct the last
    // inputs). If we're now leaving Skippable/CharaIntro, the timer
    // has served its purpose — reset it. lazyDisconnect is similarly
    // scoped to the RetryMenu→Skippable window and should be cleared
    // on any exit from Skippable/CharaIntro.
    if (g_netMan.getState() == NetplayState::Skippable ||
        g_netMan.getState() == NetplayState::CharaIntro) {
        g_roundOverTimer = -1;
        // Note: g_lazyDisconnect is handled above in the RetryMenu
        // enter/exit block; don't double-clear it here.
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
    if (!g_isNetplay && !g_isSpectator) return;

    // PlayerInputs — the high-frequency per-frame input batch from the
    // remote player. (Spectators don't receive PlayerInputs — they get
    // BothInputs instead. But drain anyway in case host ever sends one.)
    while (auto pi = caster::dll::netplay::recvPlayerInputs()) {
        if (g_logRemoteInputs) {
            const uint16_t first = pi->inputs.empty() ? 0 : pi->inputs[0];
            caster::common::logger::info(
                "RECV PlayerInputs: idx={} startFrm={} size={} first=0x{:04x}",
                pi->getIndex(), pi->getStartFrame(), pi->size(), first);
        }
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
        if (g_isSpectator && g_spectateClient) {
            g_spectateClient->onMenuIndex(*mi);
        } else {
            g_netMan.setRemoteRetryMenuIndex(mi->menuIndex);
        }
    }

    // RngState — the host's RNG snapshot, sent at the start of each
    // round. The client applies it via process_manager::setRngState so
    // both sides start the round with identical RNG (preventing desync
    // in things like crit, dust hitboxes, etc.).
    while (auto rs = caster::dll::netplay::recvRngState()) {
        if (g_isSpectator && g_spectateClient) {
            g_spectateClient->onRngState(*rs);
        } else {
            g_netMan.setRngState(*rs);
        }
    }

    // SyncHash — periodic desync-detection snapshots. Stored in
    // g_remoteSync for comparison against locally-generated ones.
    // (Spectators don't generate SyncHashes — they just receive and
    // ignore them. No desync detection for spectators.)
    while (auto sh = caster::dll::netplay::recvSyncHash()) {
        if (!g_isSpectator) {
            g_remoteSync.push_back(*sh);
        }
    }

    // Phase C / Fase 3: spectator-only inboxes. Only populated when the
    // local client is a spectator.
    if (g_isSpectator && g_spectateClient) {
        while (auto sc = caster::dll::netplay::recvSpectateConfig()) {
            g_spectateClient->onSpectateConfig(*sc);
        }
        while (auto igs = caster::dll::netplay::recvInitialGameState()) {
            g_spectateClient->onInitialGameState(*igs);
        }
        while (auto bi = caster::dll::netplay::recvBothInputs()) {
            g_spectateClient->onBothInputs(*bi);
        }
    }
}

// ============================================================================
// tryTriggerRollback — Phase B / Phase 2 (reformulated)
// ============================================================================
//
// Check if a rollback should fire this frame, and if so, fire it.
//
// Phase B / Phase 2 moved this check from the END of frameStep (after
// saveState, ~1-2ms wasted per divergent frame) to RIGHT AFTER
// drainNetplayInbox (where the divergence is actually detected). This
// avoids doing writeGameInput + advance + saveState(1.18MB) work on a
// frame that's about to be discarded by loadState + rerun.
//
// Conditions (same as the original end-of-frameStep check):
//   - We're in InGame with rollback enabled (isInRollback)
//   - The rollback timer has reset to minRollbackSpacing (we haven't
//     rolled back too recently — prevents thrashing)
//   - getLastChangedFrame() < getIndexedFrame() (a remote input arrived
//     that disagrees with what we predicted)
//
// Returns true if rollback fired (caller should return immediately).
bool tryTriggerRollback() {
    using namespace caster::dll;
    if (!g_netMan.isInRollback()) return false;
    if (g_rollbackTimer != g_minRollbackSpacing) return false;
    if (g_netMan.getLastChangedFrame().value >= g_netMan.getIndexedFrame().value)
        return false;

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
    // netMan._state/_startWorldTime/_indexedFrame to the saved values,
    // so the FSM resumes from the restored frame.
    if (g_rollMan.loadState(target, g_netMan)) {
        // Start fast-forwarding now.
        *asU32(CC_SKIP_FRAMES_ADDR) = 1;

        // Clear the divergence flag so we don't immediately re-trigger.
        // The timer countdown (step 10 in frameStep) will re-arm it
        // after minRollbackSpacing frames.
        g_netMan.clearLastChangedFrame();
        --g_rollbackTimer;

        caster::dll::netplay_debug::log_event_str("rollback-load-ok",
            std::format("rerun_to idx={} frm={}",
                g_fastFwdStopFrame.parts.index, g_fastFwdStopFrame.parts.frame));
        return true;
    }

    caster::common::logger::warn("dll_main: rollback loadState FAILED");

    // Clear the divergence flag and decrement the timer to prevent an
    // infinite retry loop.
    g_netMan.clearLastChangedFrame();
    --g_rollbackTimer;
    return true;  // We "fired" (attempted), so still skip the rest of frameStep
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

    // 1a-bis. Defensively clear the game's input buffers before the
    // ChangeMonitor runs. CCCaster writes 0,0 to both players' input
    // addresses here (DllMain.cpp:963-965) so that stale inputs from
    // the previous frame don't leak into the new frame. Without this,
    // a dropped frame or a hook timing issue could cause one player's
    // input to "stick" for an extra frame.
    {
        char* baseAddr = *(char**)caster::dll::CC_PTR_TO_WRITE_INPUT_ADDR;
        if (baseAddr) {
            *reinterpret_cast<uint16_t*>(baseAddr + caster::dll::CC_P1_OFFSET_DIRECTION) = 0;
            *reinterpret_cast<uint16_t*>(baseAddr + caster::dll::CC_P1_OFFSET_BUTTONS)   = 0;
            *reinterpret_cast<uint16_t*>(baseAddr + caster::dll::CC_P2_OFFSET_DIRECTION) = 0;
            *reinterpret_cast<uint16_t*>(baseAddr + caster::dll::CC_P2_OFFSET_BUTTONS)   = 0;
        }
    }

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

    // 2. Drain the inbox into the NetplayManager.
    //
    // Layer 4: netplay::poll() is now a no-op — ENet polling happens on
    // the dedicated network thread inside NetworkThread::loop(). The
    // inbox queues are filled asynchronously by the network thread; we
    // just drain them here on the game thread.
    //
    // We keep drainNetplayInbox() unchanged because it does non-blocking
    // try_pop on each of the 5 BlockingQueues, which is exactly the
    // correct pattern for SPSC queues.
    drainNetplayInbox();

    // 2-pre. Phase C / Fase 2.5: Host-side spectator management.
    // HOTFIX: Only run when there are actual spectators/pending.
    if (g_isHost && !g_isSpectator) {
        auto* sm = caster::dll::netplay::spectatorManager();
        if (sm && (sm->numSpectators() > 0 || sm->numPending() > 0)) {
            if (g_netMan.getState() != caster::dll::NetplayState::PreInitial) {
                sm->promoteAllPending();
            }
            sm->frameStepSpectators();
        }
    }

    // 2b. Detect initial peer connection → transition PreInitial → Initial.
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
        state == NetplayState::Loading ||
        state == NetplayState::CharaIntro ||
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
                // Pattern selection via CASTER_AUTO_INPUT_PATTERN.
                //   0 (diverge)  : host forward+A / down+B, joiner back+C / up+D
                //                  -- drives peers apart, no damage, round never ends
                //   1 (collide)  : both walk forward + mash A
                //                  -- forces collision so the round ends naturally
                //   2 (idle)     : neutral + mash A (no movement)
                //
                // For desync testing we want 'collide' so we exercise the
                // InGame→Skippable and Skippable→InGame round-boundary
                // transitions (the path the original report flagged).
                if (g_autoInputPattern == 3) {
                    // random: deterministic pseudo-random per-InGame-frame.
                    // CRITICAL: seed from g_netMan.getFrame() (the synced
                    // InGame frame counter), NOT g_autoInputFrame (which
                    // drifts between peers due to CharaSelect timing +
                    // rollback rerun counting). Using getFrame() means both
                    // peers compute the same local input for the same frame
                    // — and since the remote input arrives 1 RTT late, the
                    // rollback engine sees a divergence when the real input
                    // arrives, triggering proper rollback recovery.
                    //
                    // Per-side seed: host uses `frame`, joiner uses
                    // `frame * 2 + 1` (odd offset). A simple `+ 0x10000`
                    // offset DOES NOT WORK — the LCG `(seed * 1103515245)
                    // & 0x7FFFFFFF` has its low 16 bits invariant under
                    // `seed += 2^16`, so both peers would generate
                    // identical inputs, defeating the purpose.
                    const uint32_t seed = g_isHost
                        ? g_netMan.getFrame()
                        : (g_netMan.getFrame() * 2u + 1u);
                    uint32_t r = (seed * 1103515245u + 12345u) & 0x7FFFFFFFu;
                    input.direction = static_cast<uint8_t>(r & 0x0F);  // 0..15
                    // Buttons: pick from {0, A, B, C, D, A|B} based on bits.
                    uint8_t btns[] = {0,
                        caster::dll::CC_BUTTON_A,
                        caster::dll::CC_BUTTON_B,
                        caster::dll::CC_BUTTON_C,
                        caster::dll::CC_BUTTON_D,
                        static_cast<uint8_t>(caster::dll::CC_BUTTON_A | caster::dll::CC_BUTTON_B)};
                    input.buttons = btns[(r >> 4) & 0x07];
                } else if (g_autoInputPattern == 1) {
                    // collide: both walk forward + mash A.
                    input.direction = 6;  // forward
                    if ((g_autoInputFrame % 4) < 2) {
                        input.buttons = caster::dll::CC_BUTTON_A;
                    }
                } else if (g_autoInputPattern == 2) {
                    // idle: neutral + mash A.
                    input.direction = 0;
                    if ((g_autoInputFrame % 4) < 2) {
                        input.buttons = caster::dll::CC_BUTTON_A;
                    }
                } else {
                    // diverge (default): different pattern per player.
                    if (g_isHost) {
                        if ((g_autoInputFrame % 8) < 4) {
                            input.direction = 6;  // forward (right)
                            input.buttons = caster::dll::CC_BUTTON_A;
                        } else {
                            input.direction = 2;  // down
                            input.buttons = caster::dll::CC_BUTTON_B;
                        }
                    } else {
                        if ((g_autoInputFrame % 7) < 3) {
                            input.direction = 4;  // back (left)
                            input.buttons = caster::dll::CC_BUTTON_C;
                        } else {
                            input.direction = 8;  // up
                            input.buttons = caster::dll::CC_BUTTON_D;
                        }
                    }
                }
            } else if (state == NetplayState::RetryMenu) {
                // RetryMenu: force-select "Rematch" (menu index 1) and
                // mash CONFIRM. MBAACC retry menu layout: 0 = CharaSelect,
                // 1 = Rematch, 2 = Save Replay. Without forcing the index,
                // auto-input would mash CONFIRM at the default cursor
                // position (0 = CharaSelect) and the game would fall back
                // to character select instead of rematching.
                //
                // We call setLocalRetryMenuIndex(1) every frame while in
                // RetryMenu — it's idempotent (the setter just overwrites
                // _localRetryMenuIndex). Once both peers have set index 1,
                // getRetryMenuInput() computes _targetMenuIndex = max(1,1)
                // = 1, clamped to min(1,1) = 1, and getMenuNavInput()
                // navigates the cursor from 0 to 1 then confirms.
                //
                // The CONFIRM mash here is still needed: getMenuNavInput()
                // drives the cursor via up/down, but the final confirm
                // press comes from this input (filtered by the hook).
                if (g_isNetplay) {
                    g_netMan.setLocalRetryMenuIndex(1);
                }
                if ((g_autoInputFrame % 6) < 3) {
                    input.buttons = caster::dll::CC_BUTTON_CONFIRM;
                }
            } else {
                // Other menus (CharaSelect, Skippable, ReplayMenu):
                // mash CONFIRM (3 on, 3 off).
                if ((g_autoInputFrame % 6) < 3) {
                    input.buttons = caster::dll::CC_BUTTON_CONFIRM;
                }
            }
            ++g_autoInputFrame;
        } else if (g_p1Mapping.device_index >= 0 && g_p1Joy) {
            SDL_JoystickUpdate();
            input = read_local_input(g_p1Joy, g_p1Mapping);
        } else {
            // Keyboard (device < 0) or controller failed to open.
            // All input comes from the mapping.ini bindings — keyboard
            // bindings use GetAsyncKeyState, SDL bindings are no-ops
            // when joy is nullptr. No fallbacks, one source of truth.
            input = read_local_input(nullptr, g_p1Mapping);
        }

        uint16_t combined = combine_input(input);

        // Air Dash Macro (9AB / 7AB). Runs only while InGame so the macro
        // doesn't interfere with menu navigation. Outside InGame, reset()
        // is called so a stale sequence doesn't carry over.
        //
        // The macro is "raw" — when it sees 9AB/7AB it emits jump_dir for
        // jump_frames frames then 6|AB for 1 frame. If 9AB is still held,
        // it RETRIGGERS immediately (no lockout). See air_dash_macro.hpp.
        if (state == NetplayState::InGame) {
            auto r = g_airDashMacroP1.step(combined);
            if (r.triggered) {
                caster::common::logger::info(
                    "dll_main: AirDashMacro(P1) triggered at frame {} "
                    "(input=0x{:04x}, jump_frames={})",
                    g_netMan.getFrame(), combined,
                    g_airDashMacroP1.jumpFrames());
            }
            combined = r.output;
        } else {
            g_airDashMacroP1.reset();
        }

        g_netMan.setInput(g_localPlayer, combined);

        // P2 input — offline Versus only.
        //
        // In netplay, P2 is the remote peer: its inputs arrive via
        // drainNetplayInbox() -> setInputs(g_remotePlayer, ...) and we
        // must NOT overwrite them with a local read.
        //
        // In offline mode (Training or Versus), the "remote" slot is
        // just the second local player. We read its controller here and
        // feed it via setInput(g_remotePlayer, ...) so writeGameInput()
        // later in the frame writes both players' inputs to the game.
        // This mirrors zzcaster's frameStepOffline (frame_step.zig),
        // which reads P1 + P2 controllers and calls writeInput(1, ...)
        // + writeInput(2, ...).
        //
        // Skipped when g_autoInput is on (automated testing): the test
        // harness only drives the local player, and forcing a real P2
        // read would interfere with the synthetic-input path.
        if (!g_isNetplay && !g_autoInput) {
            GameInput p2_input;
            if (g_p2Mapping.device_index >= 0 && g_p2Joy) {
                // SDL_JoystickUpdate() was already called for P1 above;
                // it polls all open joysticks, so P2's state is fresh.
                p2_input = read_local_input(g_p2Joy, g_p2Mapping);
            } else {
                // Keyboard (device < 0) or no P2 controller opened.
                p2_input = read_local_input(nullptr, g_p2Mapping);
            }

            uint16_t p2_combined = combine_input(p2_input);

            // P2 Air Dash Macro — same simple logic as P1 (see above).
            if (state == NetplayState::InGame) {
                auto r2 = g_airDashMacroP2.step(p2_combined);
                if (r2.triggered) {
                    caster::common::logger::info(
                        "dll_main: AirDashMacro(P2) triggered at frame {} "
                        "(input=0x{:04x}, jump_frames={})",
                        g_netMan.getFrame(), p2_combined,
                        g_airDashMacroP2.jumpFrames());
                }
                p2_combined = r2.output;
            } else {
                g_airDashMacroP2.reset();
            }

            g_netMan.setInput(g_remotePlayer, p2_combined);
        }

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
                    if (g_logRemoteInputs) {
                        const uint16_t first = pi->inputs.empty() ? 0 : pi->inputs[0];
                        caster::common::logger::info(
                            "SEND PlayerInputs: idx={} startFrm={} size={} first=0x{:04x}",
                            pi->getIndex(), pi->getStartFrame(), pi->size(), first);
                    }
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

    // 3c. Ready gate (netplay only): isRngStateReady + isRemoteInputReady.
    //
    // Phase B / Phase 1: Speculative rollback.
    //
    // During InGame with rollback enabled, isRemoteInputReady() now
    // returns true as long as we're within MAX_ROLLBACK (15) frames of
    // the latest remote input. This means the spin-lock only blocks
    // when:
    //   - We're in CharSelect (no rollback to correct mispredictions)
    //   - We're in InGame but >15 frames ahead (lockstep fallback)
    //   - isRngStateReady is false (RNG must match exactly, no prediction)
    //   - Rollback is disabled (config.rollback == 0)
    //
    // In all other cases, the game thread advances immediately at 60fps,
    // using lastInputBefore as the predicted remote input. When the real
    // input arrives and diverges, the rollback engine corrects via
    // loadState + frameStepRerun.
    //
    // CASTER_DETERMINISTIC=1 env var reverts isRemoteInputReady() to the
    // old behavior (config.rollback as the cap instead of MAX_ROLLBACK)
    // for debugging desyncs. If a desync is reported, ask the user to
    // reproduce with CASTER_DETERMINISTIC=1.
    //
    // The spin-lock structure is preserved for the cases where blocking
    // IS needed (CharSelect, RNG sync, lockstep fallback). While blocked:
    //   - Re-send PlayerInputs every ~100ms (UNRELIABLE, may drop)
    //   - Timeout after 10s → delayedStop("Timed out!")
    //   - Check disconnect → delayedStop("Opponent disconnected")
    uint32_t spin_ms = 0;
    if (g_isNetplay) {
        const uint32_t spin_start = GetTickCount();
        bool first_poll = true;
        for (;;) {
            // Drain the inbox queues into the NetplayManager.
            //
            // Layer 4: netplay::poll() was removed — the network thread
            // services ENet asynchronously and pushes received messages
            // to the inbox BlockingQueues. drainNetplayInbox() does
            // non-blocking try_pop on each queue.
            //
            // The spin-lock is kept for now (Phase B / speculative
            // rollback will remove it). It blocks the game thread until
            // the network thread has delivered enough remote inputs to
            // advance safely. The network thread keeps running while
            // we're blocked here — packets keep arriving and being
            // routed to the inboxes.
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

            // Peer disconnected? Don't wait for the 10s timeout —
            // stop immediately with a clear message.
            if (!caster::dll::netplay::connected()) {
                delayedStop("Opponent disconnected");
                return;
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

    // 6.5. Phase B2: Early rollback trigger (re-implemented correctly).
    //
    // Check for rollback AFTER sendPlayerInputs (step 3b) + spin-lock
    // (step 3c) + RngState apply (step 3c-1), but BEFORE writeGameInput
    // (step 7) + saveState (step 7b).
    //
    // This saves ~1-2ms per divergent frame by skipping the writeGameInput
    // + saveState (1.18MB memcpy) work on frames that are about to be
    // discarded by loadState + rerun.
    //
    // CRITICAL: sendPlayerInputs (step 3b) has ALREADY run at this point,
    // so the remote has our latest inputs. This was the bug in the
    // original Phase B2 — it was placed BEFORE sendPlayerInputs, causing
    // inputs to not be exchanged when a rollback fired.
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

        g_fastFwdStopFrame = g_netMan.getIndexedFrame();

        if (g_rollMan.loadState(target, g_netMan)) {
            *asU32(CC_SKIP_FRAMES_ADDR) = 1;
            g_netMan.clearLastChangedFrame();
            --g_rollbackTimer;

            caster::dll::netplay_debug::log_event_str("rollback-load-ok",
                std::format("rerun_to idx={} frm={}",
                    g_fastFwdStopFrame.parts.index, g_fastFwdStopFrame.parts.frame));
            return;
        }

        caster::common::logger::warn("dll_main: rollback loadState FAILED");
        g_netMan.clearLastChangedFrame();
        --g_rollbackTimer;
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

    // 7b. Rollback: save state (every InGame frame with rollback enabled).
    //
    // Phase B2 moved the trigger check to step 6.5 (above). If we reach
    // here, no rollback is firing this frame — so we save state for
    // potential future rollbacks.
    //
    // NOTE: This runs AFTER writeGameInput (step 7), so the saved state
    // captures the game state with frame N's inputs already written to
    // the input buffer. This is INTENTIONAL — when loadState(M) restores
    // this state during a later rollback, the input buffer already has
    // M's inputs, and the rerun's writeGameInput(M) is a no-op. The game
    // then simulates frame M correctly.
    //
    // An earlier attempt moved saveState to BEFORE writeGameInput to
    // match CCCaster's ordering (DllMain.cpp:207). This made the desync
    // WORSE (frame 29 instead of 89), indicating the original post-write
    // order is correct for ReCaster's architecture. The difference from
    // CCCaster is that CCCaster's writeGameInput is at the BOTTOM of
    // frameStep (line 988), AFTER frameStepNormal returns — so CCCaster's
    // saveState (top of InGame) IS before writeGameInput. ReCaster's
    // writeGameInput is INSIDE frameStep (step 7), so saving AFTER it
    // captures the post-write state.
    if (g_netMan.isInGame() && g_netMan.getRollback()) {
        g_rollMan.saveState(g_netMan);

        if (g_roundOverTimer > 0) {
            --g_roundOverTimer;
        }
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
        // SyncHash generation schedule. CCCaster uses 5*60 OR 150 (whichever
        // comes first); we honour CASTER_SYNCHASH_INTERVAL for faster testing.
        const int interval = (g_syncHashInterval > 0) ? g_syncHashInterval : 150;
        const bool right_time =
            (g_netMan.getFrame() % (5 * 60) == 0) ||
            (g_netMan.getFrame() % interval == static_cast<uint32_t>(interval - 1));

        if (hashable_state && right_time &&
            g_fastFwdStopFrame.value == 0 &&  // not mid-rerun
            g_netMan.getFrame() != 0) {
            SyncHash sh;
            sh.readFromGame(g_netMan.getIndexedFrame());
            netplay::sendSyncHash(sh);
            g_localSync.push_back(sh);
            // Log the GENERATION (separate from MATCH log) so we can
            // confirm hashes are actually being produced at idx=4.
            caster::common::logger::info(
                "SyncHash GEN idx={} frm={} (local_q={} remote_q={})",
                g_netMan.getIndex(), g_netMan.getFrame(),
                g_localSync.size(), g_remoteSync.size());
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
                // Dump the full SyncHash struct fields so we can see WHAT
                // diverged (health, position, meter, RNG, etc.) — this is
                // critical for narrowing down the desync source.
                auto dump = [](std::string_view label, const caster::dll::SyncHash& sh) {
                    // Dump the RNG hash bytes (xxHash of RNG state) so we can
                    // see if RNG diverged even when other fields match.
                    std::string hash_hex;
                    hash_hex.reserve(sh.hash.size() * 2);
                    static constexpr const char* hex = "0123456789abcdef";
                    for (uint8_t b : sh.hash) {
                        hash_hex += hex[b >> 4];
                        hash_hex += hex[b & 0x0F];
                    }
                    caster::common::logger::err(
                        "  {} hash: rngHash={} roundTimer={} realTimer={} camX={} camY={}",
                        label, hash_hex, sh.roundTimer, sh.realTimer, sh.cameraX, sh.cameraY);
                    for (int i = 0; i < 2; ++i) {
                        const auto& c = sh.chara[i];
                        caster::common::logger::err(
                            "  {} chara[{}]: seq={} seqState={} hp={} redHp={} meter={} heat={} guardBar={} guardQ={} x={} y={} chara={} moon={}",
                            label, i, c.seq, c.seqState, c.health, c.redHealth,
                            c.meter, c.heat, c.guardBar, c.guardQuality,
                            c.x, c.y, c.chara, c.moon);
                    }
                };
                dump("local ", local);
                dump("remote", remote);
                caster::dll::netplay_debug::log_event_str("desync",
                    std::format("idx={} frm={}",
                        local.indexedFrame.parts.index,
                        local.indexedFrame.parts.frame));
                g_localSync.clear();
                g_remoteSync.clear();
                delayedStop("Desync!");
                return;
            }

            // Match — log + discard both and continue. Logged at INFO so we
            // can see SyncHashes flowing (the absence of these lines means
            // SyncHash isn't being generated, which is itself a bug signal).
            caster::common::logger::info(
                "SyncHash MATCH idx={} frm={} ({} entries queued)",
                local.indexedFrame.parts.index,
                local.indexedFrame.parts.frame,
                g_localSync.size() + g_remoteSync.size());
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
        // Wire up the previously-TODO fields so the per-frame trace is
        // actually diagnostic. localInput/remoteInput are computed at the
        // top of step 7 (writeGameInput) and remain in scope here.
        const auto rmtIdxFrm = g_netMan.getRemoteIndexedFrame();
        caster::dll::netplay_debug::log_frame(
            s_frameTick,
            netplayStateStr(g_netMan.getState()),
            g_netMan.getIndex(), g_netMan.getFrame(),
            rmtIdxFrm.parts.index, rmtIdxFrm.parts.frame,
            lcf.parts.index, lcf.parts.frame,
            localInput, remoteInput,
            rb_action, spin_ms, force);
    }
}

} // namespace

// ============================================================================
// callback() — per-frame hook entry point
// ============================================================================

extern "C" void callback() {
    if (!g_running.load()) return;

    // Check if the game is still alive. MBAACC sets CC_ALIVE_FLAG_ADDR
    // to a nonzero value while the game is running; when it drops to 0
    // (Alt+F4, crash, or normal exit), the game is gone and we should
    // stop the DLL cleanly. Without this check, the hook keeps firing
    // into a dead process and sockets stay open until the OS reaps it.
    if (g_isNetplay && *caster::dll::asU32(caster::dll::CC_ALIVE_FLAG_ADDR) == 0) {
        caster::common::logger::warn("dll_main: game alive flag is 0 — stopping");
        delayedStop("Game closed");
        return;
    }

    // Check if the peer has disconnected during netplay. The ENet connector
    // sets g_connected = false when it receives a DISCONNECT event (either
    // graceful from the peer's DLL_PROCESS_DETACH, or after ENet's ~30s
    // timeout for silent loss). Without this check, the local player would
    // freeze for up to 10s (spin-lock timeout) and see "Timed out!" instead
    // of a clear "Opponent disconnected" message.
    //
    // We only check this once we've passed the initial connection phase
    // (state != PreInitial) and only during active play (not in menus where
    // the connection might briefly drop during transitions).
    if (g_isNetplay && g_netMan.getState() != caster::dll::NetplayState::PreInitial) {
        if (!caster::dll::netplay::connected()) {
            caster::common::logger::warn("dll_main: peer disconnected during netplay");
            delayedStop("Opponent disconnected");
            return;
        }
    }

    // ---- Hotkey polling (BEFORE frameStep) ----
    //
    // We poll GetAsyncKeyState for the top-row number keys 1-5 every frame
    // instead of relying on WM_KEYDOWN in WindowProcHook. The MBAA game
    // window often consumes keyboard events before they reach WindowProc
    // (especially under Wine), so WM_KEYDOWN is unreliable. GetAsyncKeyState
    // reflects the global keyboard state regardless of which window has
    // focus.
    //
    // Edge detection with debounce: we require the key to be released for
    // at least N frames before accepting a new press-edge. This filters out
    // auto-repeat (which generates spurious "release" frames between repeats
    // on some systems, defeating naive edge detection).
    //
    // Hotkey '4' (keymapper) is DISABLED during netplay — only available
    // in offline versus and training modes.
    {
        // Per-key state machine:
        //   0 = released (waiting for press)
        //   1 = pressed (waiting for release)
        //   2 = released-but-debouncing (need N frames before accepting press)
        static std::array<uint8_t, 256> keyState{};
        static std::array<uint8_t, 256> debounceCounter{};
        constexpr uint8_t kDebounceFrames = 5;  // ~83ms at 60fps

        constexpr int hotkeys[] = { '1', '2', '3', '4', '5' };
        for (int vk : hotkeys) {
            const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
            uint8_t& state = keyState[vk];

            // Advance the debounce counter.
            if (state == 2) {
                if (debounceCounter[vk] > 0) {
                    --debounceCounter[vk];
                    continue;  // still debouncing
                }
                state = 0;  // debounce complete, ready for new press
            }

            if (state == 0 && now) {
                // Press-edge: trigger the hotkey.
                state = 1;
            } else if (state == 1 && !now) {
                // Release-edge: start debounce.
                state = 2;
                debounceCounter[vk] = kDebounceFrames;
                continue;
            } else {
                continue;  // no edge
            }

            // Forward to keymapper first — if it's capturing a key, consume.
            if (caster::dll::overlay::keymapper::handleKeyEvent(
                    static_cast<uint32_t>(vk), /*isDown=*/true)) {
                continue;
            }

            switch (vk) {
                case '1':
                    caster::common::logger::info("hotkey: '1' pressed (reserved — not implemented yet)");
                    break;
                case '2':
                    caster::common::logger::info("hotkey: '2' pressed (reserved — not implemented yet)");
                    break;
                case '3':
                    if (!caster::dll::overlay::keymapper::isActive()) {
                        caster::dll::overlay::toggle();
                        caster::common::logger::info("hotkey: '3' overlay toggled (now {})",
                            caster::dll::overlay::isEnabled() ? "enabled" : "disabled");
                    }
                    break;
                case '4':
                    // Keymapper only available in offline/training modes.
                    // During netplay, ignore — mapping changes would desync
                    // rollback state.
                    if (g_isNetplay) {
                        caster::common::logger::info("hotkey: '4' ignored during netplay");
                    } else {
                        caster::dll::overlay::keymapper::toggle();
                    }
                    break;
                case '5':
                    // Toggle playername overlay (only meaningful during netplay).
                    caster::dll::overlay::playername::toggle();
                    break;
            }
        }
    }

    // ---- Keymapper takes over the frame when active ----
    //
    // When the keymapper overlay is active, we suppress all game inputs
    // (write neutral 0,0 to both players) and skip the normal frameStep().
    // The controller state is consumed by keymapper::update() for navigation
    // and binding capture, not by the game.
    if (caster::dll::overlay::keymapper::isActive()) {
        // Write neutral inputs to both players so the game doesn't act on
        // the controller buttons the user is pressing to navigate the mapper.
        caster::dll::process_manager::writeGameInput(1, 0, 0);
        caster::dll::process_manager::writeGameInput(2, 0, 0);

        // Drive the keymapper state machine.
        try {
            std::array<SDL_Joystick*, 2> joys = { g_p1Joy, g_p2Joy };
            std::array<caster::common::controller::ControllerMapping*, 2> maps = { &g_p1Mapping, &g_p2Mapping };
            caster::dll::overlay::keymapper::update(joys, maps, g_mappingPath);

            // If keymapper just deactivated (user pressed Done on both
            // players), reload the mappings from disk so the next frameStep
            // uses the new bindings. This also re-opens the SDL_Joystick
            // handles if the device_index changed.
            if (!caster::dll::overlay::keymapper::isActive()) {
                caster::common::logger::info("dll_main: keymapper closed — reloading mappings");
                // Force reload by resetting the loaded flag and re-calling loadMappings.
                g_mappingsLoaded = false;
                if (g_p1Joy) { SDL_JoystickClose(g_p1Joy); g_p1Joy = nullptr; }
                if (g_p2Joy) { SDL_JoystickClose(g_p2Joy); g_p2Joy = nullptr; }
                loadMappings();
            }
        } catch (...) {
            caster::common::logger::err("dll_main: exception in keymapper::update()");
        }
        return;
    }

    try {
        frameStep();
    } catch (...) {
        caster::common::logger::err("dll_main: exception in callback()");
    }

    // Drive the overlay state machine every frame: animate bar height,
    // tick message timeouts, refresh text. This runs after frameStep() so
    // the overlay reflects the latest netplay state. The state machine
    // always ticks (even on Wine if the DX hook failed), but the actual
    // rendering only happens in presentFrameBegin() when the DX9 Present
    // vtable hook is installed and fires.
    //
    // Note: when the keymapper is active, the early-return block above
    // already called keymapper::update() and returned, so we only reach
    // here when the keymapper is inactive.
    try {
        if (caster::dll::overlay::isShowingMessage()) {
            caster::dll::overlay::updateMessage();
        } else {
            caster::dll::overlay::updateText();
        }
    } catch (...) {
        // Overlay errors must never crash the game.
    }

    // Update the playername overlay: feed it the latest netplay state
    // and player names. It renders in presentFrameBegin() on the next
    // Present call. Does nothing if not in netplay.
    try {
        caster::dll::overlay::playername::setNetplayActive(g_isNetplay);
        if (g_isNetplay) {
            // Names come from the IPC config (v4), stored in
            // g_netMan.config.names[2]. P1 = names[0], P2 = names[1].
            // If names are empty (e.g. v3 launcher), fall back to
            // "Player 1" / "Player 2" so the overlay is still visible.
            const auto& names = g_netMan.config.names;
            std::string p1 = (names.size() > 0 && !names[0].empty()) ? names[0] : "Player 1";
            std::string p2 = (names.size() > 1 && !names[1].empty()) ? names[1] : "Player 2";
            caster::dll::overlay::playername::setNames(p1, p2);
        }
    } catch (...) {
        // Playername overlay errors must never crash the game.
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

void PresentFrameBegin(IDirect3DDevice9* device) {
    caster::dll::overlay::presentFrameBegin(device);
    // Render the playername overlay after the info overlay so it draws
    // on top. Does nothing if not visible (offline / disabled / no names).
    caster::dll::overlay::playername::render(device);
}
void EndScene(IDirect3DDevice9*) {}
void InvalidateDeviceObjects() {
    caster::dll::overlay::invalidateDeviceObjects();
    caster::dll::overlay::playername::invalidateDeviceObjects();
}
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

            // CRITICAL ORDERING (Layer 4):
            //   1. netplay::shutdown() — stops the NetworkThread jthread
            //      (request_stop + join), THEN destroys ENetHost. The
            //      jthread must be joined before anything else happens,
            //      because the network thread reads host_/peer_ inside
            //      its loop. If we destroyed ENetHost first, the network
            //      thread would touch freed memory.
            //   2. SDL_JoystickClose — game thread only, safe anytime.
            //   3. deinitialize() — removes ASM hooks + D3D9 hooks from
            //      the game. After this returns, the game's main loop
            //      callback() no longer fires, so frameStep() can't run
            //      concurrently with our cleanup.
            //
            // The NetworkThread destructor is a safety net (auto-joins
            // the jthread if stop() wasn't called), but explicit
            // shutdown() here guarantees clean ordering.
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
