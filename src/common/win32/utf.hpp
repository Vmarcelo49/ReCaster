// src/common/win32/utf.hpp
//
// UTF-8 ↔ UTF-16 conversion helpers for Win32 APIs that require
// wide strings (CreateNamedPipeW, CreateFileW, CreateProcessW, etc.).

#pragma once

#include <string>

namespace caster::common::win32::utf {

// Convert a UTF-8 string to a UTF-16LE wide string.
// Returns empty string on empty input or invalid UTF-8.
std::wstring utf8_to_wide(const std::string& s);

} // namespace caster::common::win32::utf
