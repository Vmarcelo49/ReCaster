// src/dll/ipc/receiver.hpp
//
// DLL-side IPC client. Reads the config_buffer message that the launcher
// sent over the named pipe.
//
// Lifecycle (called from a worker thread in dll_main.cpp, BEFORE other
// worker threads start, so they can read the received config):
//   1. Read CASTER_PIPE env var to find the pipe name
//   2. Connect to the pipe (with timeout)
//   3. Receive the config_buffer bytes
//   4. Deserialize into a config_buffer::Config
//   5. Store in a global atomic so other code can read it
//
// If CASTER_PIPE is unset (DLL was injected manually for debugging), this
// module reports "no config" and the DLL continues with defaults.

#pragma once

#include "ipc/config_buffer.hpp"  // via caster_common's PUBLIC include dir (src/common)

#include <atomic>
#include <mutex>
#include <string>

namespace caster::dll::ipc_receiver {

// Try to receive the config from the launcher. Blocks up to `timeout_ms`.
// Returns true on success; the config is then available via get_config().
//
// Safe to call from any thread (typically the GUI worker thread before
// it constructs its SDL2 window).
bool receive(std::uint32_t timeout_ms);

// True if receive() succeeded. Other threads can poll this.
bool is_ready();

// Get a copy of the received config. Returns true if a config was received;
// false (and an unchanged `out`) otherwise.
bool get_config(caster::common::ipc::config_buffer::Config& out);

// Get a human-readable status string for UI display.
//   "Not received"      — receive() hasn't been called or failed
//   "Receiving..."      — receive() is in progress (set by receive())
//   "Received: <info>"  — success, with a brief summary
//   "No CASTER_PIPE"    — env var not set (manual injection case)
//   "Error: <message>"  — failure
std::string status_string();

// Send a status notification to the launcher through the IPC pipe.
// The pipe must have been kept open after receive() succeeded. If the
// pipe is closed or was never opened, this is a no-op.
//
// Messages are newline-terminated text. The launcher reads them via
// IpcServer::try_recv() and displays them to the user when the game
// exits abnormally (desync, timeout, disconnect, etc.).
void notify_launcher(const std::string& message);

} // namespace caster::dll::ipc_receiver
