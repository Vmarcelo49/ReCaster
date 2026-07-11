// src/exe/launcher/game_runner.cpp
//
// GameRunner implementation — worker-thread edition (Layer 2).
//
// All launch/kill/IPC operations run on the internal `std::jthread`.
// The UI thread enqueues commands via `*_async()` and reads state via
// `snapshot()`.

#include "game_runner.hpp"
#include "../../common/config.hpp"
#include "../../common/ipc/config_buffer.hpp"
#include "../../common/ipc/pipe_name.hpp"
#include "../../common/logger.hpp"
#include "../../common/win32/env.hpp"

#include <SDL2/SDL.h>

#include <chrono>
#include <filesystem>
#include <thread>
#include <utility>

namespace fs = std::filesystem;

namespace caster::exe::launcher {

namespace {

// Wait this long (ms) for the DLL to connect to the IPC server after
// we resume the main thread. The DLL's worker thread needs time to:
//   1. Run DllMain
//   2. Spawn its IPC receiver thread
//   3. Connect to the pipe
// 10 s is generous; real-world should be <1 s.
constexpr std::uint32_t kIpcConnectTimeoutMs = 10000;

// Worker loop sleep between updates. ~60fps is responsive enough for
// detecting game exit and IPC messages.
constexpr auto kWorkerSleep = std::chrono::milliseconds(16);

} // namespace

// ============================================================================
// Construction / destruction
// ============================================================================

GameRunner::GameRunner()
    : worker_([this](std::stop_token st) { worker_loop(st); }) {
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

void GameRunner::launch_offline_async(const common::config::Config& cfg,
                                      const LaunchOfflineParams& params) {
    commands_.push(game_runner_command::LaunchOffline{cfg, params});
}

void GameRunner::launch_after_handshake_async(
    const common::config::Config& cfg,
    const session::NetplayConfig& np_cfg) {
    commands_.push(game_runner_command::LaunchAfterHandshake{cfg, np_cfg});
}

void GameRunner::force_kill_async() {
    commands_.push(game_runner_command::ForceKill{});
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
            // Don't launch if already running.
            if (launcher_.is_launched()) {
                last_error_ = "Game already running (PID " +
                              std::to_string(launcher_.pid()) + ")";
                return;
            }
            launch_in_progress_ = true;
            last_error_.clear();
            publish_snapshot();

            // Resolve paths.
            std::string game_exe = resolve_game_exe(c.cfg);
            if (game_exe.empty()) {
                last_error_ = "MBAA.exe not found. Place it in the same folder "
                              "as caster.exe (or set game_dir in caster/config.ini).";
                launch_in_progress_ = false;
                return;
            }
            std::string dll_path = resolve_hook_dll();
            if (!fs::exists(dll_path)) {
                last_error_ = "hook.dll not found at " + dll_path;
                launch_in_progress_ = false;
                return;
            }
            std::string working_dir = fs::path(game_exe).parent_path().string();

            // Build the IPC config buffer for offline mode.
            common::ipc::config_buffer::Config ipc_cfg;
            ipc_cfg.flags = common::ipc::config_buffer::kFlagTraining;  // bit0
            if (!c.params.training) {
                ipc_cfg.flags = 0;
            }
            ipc_cfg.delay          = 0;
            ipc_cfg.rollback       = static_cast<std::uint8_t>(c.cfg.default_rollback);
            ipc_cfg.win_count      = static_cast<std::uint8_t>(c.cfg.versus_win_count);
            ipc_cfg.host_player    = 1;
            ipc_cfg.peer_port      = 0;
            ipc_cfg.local_udp_port = 0;
            ipc_cfg.match_seed     = 0;
            ipc_cfg.peer_addr      = "";

            auto r = launch_internal(game_exe, dll_path, working_dir,
                                     c.cfg.high_cpu_priority, ipc_cfg);
            if (!r.success) {
                last_error_ = r.error_message;
            }
            launch_in_progress_ = false;
        } else if constexpr (std::is_same_v<T, LaunchAfterHandshake>) {
            if (launcher_.is_launched()) {
                last_error_ = "Game already running (PID " +
                              std::to_string(launcher_.pid()) + ")";
                return;
            }
            launch_in_progress_ = true;
            last_error_.clear();
            publish_snapshot();

            // 1. Legacy 1s sleep to let the OS release the UDP port after
            //    the session's ENet/relay teardown.
            common::logger::info("game_runner: sleeping 1s to release UDP port...");
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // 2. Resolve paths.
            std::string game_exe = resolve_game_exe(c.cfg);
            if (game_exe.empty()) {
                last_error_ = "MBAA.exe not found. Place it in the same folder "
                              "as caster.exe (or set game_dir in caster/config.ini).";
                launch_in_progress_ = false;
                return;
            }
            std::string dll_path = resolve_hook_dll();
            if (!fs::exists(dll_path)) {
                last_error_ = "hook.dll not found at " + dll_path;
                launch_in_progress_ = false;
                return;
            }
            std::string working_dir = fs::path(game_exe).parent_path().string();

            // 3. Build the IPC config buffer from the NetplayConfig snapshot.
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
            ipc_cfg.delay          = c.np_cfg.delay;
            ipc_cfg.rollback       = c.np_cfg.rollback;
            ipc_cfg.win_count      = c.np_cfg.win_count;
            ipc_cfg.host_player    = c.np_cfg.host_player;
            ipc_cfg.peer_port      = c.np_cfg.peer_port;
            ipc_cfg.local_udp_port = c.np_cfg.local_udp_port;
            ipc_cfg.match_seed     = c.np_cfg.match_seed;
            ipc_cfg.peer_addr      = c.np_cfg.peer_addr;

            common::logger::info(
                "game_runner: launching netplay game (host={} delay={} rollback={} "
                "win={} peer={}:{} local_udp={} seed=0x{:08x})",
                c.np_cfg.is_host, ipc_cfg.delay, ipc_cfg.rollback,
                ipc_cfg.win_count, c.np_cfg.peer_addr, c.np_cfg.peer_port,
                c.np_cfg.local_udp_port, c.np_cfg.match_seed);

            auto r = launch_internal(game_exe, dll_path, working_dir,
                                     c.cfg.high_cpu_priority, ipc_cfg);
            if (!r.success) {
                last_error_ = r.error_message;
            }
            launch_in_progress_ = false;
        } else if constexpr (std::is_same_v<T, ForceKill>) {
            force_kill_sync();
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
    const char* base = SDL_GetBasePath();
    if (base) {
        fs::path flat = fs::path(base) / "MBAA.exe";
        common::logger::info("game_runner: checking exe dir: {}", flat.string());
        if (fs::exists(flat)) return flat.string();

        // 3. <exe_dir>/game/MBAA.exe (alternative subfolder layout).
        fs::path game_subdir = fs::path(base) / "game" / "MBAA.exe";
        common::logger::info("game_runner: checking exe/game: {}", game_subdir.string());
        if (fs::exists(game_subdir)) return game_subdir.string();
    } else {
        common::logger::warn("game_runner: SDL_GetBasePath() returned null");
    }

    // 4. Current working directory.
    fs::path cwd_flat = fs::current_path() / "MBAA.exe";
    common::logger::info("game_runner: checking CWD: {}", cwd_flat.string());
    if (fs::exists(cwd_flat)) return cwd_flat.string();

    common::logger::err("game_runner: MBAA.exe not found in exe dir, game/ subdir, or CWD");

    return {};
}

std::string GameRunner::resolve_hook_dll() const {
    const char* base = SDL_GetBasePath();
    if (base) {
        return (fs::path(base) / "hook.dll").string();
    }
    return fs::absolute("hook.dll").string();
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

    LaunchResult r;

    // 1. Generate pipe name and set env var so the DLL can find it.
    pipe_name_ = common::ipc::pipe_name::for_current_process();
    common::win32::env::set(common::ipc::pipe_name::kEnvVarName, pipe_name_);
    common::logger::info("game_runner: pipe = {}", pipe_name_);

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
        ipc_server_.close();
        pipe_name_.clear();
        return r;
    }
    r.pid = launcher_.pid();

    // 4. Wait for the DLL to connect to the IPC server.
    if (!ipc_server_.wait_for_connection(kIpcConnectTimeoutMs)) {
        r.error_message = "DLL did not connect to IPC server within " +
                          std::to_string(kIpcConnectTimeoutMs) + " ms";
        // The game is running but uninitalized — kill it.
        launcher_.terminate();
        ipc_server_.close();
        pipe_name_.clear();
        return r;
    }

    // 5. Send the config buffer.
    std::uint8_t buf[common::ipc::config_buffer::kMaxBufferSize];
    std::size_t n = common::ipc::config_buffer::serialize(ipc_cfg, buf, sizeof(buf));
    if (n == 0) {
        r.error_message = "Failed to serialize IPC config buffer";
        launcher_.terminate();
        ipc_server_.close();
        pipe_name_.clear();
        return r;
    }
    if (!ipc_server_.send(buf, n)) {
        r.error_message = "Failed to send IPC config buffer";
        launcher_.terminate();
        ipc_server_.close();
        pipe_name_.clear();
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
