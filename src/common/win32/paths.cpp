// src/common/win32/paths.cpp

#include "paths.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <vector>

namespace caster::common::win32::paths {

std::optional<std::filesystem::path> exe_dir() {
    // Wide API: immune to non-ASCII install paths (e.g. C:\Users\João\...).
    // Grow the buffer until the path fits (MAX_PATH is not a limit here).
    std::vector<wchar_t> buf(1024);
    DWORD len = 0;
    while ((len = GetModuleFileNameW(nullptr, buf.data(),
                                     static_cast<DWORD>(buf.size()))) >=
           buf.size()) {
        if (buf.size() >= 32768) return std::nullopt;
        buf.resize(buf.size() * 2);
    }
    if (len == 0) return std::nullopt;

    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, buf.data(), len, nullptr,
                                       0, nullptr, nullptr);
    if (utf8_len <= 0) return std::nullopt;
    std::string utf8(static_cast<std::size_t>(utf8_len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf.data(), len, utf8.data(), utf8_len,
                        nullptr, nullptr);
    return std::filesystem::path(utf8).parent_path();
}

std::optional<std::filesystem::path> exe_file(const std::string& filename) {
    auto dir = exe_dir();
    if (!dir) return std::nullopt;
    return *dir / filename;
}

} // namespace caster::common::win32::paths
