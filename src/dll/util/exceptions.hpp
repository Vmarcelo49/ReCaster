// src/dll/util/exceptions.hpp
// Ported from CCCaster lib/Exceptions.hpp. Uses our logger.

#pragma once

#include "string_utils.hpp"
#include "../common/logger.hpp"

#include <string>

namespace caster::dll {

struct Exception {
    std::string debug, user;
    Exception() = default;
    Exception(const std::string& debug, const std::string& user)
        : debug(debug), user(user.empty() ? debug : user) {}
    virtual std::string str() const;
};

struct WinException : public Exception {
    int code = 0;
    std::string desc;
    WinException() = default;
    WinException(int code, const std::string& debug, const std::string& user)
        : Exception(debug, user), code(code), desc(getAsString(code)) {}
    std::string str() const override;
    static std::string getAsString(int windowsErrorCode);
    static std::string getLastError();
    static std::string getLastSocketError();
};

} // namespace caster::dll
