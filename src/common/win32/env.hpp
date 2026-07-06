// src/common/win32/env.hpp
//
// Win32 environment variable helpers + path resolution. We use
// SetEnvironmentVariableA (not putenv) so the child process inherits
// the variable via CreateProcess.

#pragma once

#include <filesystem>
#include <string>

namespace caster::common::win32::env {

// Set an environment variable in the current process. Child processes
// created via CreateProcess will inherit it.
void set(const std::string& name, const std::string& value);

// Get an environment variable. Returns empty string if not set.
std::string get(const std::string& name);

// Resolve %LOCALAPPDATA% (e.g. C:\Users\<user>\AppData\Local).
// Falls back to <CWD>/caster-local if LOCALAPPDATA is unset (rare).
std::filesystem::path local_app_data();

// Resolve the log path: <LOCALAPPDATA>\caster\debug.log.
// Falls back to <CWD>/caster-debug.log if LOCALAPPDATA is missing or
// the directory can't be created.
std::filesystem::path log_path();

} // namespace caster::common::win32::env
