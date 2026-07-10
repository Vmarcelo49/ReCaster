// src/dll/util/string_utils.cpp

#include "string_utils.hpp"

namespace caster::dll {

std::string trimmed(std::string str, const std::string& ws) {
    size_t endpos = str.find_last_not_of(ws);
    if (std::string::npos != endpos) str = str.substr(0, endpos + 1);
    size_t startpos = str.find_first_not_of(ws);
    if (std::string::npos != startpos) str = str.substr(startpos);
    return str;
}

} // namespace caster::dll
