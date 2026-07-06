// src/dll/string_utils.cpp

#include "string_utils.hpp"

namespace caster::dll {

std::string formatAsHex(const std::string& bytes) {
    if (bytes.empty()) return "";
    std::string str;
    for (unsigned char c : bytes) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02x ", c);
        str += buf;
    }
    return str.substr(0, str.size() - 1);
}

std::string formatAsHex(const void* bytes, size_t len) {
    if (len == 0) return "";
    auto p = static_cast<const unsigned char*>(bytes);
    std::string str;
    for (size_t i = 0; i < len; ++i) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02x ", p[i]);
        str += buf;
    }
    return str.substr(0, str.size() - 1);
}

std::string trimmed(std::string str, const std::string& ws) {
    size_t endpos = str.find_last_not_of(ws);
    if (std::string::npos != endpos) str = str.substr(0, endpos + 1);
    size_t startpos = str.find_first_not_of(ws);
    if (std::string::npos != startpos) str = str.substr(startpos);
    return str;
}

std::vector<std::string> split(const std::string& str, const std::string& delim) {
    std::vector<std::string> result;
    std::string copy = str;
    for (;;) {
        size_t i = copy.find_first_of(delim);
        if (i == std::string::npos) break;
        result.push_back(copy.substr(0, i));
        copy = copy.substr(i + delim.size());
    }
    result.push_back(copy);
    return result;
}

} // namespace caster::dll
