// src/exe/launcher/game_runner.cpp
//
// GameRunner implementation — worker-thread edition (Layer 2).
//
// All launch/kill/IPC operations run on the internal `std::jthread`.
// The UI thread enqueues commands via `*_async()` and reads state via
// `snapshot()`.

#include "game_runner.hpp"
#include "dxvk.hpp"
#include "../../common/config.hpp"
#include "../../common/ipc/config_buffer.hpp"
#include "../../common/ipc/pipe_name.hpp"
#include "../../common/logger.hpp"
#include "../../common/win32/env.hpp"
#include "../../common/win32/paths.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <utility>

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace fs = std::filesystem;

namespace caster::exe::launcher {

namespace {

// Wait this long (ms) for the DLL to connect to the IPC server after
// we resume the main thread. The DLL connects on its FIRST HOOKED GAME
// FRAME (see dll_main::doIpcAndModePatch), not at process start — so on
// slow disks / cold boots / heavy antivirus scanning, the first frame can
// take well over 10 s. The wait returns as soon as the DLL connects, so a
// generous budget only costs time in genuine failure cases.
constexpr std::uint32_t kIpcConnectTimeoutMs = 30000;

// Worker loop sleep between updates. ~60fps is responsive enough for
// detecting game exit and IPC messages.
constexpr auto kWorkerSleep = std::chrono::milliseconds(16);

// Locate the bundled DXVK d3d9.dll that ships next to caster.exe.
// Returns empty string if not found (CMakeLists.txt should have copied
// it into the runtime output dir at configure time).
//
// We search in order:
//   1. <exe_dir>/d3d9.dll (primary location — matches CMakeLists.txt
//      CMAKE_RUNTIME_OUTPUT_DIRECTORY).
//   2. Current working directory — fallback for unusual install layouts.
//
// The exe dir is resolved via common::win32::paths (unicode-safe). If it
// is unavailable we say so loudly: silently falling back to CWD has
// launched another install's files before.
std::string resolve_bundled_dxvk_dll() {
    if (auto p = common::win32::paths::exe_file(CASTER_DXVK_D3D9_FILENAME);
        p && fs::exists(*p)) {
        return p->string();
    } else if (!p) {
        common::logger::err("game_runner: could not resolve exe dir while "
                            "looking for {} — falling back to CWD (risk: files "
                            "from another install!)", CASTER_DXVK_D3D9_FILENAME);
    } else {
        common::logger::warn("game_runner: bundled DXVK d3d9.dll not found at {}",
                             p->string());
    }
    // Fallback: CWD.
    const fs::path cwd = fs::current_path() / CASTER_DXVK_D3D9_FILENAME;
    if (fs::exists(cwd)) {
        return cwd.string();
    }
    common::logger::warn("game_runner: bundled DXVK d3d9.dll not found in CWD either");
    return {};
}

// Remove a stale DXVK d3d9.dll from the game dir so the game falls back
// to real native D3D9. Windows loads d3d9.dll from the game dir first —
// a leftover DXVK build (shipped in the release zip, or deployed earlier
// when Vulkan was available) would load and crash during D3D init on
// machines without Vulkan (e.g. pre-Skylake Intel iGPUs), before our
// first hooked frame, surfacing as a misleading IPC timeout.
//
// Only removes the file when it size-matches our bundled DXVK build, so
// a user's own d3d9.dll (Reshade, etc.) is never touched.
void remove_stale_dxvk(const std::string& working_dir) {
    const std::string bundled = resolve_bundled_dxvk_dll();
    if (bundled.empty()) {
        // Can't verify provenance — leave any d3d9.dll alone.
        return;
    }
    std::error_code ec;
    const fs::path dest = fs::path(working_dir) / "d3d9.dll";
    if (!fs::exists(dest, ec)) return;
    const auto src_size = fs::file_size(bundled, ec);
    const auto dst_size = fs::file_size(dest, ec);
    if (ec || src_size != dst_size) {
        common::logger::info("game_runner: leaving foreign d3d9.dll in place "
                             "at {} (not our DXVK build)", dest.string());
        return;
    }
    if (fs::remove(dest, ec)) {
        common::logger::info("game_runner: removed stale DXVK d3d9.dll at {} "
                             "(no Vulkan / DXVK disabled — using native D3D9)",
                             dest.string());
    }
}

// Orchestrate the DXVK deploy + env var setup. Called before every
// launch_internal() when cfg.dxvk_enabled is true.
//
// Behavior:
//   - If cfg.dxvk_enabled is false: remove our stale DXVK dll if present,
//     log + return true (caller proceeds with native D3D9).
//   - If Vulkan is not available: same removal, log + return true
//     (caller proceeds without DXVK — graceful fallback).
//   - If Vulkan is available: deploy d3d9.dll + set env vars. If deploy
//     fails, log the error + return true (don't block the launch — the
//     game still works on native D3D9, just with worse frametimes).
//
// Returns false only on critical misconfiguration that should abort
// the launch (currently none — every failure mode falls back to native
// D3D9 gracefully).
bool setup_dxvk(const common::config::Config& cfg,
                const std::string& working_dir) {
    if (!cfg.dxvk_enabled) {
        common::logger::info("game_runner: DXVK disabled by config "
                             "([game] dxvk_enabled=false) — using native D3D9");
        remove_stale_dxvk(working_dir);
        // Scrub a possibly globally-set DXVK_FRAME_RATE: our DLL stands
        // down its own limiter when that var is present, which would
        // leave the game uncapped with no DXVK around to pace it.
        common::win32::env::unset("DXVK_FRAME_RATE");
        return true;
    }

    if (!dxvk::is_vulkan_available()) {
        // Already logged inside is_vulkan_available(). Don't set any
        // DXVK env vars — proceed with native D3D9.
        remove_stale_dxvk(working_dir);
        common::win32::env::unset("DXVK_FRAME_RATE");
        return true;
    }

    // Vulkan is available — deploy the DLL + set env vars.
    const std::string bundled = resolve_bundled_dxvk_dll();
    if (bundled.empty()) {
        common::logger::warn("game_runner: DXVK enabled + Vulkan available, "
                             "but bundled d3d9.dll not found — falling back to "
                             "native D3D9. Install may be incomplete.");
        return true;
    }

    std::string err;
    if (!dxvk::deploy(bundled, working_dir, err)) {
        common::logger::warn("game_runner: DXVK deploy failed ({}), "
                             "falling back to native D3D9", err);
        return true;
    }

    dxvk::set_env_vars(working_dir);
    return true;
}

} // namespace

// ============================================================================
// Construction / destruction
// ============================================================================

GameRunner::GameRunner(unsigned instance_id)
    : instance_id_(instance_id),
      worker_([this](std::stop_token st) { worker_loop(st); }) {
    publish_snapshot();
}

GameRunner::~GameRunner() {
    // Enqueue ForceKill so the worker cleans up before the jthread stops.
    commands_.push(game_runner_command::ForceKill{});
    worker_.request_stop();
    // jthread destructor joins.
}

// ============================================================================
// Async command API
// ============================================================================
//
// IMPORTANT: the launch_*_async methods set `launch_in_progress_ = true`
// SYNCHRONOUSLY (under the snapshot mutex) BEFORE enqueuing the command.
// This closes a race where the UI thread enqueues the command, transitions
// to InGame, reads the snapshot in the same frame, and sees
// `is_running=false && launch_in_progress=false` (because the worker
// hasn't picked up the command yet) — which would make the UI think the
// game already exited and fall back to Idle.
//
// By setting the flag synchronously, the UI is guaranteed to see
// `launch_in_progress=true` on the very next snapshot read, even if the
// worker hasn't started processing the command yet.

void GameRunner::launch_offline_async(const common::config::Config& cfg,
                                      const LaunchOfflineParams& params) {
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        launch_in_progress_ = true;
        last_error_.clear();
        snapshot_.launch_in_progress = true;
        snapshot_.last_error.clear();
    }
    commands_.push(game_runner_command::LaunchOffline{cfg, params});
}

