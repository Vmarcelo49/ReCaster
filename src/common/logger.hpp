// src/common/logger.hpp
//
// Thread-safe file logger. Equivalent of zzcaster's `common/logging.zig`,
// ported to C++23. Writes to `<LOCALAPPDATA>\caster\debug.log` (Windows)
// or `./caster-debug.log` (fallback / non-Windows).
//
// Design:
//   - Single shared Logger instance via logger::get() (Meyer's singleton).
//   - Mutex-protected file handle; safe to call from any thread.
//   - Three severities: info, warn, err.
//   - Optional stdout mirror (config.log_to_stdout).
//   - Each line: `[YYYY-MM-DD HH:MM:SS.mmm] [LEVEL] [thread_id] message`.
//
// Usage:
//   #include "logger.hpp"
//   caster::common::logger::info("SDL initialized");
//   caster::common::logger::err("open failed: {}", errno);

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace caster::common::logger {

// Initialize the logger. Must be called once at startup, before any log()
// call. If `path` is empty, falls back to %LOCALAPPDATA%\caster\debug.log
// (Windows) or ./caster-debug.log. If the file can't be opened, logs go
// to stderr only.
void init(std::filesystem::path path = {},
          bool mirror_to_stdout = false);

// Shut down: flush and close. Optional; the destructor takes care of it
// at program exit, but explicit shutdown guarantees the file is closed
// before the OS reclaims the handle.
void shutdown();

// Severity-aware log functions. Use these instead of raw log().
void info(std::string_view msg);
void warn(std::string_view msg);
void err (std::string_view msg);

// fmt-like overloads (compile-time checked via std::format, C++23).
// Example: logger::info("connected to {}:{}", host, port);
template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    info(std::string(std::format(fmt, std::forward<Args>(args)...)));
}
template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    warn(std::string(std::format(fmt, std::forward<Args>(args)...)));
}
template <typename... Args>
void err(std::format_string<Args...> fmt, Args&&... args) {
    err(std::string(std::format(fmt, std::forward<Args>(args)...)));
}

// Where is the log file? Useful for the UI to show the path.
std::filesystem::path current_log_path();

} // namespace caster::common::logger
