// src/common/win32/utf.cpp

#include "utf.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace caster::common::win32::utf {

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                  static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                        static_cast<int>(s.size()), out.data(), len);
    return out;
}

} // namespace caster::common::win32::utf
