// src/exe/launcher/game_runner.hpp
//
// GameRunner — high-level orchestration of the game launch lifecycle.
// Owns a WindowsLauncher + IpcServer instance across frames.
//
// Used by the MainMenu to:
//   1. Generate the pipe name (PID-based, unique per launcher process)
//   2. Set CASTER_PIPE env var (so the injected DLL can find the pipe)
//   3. Call WindowsLauncher::launch() (CreateProcessW suspended + inject + resume)
//   4. Wait for the DLL to connect to the IPC server
//   5. Send the config_buffer message with the launch parameters
//   6. Poll is_alive() each frame while in the InGame UI state
//   7. On user Force-Kill or natural exit: cleanup and return to Idle

#pragma once

#include "launcher.hpp"
#include "../../common/config.hpp"
#include "../../common/ipc/config_buffer.hpp"
#include "../../common/ipc/ipc_server.hpp"
#include "../../common/ipc/pipe_name.hpp"

#include <cstdint>
#include <string>

namespace caster::exe::launcher {

struct LaunchOfflineParams {
    bool training = true;             // true = Training, false = Versus
    // Other fields (port, is_netplay_host) are irrelevant for offline mode
    // but are part of the IPC v3 wire format. We default them sensibly.
};

struct LaunchResult {
    bool        success       = false;
    std::uint32_t pid         = 0;
    std::string  error_message;
};

class GameRunner {
public:
    GameRunner() = default;
    ~GameRunner();

    GameRunner(const GameRunner&)            = delete;
    GameRunner& operator=(const GameRunner&) = delete;
    GameRunner(GameRunner&&)                 = delete;
    GameRunner& operator=(GameRunner&&)      = delete;

    // Launch the game in offline mode (Training or Versus). Resolves
    // MBAA.exe and hook.dll paths from `cfg` + the exe directory.
    //
    // On success: PID is filled, IPC handshake completed, env var set.
    // On failure: error_message is filled; state is rolled back.
    LaunchResult launch_offline(const common::config::Config& cfg,
                                const LaunchOfflineParams& params);

    // Call this every frame while the game is supposed to be running.
    // Returns true while the game is alive; false once it exits.
    // When it returns false, the runner has already cleaned up.
    bool update();

    // Force-kill the game and cleanup. Safe to call multiple times.
    void force_kill();

    // True between successful launch_offline() and the next cleanup().
    bool is_running() const { return launcher_.is_launched(); }

    // PID of the running game (0 if not running).
    std::uint32_t pid() const { return launcher_.pid(); }

    // True if the IPC handshake completed (DLL received our config).
    bool ipc_handshake_done() const { return ipc_handshake_done_; }

private:
    // Resolve MBAA.exe path. Returns empty string on failure.
    // Priority:
    //   1. cfg.game_dir (if non-empty)
    //   2. <exe_dir>/game/MBAA.exe (if it exists)
    //   3. <exe_dir>/MBAA.exe (if it exists)
    std::string resolve_game_exe(const common::config::Config& cfg) const;

    // Resolve hook.dll path: always next to caster.exe.
    std::string resolve_hook_dll() const;

    // Common launch logic shared by offline (and, in Phase 8, netplay).
    // `pipe_name` is generated here; env var is set; launcher + IPC run.
    LaunchResult launch_internal(
        const std::string& game_exe,
        const std::string& dll_path,
        const std::string& working_dir,
        bool high_priority,
        const common::ipc::config_buffer::Config& ipc_cfg);

    WindowsLauncher               launcher_;
    common::ipc::IpcServer        ipc_server_;
    std::string                   pipe_name_;
    bool                          ipc_handshake_done_ = false;
};

} // namespace caster::exe::launcher
