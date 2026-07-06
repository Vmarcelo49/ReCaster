// src/exe/launcher/game_runner.cpp

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

} // namespace

GameRunner::~GameRunner() {
    force_kill();
}

std::string GameRunner::resolve_game_exe(
    const common::config::Config& cfg) const {

    // 1. cfg.game_dir overrides everything.
    if (!cfg.game_dir.empty()) {
        fs::path p = fs::path(cfg.game_dir) / "MBAA.exe";
        if (fs::exists(p)) return p.string();
        common::logger::warn("game_runner: cfg.game_dir set but MBAA.exe not found at {}",
                     p.string());
    }

    // 2. <exe_dir>/MBAA.exe (same folder as caster.exe — primary layout).
    const char* base = SDL_GetBasePath();
    if (base) {
        fs::path flat = fs::path(base) / "MBAA.exe";
        if (fs::exists(flat)) return flat.string();

        // 3. <exe_dir>/game/MBAA.exe (alternative subfolder layout).
        fs::path game_subdir = fs::path(base) / "game" / "MBAA.exe";
        if (fs::exists(game_subdir)) return game_subdir.string();
    }

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

LaunchResult GameRunner::launch_offline(const common::config::Config& cfg,
                                        const LaunchOfflineParams& params) {
    LaunchResult r;

    if (launcher_.is_launched()) {
        r.error_message = "Game already running (PID " +
                          std::to_string(launcher_.pid()) + ")";
        return r;
    }

    // Resolve paths.
    std::string game_exe = resolve_game_exe(cfg);
    if (game_exe.empty()) {
        r.error_message = "MBAA.exe not found. Place it in the same folder "
                          "as caster.exe (or set game_dir in caster/config.ini).";
        return r;
    }
    std::string dll_path = resolve_hook_dll();
    if (!fs::exists(dll_path)) {
        r.error_message = "hook.dll not found at " + dll_path;
        return r;
    }

    // Working directory: the game's folder (so the game can find its
    // data files relative to its own exe).
    std::string working_dir = fs::path(game_exe).parent_path().string();

    // Build the IPC config buffer for offline mode.
    common::ipc::config_buffer::Config ipc_cfg;
    ipc_cfg.flags = common::ipc::config_buffer::kFlagTraining;  // bit0
    if (!params.training) {
        // Versus mode = training bit cleared. The DLL treats both as
        // "offline, no netplay" — the distinction is for the game itself.
        ipc_cfg.flags = 0;
    }
    ipc_cfg.delay          = 0;
    ipc_cfg.rollback       = static_cast<std::uint8_t>(cfg.default_rollback);
    ipc_cfg.win_count      = static_cast<std::uint8_t>(cfg.versus_win_count);
    ipc_cfg.host_player    = 1;
    ipc_cfg.peer_port      = 0;
    ipc_cfg.local_udp_port = 0;
    ipc_cfg.match_seed     = 0;
    ipc_cfg.peer_addr      = "";

    return launch_internal(game_exe, dll_path, working_dir,
                           cfg.high_cpu_priority, ipc_cfg);
}

bool GameRunner::update() {
    if (!launcher_.is_launched()) return false;
    if (!launcher_.is_alive()) {
        common::logger::info("game_runner: PID {} exited", launcher_.pid());
        // Cleanup.
        ipc_server_.close();
        launcher_.terminate();  // safe — already exited, just frees handles
        ipc_handshake_done_ = false;
        pipe_name_.clear();
        return false;
    }
    return true;
}

void GameRunner::force_kill() {
    if (!launcher_.is_launched()) return;
    common::logger::info("game_runner: force-killing PID {}", launcher_.pid());
    launcher_.terminate();
    ipc_server_.close();
    ipc_handshake_done_ = false;
    pipe_name_.clear();
}

LaunchResult GameRunner::launch_after_handshake(
    const common::config::Config& cfg,
    const session::NetplayConfig& np_cfg) {

    LaunchResult r;

    if (launcher_.is_launched()) {
        r.error_message = "Game already running (PID " +
                          std::to_string(launcher_.pid()) + ")";
        return r;
    }

    // 1. Legacy 1s sleep to let the OS release the UDP port after the
    //    session's ENet/relay teardown. (zzcaster MainApp.cpp:933-934.)
    common::logger::info("game_runner: sleeping 1s to release UDP port...");
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // 2. Resolve paths.
    std::string game_exe = resolve_game_exe(cfg);
    if (game_exe.empty()) {
        r.error_message = "MBAA.exe not found. Place it in the same folder "
                          "as caster.exe (or set game_dir in caster/config.ini).";
        return r;
    }
    std::string dll_path = resolve_hook_dll();
    if (!fs::exists(dll_path)) {
        r.error_message = "hook.dll not found at " + dll_path;
        return r;
    }
    std::string working_dir = fs::path(game_exe).parent_path().string();

    // 3. Build the IPC config buffer from the NetplayConfig snapshot.
    common::ipc::config_buffer::Config ipc_cfg;
    ipc_cfg.flags = common::ipc::config_buffer::kFlagNetplay;
    if (np_cfg.is_host) {
        ipc_cfg.flags |= common::ipc::config_buffer::kFlagHost;
    }
    if (np_cfg.is_training) {
        ipc_cfg.flags |= common::ipc::config_buffer::kFlagTraining;
    }
    if (np_cfg.is_spectator) {
        ipc_cfg.flags |= common::ipc::config_buffer::kFlagSpectator;
    }
    ipc_cfg.delay          = np_cfg.delay;
    ipc_cfg.rollback       = np_cfg.rollback;
    ipc_cfg.win_count      = np_cfg.win_count;
    ipc_cfg.host_player    = np_cfg.host_player;
    ipc_cfg.peer_port      = np_cfg.peer_port;
    ipc_cfg.local_udp_port = np_cfg.local_udp_port;
    ipc_cfg.match_seed     = np_cfg.match_seed;
    ipc_cfg.peer_addr      = np_cfg.peer_addr;

    common::logger::info(
        "game_runner: launching netplay game (host={} delay={} rollback={} "
        "win={} peer={}:{} local_udp={} seed=0x{:08x})",
        np_cfg.is_host, ipc_cfg.delay, ipc_cfg.rollback,
        ipc_cfg.win_count, np_cfg.peer_addr, np_cfg.peer_port,
        np_cfg.local_udp_port, np_cfg.match_seed);

    return launch_internal(game_exe, dll_path, working_dir,
                           cfg.high_cpu_priority, ipc_cfg);
}

} // namespace caster::exe::launcher