void GameRunner::launch_after_handshake_async(
    const common::config::Config& cfg,
    const session::NetplayConfig& np_cfg) {
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        launch_in_progress_ = true;
        last_error_.clear();
        snapshot_.launch_in_progress = true;
        snapshot_.last_error.clear();
    }
    commands_.push(game_runner_command::LaunchAfterHandshake{cfg, np_cfg});
}

void GameRunner::force_kill_async() {
    commands_.push(game_runner_command::ForceKill{});
}

void GameRunner::suspend_async() {
    commands_.push(game_runner_command::Suspend{});
}

void GameRunner::resume_async() {
    commands_.push(game_runner_command::Resume{});
}

void GameRunner::minimize_window_async() {
    commands_.push(game_runner_command::MinimizeWindow{});
}

void GameRunner::restore_window_async() {
    commands_.push(game_runner_command::RestoreWindow{});
}

// ============================================================================
// Snapshot
// ============================================================================

GameRunnerSnapshot GameRunner::snapshot() const {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    return snapshot_;
}

void GameRunner::publish_snapshot() {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snapshot_.is_running         = launcher_.is_launched();
    snapshot_.pid                = launcher_.pid();
    snapshot_.ipc_handshake_done = ipc_handshake_done_;
    snapshot_.stop_reason        = stop_reason_;
    snapshot_.last_error         = last_error_;
    snapshot_.launch_in_progress = launch_in_progress_;
    snapshot_.is_suspended       = launcher_.is_suspended();
}

