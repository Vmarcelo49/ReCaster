// src/common/win32/env.cpp

#include "env.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <system_error>

namespace caster::common::win32::env {

void set(const std::string& name, const std::string& value) {
    SetEnvironmentVariableA(name.c_str(), value.c_str());
}

std::string get(const std::string& name) {
    char buf[32768] = {0};  // max env var size on Windows
    DWORD len = GetEnvironmentVariableA(name.c_str(), buf, sizeof(buf));
    if (len == 0 || len >= sizeof(buf)) return {};
    return std::string(buf, len);
}

std::filesystem::path local_app_data() {
    char buf[MAX_PATH] = {0};
    DWORD len = GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        // Fallback for very old Windows or unusual environments.
        return std::filesystem::current_path() / "caster-local";
    }
    return std::filesystem::path(buf);
}

std::filesystem::path log_path() {
    auto base = local_app_data() / "caster";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    if (ec) {
        return std::filesystem::current_path() / "caster-debug.log";
    }
    return base / "debug.log";
}

} // namespace caster::common::win32::env
