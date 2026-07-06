// src/dll/exceptions.cpp

#include "exceptions.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

namespace caster::dll {

std::string Exception::str() const {
    if (debug.empty() || debug == user) return user;
    if (user.empty()) return debug;
    return debug + "; " + user;
}

std::string WinException::str() const {
    return "[" + std::to_string(code) + "] '" + desc + "'; " + debug + "; " + user;
}

std::string WinException::getAsString(int windowsErrorCode) {
    char* errorString = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
                   0, windowsErrorCode, 0, reinterpret_cast<LPSTR>(&errorString), 0, 0);
    std::string str = errorString ? trimmed(errorString) : "(null)";
    LocalFree(errorString);
    return str;
}

std::string WinException::getLastError() {
    return getAsString(GetLastError());
}

std::string WinException::getLastSocketError() {
    return getAsString(WSAGetLastError());
}

} // namespace caster::dll
