// src/common/string_utils.hpp
//
// Shared string helpers. Header-only.

#pragma once

#include <string>
#include <string_view>

namespace caster::common {

// Trim leading/trailing whitespace from a string view.
// Default whitespace: space, tab, CR, LF.
inline std::string trim(std::string_view s,
                        std::string_view ws = " \t\r\n") {
    size_t a = 0, b = s.size();
    while (a < b && ws.find(s[a]) != std::string_view::npos) ++a;
    while (b > a && ws.find(s[b-1]) != std::string_view::npos) --b;
    return std::string(s.substr(a, b - a));
}

} // namespace caster::common
