// src/common/ipc/pipe_name.cpp

#include "pipe_name.hpp"
#include "../win32/env.hpp"
#include "../win32/process.hpp"

#include <format>

namespace caster::common::ipc::pipe_name {

std::string for_pid(unsigned pid) {
    return std::format("\\\\.\\pipe\\caster_{}_pipe", pid);
}

std::string for_current_process() {
    return for_pid(win32::process::current_pid());
}

std::string from_env() {
    return win32::env::get(kEnvVarName);
}

} // namespace caster::common::ipc::pipe_name