// ============================================================================
// Worker thread
// ============================================================================

void GameRunner::worker_loop(std::stop_token st) {
    while (!st.stop_requested()) {
        drain_commands();

        // If the game is running, poll for exit + IPC messages.
        if (launcher_.is_launched()) {
            update();
        }

        publish_snapshot();
        std::this_thread::sleep_for(kWorkerSleep);
    }

    // Final cleanup on shutdown.
    drain_commands();
    if (launcher_.is_launched()) {
        force_kill_sync();
    }
    publish_snapshot();
}

void GameRunner::drain_commands() {
    game_runner_command::Command cmd;
    while (commands_.try_pop(cmd)) {
        apply_command(cmd);
    }
}

void GameRunner::apply_command(const game_runner_command::Command& cmd) {
    using namespace game_runner_command;
    std::visit([this](const auto& c) {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, LaunchOffline>) {
            auto paths = prepare_launch(c.cfg);
            if (!paths) return;

            // Build the IPC config buffer for offline mode.
            common::ipc::config_buffer::Config ipc_cfg;
            ipc_cfg.flags = common::ipc::config_buffer::kFlagTraining;  // bit0
            if (!c.params.training) {
                ipc_cfg.flags = 0;
            }
            // Overlay settings (passed via flags bits 4-5).
            if (c.cfg.playername_enabled)
                ipc_cfg.flags |= common::ipc::config_buffer::kFlagPlayernameEnabled;
            if (c.cfg.playername_position_bottom)
                ipc_cfg.flags |= common::ipc::config_buffer::kFlagPlayernameBottom;
            ipc_cfg.delay          = 0;
            ipc_cfg.rollback       = 0;  // offline mode — no rollback needed
            ipc_cfg.win_count      = static_cast<std::uint8_t>(c.cfg.versus_win_count);
            ipc_cfg.host_player    = 1;
            ipc_cfg.peer_port      = 0;
            ipc_cfg.local_udp_port = 0;
            ipc_cfg.match_seed     = 0;
            ipc_cfg.peer_addr      = "";

            auto r = launch_internal(paths->game_exe, paths->dll_path,
                                     paths->working_dir, paths->high_priority,
                                     ipc_cfg);
            if (!r.success) {
                last_error_ = r.error_message;
            }
            launch_in_progress_ = false;
        } else if constexpr (std::is_same_v<T, LaunchAfterHandshake>) {
            // No sleep needed before the game binds its UDP port: the
            // launcher only reaches this path after the session has
            // reported Idle (its ENet/relay socket is destroyed), and UDP
            // sockets have no TIME_WAIT, so the port is already released.
            auto paths = prepare_launch(c.cfg);
            if (!paths) return;

            // Build the IPC config buffer from the NetplayConfig snapshot.
            common::ipc::config_buffer::Config ipc_cfg;
            ipc_cfg.flags = common::ipc::config_buffer::kFlagNetplay;
            if (c.np_cfg.is_host) {
                ipc_cfg.flags |= common::ipc::config_buffer::kFlagHost;
            }
            if (c.np_cfg.is_training) {
                ipc_cfg.flags |= common::ipc::config_buffer::kFlagTraining;
            }
            if (c.np_cfg.is_spectator) {
                ipc_cfg.flags |= common::ipc::config_buffer::kFlagSpectator;
            }
            // Overlay settings (passed via flags bits 4-5).
            if (c.cfg.playername_enabled)
                ipc_cfg.flags |= common::ipc::config_buffer::kFlagPlayernameEnabled;
            if (c.cfg.playername_position_bottom)
                ipc_cfg.flags |= common::ipc::config_buffer::kFlagPlayernameBottom;
            ipc_cfg.delay          = c.np_cfg.delay;
            ipc_cfg.rollback       = c.np_cfg.rollback;
            ipc_cfg.win_count      = c.np_cfg.win_count;
            ipc_cfg.host_player    = c.np_cfg.host_player;
            ipc_cfg.peer_port      = c.np_cfg.peer_port;
            ipc_cfg.local_udp_port = c.np_cfg.local_udp_port;
            ipc_cfg.match_seed     = c.np_cfg.match_seed;
            ipc_cfg.peer_addr      = c.np_cfg.peer_addr;
            ipc_cfg.local_name     = c.np_cfg.local_name;
            ipc_cfg.remote_name    = c.np_cfg.remote_name;

            common::logger::info(
                "game_runner: launching netplay game (host={} delay={} rollback={} "
                "win={} peer={}:{} local_udp={} seed=0x{:08x} local_name='{}' remote_name='{}')",
                c.np_cfg.is_host, ipc_cfg.delay, ipc_cfg.rollback,
                ipc_cfg.win_count, c.np_cfg.peer_addr, c.np_cfg.peer_port,
                c.np_cfg.local_udp_port, c.np_cfg.match_seed,
                c.np_cfg.local_name, c.np_cfg.remote_name);

            auto r = launch_internal(paths->game_exe, paths->dll_path,
                                     paths->working_dir, paths->high_priority,
                                     ipc_cfg);
            if (!r.success) {
                last_error_ = r.error_message;
            }
            launch_in_progress_ = false;
        } else if constexpr (std::is_same_v<T, ForceKill>) {
            force_kill_sync();
        } else if constexpr (std::is_same_v<T, Suspend>) {
            launcher_.suspend();
        } else if constexpr (std::is_same_v<T, Resume>) {
            launcher_.resume();
        } else if constexpr (std::is_same_v<T, MinimizeWindow>) {
            launcher_.minimize_window();
        } else if constexpr (std::is_same_v<T, RestoreWindow>) {
            launcher_.restore_window();
        }
    }, cmd);
}

