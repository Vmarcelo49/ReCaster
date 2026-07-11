// src/exe/launcher/game_runner.hpp
//
// GameRunner — high-level orchestration of the game launch lifecycle.
// Owns a WindowsLauncher + IpcServer instance, runs on a dedicated
// worker thread (Layer 2 of the threading migration — see
// docs/threading-migration.md).
//
// Threading model:
//   - All launch/kill/IPC operations happen on the internal `std::jthread`
//     (the "game runner worker"). The UI thread never touches the
//     WindowsLauncher or IpcServer directly.
//   - The UI thread enqueues commands via `*_async()` methods and reads
//     state via `snapshot()` (a cheap copy under a mutex).
//   - The worker drains commands, runs `update()` (poll is_alive + IPC),
//     and updates the snapshot under the mutex.
//
// Used by the MainMenu to:
//   1. Launch the game (offline or netplay) — now async, UI doesn't block
//      on the 1-2s CreateProcess + inject + IPC handshake.
//   2. Poll for natural exit / DLL stop messages — worker does this
//      continuously, UI just reads the snapshot.
//   3. Force-kill — async, worker does the TerminateProcess.

#pragma once

#include "launcher.hpp"
#include "../../common/concurrency.hpp"
#include "../../common/config.hpp"
#include "../../common/ipc/config_buffer.hpp"
#include "../../common/ipc/ipc_server.hpp"
#include "../../common/ipc/pipe_name.hpp"
#include "../session/netplay_config.hpp"

#include <cstdint>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <variant>

namespace caster::exe::launcher {

struct LaunchOfflineParams {
    bool training = true;             // true = Training, false = Versus
};

struct LaunchResult {
    bool        success       = false;
    std::uint32_t pid         = 0;
    std::string  error_message;
};

// Immutable snapshot of the game runner state, read by the UI thread.
struct GameRunnerSnapshot {
    bool          is_running         = false;
    std::uint32_t pid                = 0;
    bool          ipc_handshake_done = false;
    std::string   stop_reason;
    std::string   last_error;        // populated if a launch failed
    // True while a launch is in progress (between launch_*_async and the
    // worker finishing the launch). The UI can show a "Launching..." spinner.
    bool          launch_in_progress = false;
};

// Commands enqueued by the UI thread, drained by the game runner worker.
namespace game_runner_command {

struct LaunchOffline {
    common::config::Config cfg;
    LaunchOfflineParams    params;
};
struct LaunchAfterHandshake {
    common::config::Config     cfg;
    session::NetplayConfig     np_cfg;
};
struct ForceKill {};

using Command = std::variant<
    LaunchOffline,
    LaunchAfterHandshake,
    ForceKill
>;

} // namespace game_runner_command

class GameRunner {
public:
    GameRunner();
    ~GameRunner();

    GameRunner(const GameRunner&)            = delete;
    GameRunner& operator=(const GameRunner&) = delete;
    GameRunner(GameRunner&&)                 = delete;
    GameRunner& operator=(GameRunner&&)      = delete;

    // ---- Async command API (enqueue + return immediately) ----

    // Launch the game in offline mode (Training or Versus). Resolves
    // MBAA.exe and hook.dll paths from `cfg` + the exe directory.
    //
    // On success: snapshot().is_running becomes true, pid is filled,
    // ipc_handshake_done becomes true.
    // On failure: snapshot().last_error is filled.
    void launch_offline_async(const common::config::Config& cfg,
                              const LaunchOfflineParams& params);

    // Launch the game AFTER a netplay handshake completes. Takes the
    // NetplayConfig snapshot from the session and sends it via IPC.
    //
    // The caller is responsible for calling session.deinit_async() BEFORE
    // this method (to release the UDP port for rebind). The worker adds a
    // 1s sleep internally to let the OS release the port.
    void launch_after_handshake_async(const common::config::Config& cfg,
                                      const session::NetplayConfig& np_cfg);

    // Force-kill the game and cleanup. Safe to call multiple times.
    void force_kill_async();

    // ---- State access (thread-safe) ----
    //
    // Returns a consistent snapshot of the game runner state. Cheap (one
    // mutex lock + copy). Safe to call from the UI thread.
    GameRunnerSnapshot snapshot() const;

private:
    // ---- Worker thread loop ----
    void worker_loop(std::stop_token st);
    void drain_commands();
    void apply_command(const game_runner_command::Command& cmd);
    void publish_snapshot();

    // ---- Internal helpers (run on the worker thread) ----
    std::string resolve_game_exe(const common::config::Config& cfg) const;
    std::string resolve_hook_dll() const;

    LaunchResult launch_internal(
        const std::string& game_exe,
        const std::string& dll_path,
        const std::string& working_dir,
        bool high_priority,
        const common::ipc::config_buffer::Config& ipc_cfg);

    // Called by the worker each iteration while the game is running.
    // Returns true while the game is alive; false once it exits (and
    // cleans up internally on the false return).
    bool update();

    // Synchronous force-kill (called from apply_command on the worker).
    void force_kill_sync();

    // ---- State (only touched from the worker thread) ----
    WindowsLauncher               launcher_;
    common::ipc::IpcServer        ipc_server_;
    std::string                   pipe_name_;
    bool                          ipc_handshake_done_ = false;
    std::string                   stop_reason_;
    std::string                   ipc_recv_buffer_;
    bool                          launch_in_progress_ = false;
    std::string                   last_error_;

    // ---- Threading machinery ----
    common::concurrency::BlockingQueue<game_runner_command::Command> commands_;
    std::jthread   worker_;

    // ---- Snapshot (read by UI, written by worker) ----
    mutable std::mutex       snapshot_mutex_;
    GameRunnerSnapshot       snapshot_;
};

} // namespace caster::exe::launcher
