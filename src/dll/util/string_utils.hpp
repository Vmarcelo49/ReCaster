// src/dll/util/string_utils.hpp
// Ported from CCCaster lib/StringUtils.hpp. Uses std::format where applicable.

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace caster::dll {

std::string formatAsHex(const std::string& bytes);
std::string formatAsHex(const void* bytes, size_t len);

std::string trimmed(std::string str, const std::string& ws = " \t\r\n");
std::vector<std::string> split(const std::string& str, const std::string& delim = " ");

inline std::string lowerCase(std::string str) {
    for (char& c : str) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return str;
}

inline std::string upperCase(std::string str) {
    for (char& c : str) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return str;
}

template<typename T>
inline std::string format(const T& val) {
    std::stringstream ss;
    ss << val;
    return ss.str();
}

template<typename T>
inline T lexical_cast(const std::string& str, T fallback = 0) {
    T val;
    std::stringstream ss(str);
    ss >> val;
    if (ss.fail()) return fallback;
    return val;
}

inline std::string normalizeWindowsPath(std::string path) {
    path = path.substr(0, path.find_last_of("/\\"));
    std::replace(path.begin(), path.end(), '/', '\\');
    if (!path.empty() && path.back() != '\\') path += '\\';
    return path;
}

} // namespace caster::dll