// ============================================================================
// Synchronous helpers (run on the worker thread)
// ============================================================================

std::string GameRunner::resolve_game_exe(
    const common::config::Config& cfg) const {

    // 1. cfg.game_dir overrides everything.
    if (!cfg.game_dir.empty()) {
        fs::path p = fs::path(cfg.game_dir) / "MBAA.exe";
        if (fs::exists(p)) {
            common::logger::info("game_runner: found MBAA.exe at {} (from config game_dir)", p.string());
            return p.string();
        }
        common::logger::warn("game_runner: cfg.game_dir='{}' but MBAA.exe not found there",
                     cfg.game_dir);
    }

    // 2. <exe_dir>/MBAA.exe (same folder as caster.exe — primary layout).
    // 3. <exe_dir>/game/MBAA.exe (alternative subfolder layout).
    // (via common::win32::paths — unicode-safe; see resolve_bundled_dxvk_dll
    // for why a missing exe dir is an err, not a silent CWD fallback).
    if (auto exe_dir = common::win32::paths::exe_dir()) {
        fs::path flat = *exe_dir / "MBAA.exe";
        common::logger::info("game_runner: checking exe dir: {}", flat.string());
        if (fs::exists(flat)) {
            common::logger::info("game_runner: using MBAA.exe at {} (exe dir)",
                                 flat.string());
            return flat.string();
        }

        fs::path game_subdir = *exe_dir / "game" / "MBAA.exe";
        common::logger::info("game_runner: checking exe/game: {}", game_subdir.string());
        if (fs::exists(game_subdir)) {
            common::logger::info("game_runner: using MBAA.exe at {} (exe/game subdir)",
                                 game_subdir.string());
            return game_subdir.string();
        }
    } else {
        common::logger::err("game_runner: could not resolve exe dir while "
                            "looking for MBAA.exe — falling back to CWD (risk: "
                            "game from another install!)");
    }

    // 4. Current working directory.
    fs::path cwd_flat = fs::current_path() / "MBAA.exe";
    common::logger::info("game_runner: checking CWD: {}", cwd_flat.string());
    if (fs::exists(cwd_flat)) {
        common::logger::info("game_runner: using MBAA.exe at {} (CWD fallback)",
                             cwd_flat.string());
        return cwd_flat.string();
    }

    common::logger::err("game_runner: MBAA.exe not found in exe dir, game/ subdir, or CWD");

    return {};
}

