// src/dll/util/string_utils.hpp
// Ported from CCCaster lib/StringUtils.hpp. Uses std::format where applicable.

#pragma once

#include <cctype>
#include <string>

namespace caster::dll {

std::string trimmed(std::string str, const std::string& ws = " \t\r\n");

inline std::string lowerCase(std::string str) {
    for (char& c : str) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return str;
}

} // namespace caster::dll
