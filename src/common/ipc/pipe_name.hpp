// src/common/ipc/pipe_name.hpp
//
// Generate and resolve the named-pipe path used for launcher ↔ DLL
// communication. The pipe name is unique per launcher process (PID-based)
// so multiple casters can run simultaneously without colliding.
//
// Format: \\.\pipe\caster_<pid>_pipe
// The launcher generates the name, opens the server end, and exports it
// to the spawned game process via the CASTER_PIPE environment variable.
// The DLL (hook.dll) reads that env var on DllMain and connects as client.

#pragma once

#include <string>

namespace caster::common::ipc::pipe_name {

// Generate the pipe path for the current process. The result is suitable
// for CreateNamedPipeW / CreateFileW. Returns something like
// "\\\\.\\pipe\\caster_1234_pipe".
std::string for_current_process();

// Generate the pipe path for a specific PID. Used by the DLL client which
// knows the env var value but not necessarily the PID.
std::string for_pid(unsigned pid);

// Name of the env var we use to pass the pipe path from launcher to the
// game process (which inherits it via CreateProcess, so the injected DLL
// can read it).
inline constexpr const char* kEnvVarName = "CASTER_PIPE";

// Read the pipe path from CASTER_PIPE. Returns empty string if unset
// (which means the DLL was injected into a process that wasn't launched
// by caster.exe — manual injection for debugging).
std::string from_env();

} // namespace caster::common::ipc::pipe_name