std::string GameRunner::resolve_hook_dll() const {
    if (auto p = common::win32::paths::exe_file("hook.dll")) {
        common::logger::info("game_runner: using hook.dll at {} (exe dir)",
                             p->string());
        return p->string();
    }
    // No exe dir (practically impossible) — CWD fallback, said loudly.
    common::logger::err("game_runner: could not resolve exe dir while looking "
                        "for hook.dll — falling back to CWD (risk: DLL from "
                        "another install!)");
    auto fallback = fs::absolute("hook.dll").string();
    common::logger::info("game_runner: using hook.dll at {} (CWD fallback)",
                         fallback);
    return fallback;
}

std::optional<GameRunner::ResolvedPaths> GameRunner::prepare_launch(
    const common::config::Config& cfg) {
    if (launcher_.is_launched()) {
        last_error_ = "Game already running (PID " +
                      std::to_string(launcher_.pid()) + ")";
        return std::nullopt;
    }
    launch_in_progress_ = true;
    last_error_.clear();
    publish_snapshot();

    ResolvedPaths rp;
    rp.game_exe = resolve_game_exe(cfg);
    if (rp.game_exe.empty()) {
        last_error_ = "MBAA.exe not found. Place it in the same folder "
                      "as caster.exe (or set game_dir in caster/config.ini).";
        launch_in_progress_ = false;
        return std::nullopt;
    }
    rp.dll_path = resolve_hook_dll();
    if (!fs::exists(rp.dll_path)) {
        last_error_ = "hook.dll not found at " + rp.dll_path;
        launch_in_progress_ = false;
        return std::nullopt;
    }
    rp.working_dir = fs::path(rp.game_exe).parent_path().string();
    rp.high_priority = cfg.high_cpu_priority;

    // Always visible: the exact files this launch will use. If exe-dir
    // resolution ever falls back to CWD again, this line shows it.
    common::logger::info("game_runner: resolved game_exe='{}' hook_dll='{}' "
                         "working_dir='{}'",
                         rp.game_exe, rp.dll_path, rp.working_dir);

    setup_dxvk(cfg, rp.working_dir);
    return rp;
}

LaunchResult GameRunner::launch_internal(
    const std::string& game_exe,
    const std::string& dll_path,
    const std::string& working_dir,
    bool high_priority,
    const common::ipc::config_buffer::Config& ipc_cfg) {
    // Clear state from any previous run.
    stop_reason_.clear();
    ipc_recv_buffer_.clear();
    // Stage 0: t=0 for the "CreateProcess" timeline line — measures the
    // launch prep + CreateProcess + inject + resume latency.
    const auto launchStart = std::chrono::steady_clock::now();

    LaunchResult r;

    // Cleanup helper for error exits after IPC server is open.
    auto cleanup = [&]() {
        if (launcher_.is_launched()) launcher_.terminate();
        ipc_server_.close();
        pipe_name_.clear();
    };

    // 1. Generate pipe name and set env var so the DLL can find it.
    //    Use instance_id to make it unique when multiple game instances
    //    run from the same launcher (training-while-hosting).
    pipe_name_ = common::ipc::pipe_name::for_instance(
        common::win32::process::current_pid(), instance_id_);
    common::win32::env::set(common::ipc::pipe_name::kEnvVarName, pipe_name_);
    common::logger::info("game_runner: pipe = {} (instance={})", pipe_name_, instance_id_);

    // 2. Start the IPC server (before launching, so the DLL can connect
    //    as soon as it's injected).
    if (!ipc_server_.listen(pipe_name_)) {
        r.error_message = "Failed to start IPC server on " + pipe_name_;
        return r;
    }

    // 3. Launch the game (suspended + inject + patches + resume).
    LaunchConfig lcfg;
    lcfg.game_exe_path = game_exe;
    lcfg.dll_path      = dll_path;
    lcfg.working_dir   = working_dir;
    lcfg.high_priority = high_priority;
    lcfg.training      = ipc_cfg.is_training();

    std::string launch_err;
    if (!launcher_.launch(lcfg, launch_err)) {
        r.error_message = launch_err;
        cleanup();
        return r;
    }
    r.pid = launcher_.pid();
    // Stage 0: timeline anchor between "session: deinit" and the DLL's
    // "initial connect established" — the game is now spawned+injected.
    const auto launchMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - launchStart).count();
    common::logger::info(
        "game_runner: CreateProcess (pid={}, t={}ms since launch start)",
        r.pid, launchMs);

    // 4. Wait for the DLL to connect to the IPC server. The DLL connects
    // on its first hooked game frame, so a timeout here means one of two
    // very different things — check whether the game is even alive before
    // reporting, otherwise a boot crash and a slow boot produce the same
    // misleading "DLL did not connect" error.
    if (!ipc_server_.wait_for_connection(kIpcConnectTimeoutMs)) {
        if (!launcher_.is_alive()) {
            r.error_message =
                "Game crashed during boot (PID " + std::to_string(r.pid) +
                " exited before the IPC handshake — the hook never saw a "
                "first game frame). Check Windows Event Viewer > Windows "
                "Logs > Application for an MBAA.exe error at this time "
                "(faulting module + exception code: 0xc000001d = illegal "
                "instruction, 0xc0000005 = bad memory access), add the "
                "game folder to your antivirus exclusions, and try running "
                "MBAA.exe standalone to see if the game itself boots.";
        } else {
            r.error_message =
                "Game is running but produced no hooked frame within " +
                std::to_string(kIpcConnectTimeoutMs / 1000) +
                " s (DLL IPC handshake never started). Suspects: very slow "
                "boot (HDD/antivirus) or a hook vs MBAA.exe-version "
                "mismatch (offsets target 1.07 Rev.1.4.0). If the game "
                "boots fine standalone, report your MBAA.exe version.";
        }
        cleanup();
        return r;
    }

    // 5. Send the config buffer.
    std::uint8_t buf[common::ipc::config_buffer::kMaxBufferSize];
    std::size_t n = common::ipc::config_buffer::serialize(ipc_cfg, buf, sizeof(buf));
    if (n == 0) {
        r.error_message = "Failed to serialize IPC config buffer";
        cleanup();
        return r;
    }
    if (!ipc_server_.send(buf, n)) {
        r.error_message = "Failed to send IPC config buffer";
        cleanup();
        return r;
    }

    ipc_handshake_done_ = true;
    r.success = true;
    common::logger::info("game_runner: launch OK, PID {}, IPC handshake complete", r.pid);
    return r;
}

bool GameRunner::update() {
    if (!launcher_.is_launched()) return false;

    // Poll for status messages from the DLL (non-blocking).
    if (ipc_server_.is_open()) {
        char buf[256];
        std::size_t got = ipc_server_.try_recv(buf, sizeof(buf));
        if (got > 0) {
            ipc_recv_buffer_.append(buf, got);
            // Process complete lines (newline-delimited protocol).
            std::size_t pos = 0;
            while ((pos = ipc_recv_buffer_.find('\n')) != std::string::npos) {
                std::string line = ipc_recv_buffer_.substr(0, pos);
                ipc_recv_buffer_.erase(0, pos + 1);
                // Parse "STOPPED|<reason>" messages.
                if (line.starts_with("STOPPED|")) {
                    stop_reason_ = line.substr(8);
                    common::logger::info("game_runner: DLL stop reason: {}", stop_reason_);
                    // The DLL sent a stop reason — this means something went
                    // wrong (disconnect, desync, timeout, game closed). The
                    // DLL has stopped its hook but the game process is still
                    // alive (frozen). Kill it immediately so the user doesn't
                    // have to manually close a frozen game window.
                    if (launcher_.is_alive()) {
                        common::logger::info("game_runner: auto-killing game after STOPPED signal");
                        launcher_.terminate();
                    }
                }
            }
        }
    }

    if (!launcher_.is_alive()) {
        common::logger::info("game_runner: PID {} exited", launcher_.pid());
        if (!stop_reason_.empty()) {
            common::logger::info("game_runner: stop reason: {}", stop_reason_);
        }
        // Cleanup.
        ipc_server_.close();
        launcher_.terminate();  // safe — already exited, just frees handles
        ipc_handshake_done_ = false;
        pipe_name_.clear();
        return false;
    }
    return true;
}

void GameRunner::force_kill_sync() {
    if (!launcher_.is_launched()) return;
    common::logger::info("game_runner: force-killing PID {}", launcher_.pid());
    launcher_.terminate();
    ipc_server_.close();
    ipc_handshake_done_ = false;
    pipe_name_.clear();
}

} // namespace caster::exe::launcher
