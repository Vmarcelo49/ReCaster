// src/exe/injector.cpp

#include "injector.hpp"
#include "../common/win32/process.hpp"
#include "../common/win32/memory.hpp"
#include "../common/logger.hpp"

#include <string>

namespace caster::exe {

uint32_t find_process_by_name(const std::string& name) {
    return caster::common::win32::process::find_by_name(name);
}

InjectionResult inject_dll(uint32_t pid, const std::string& dll_path) {
    using namespace caster::common::win32;

    InjectionResult r;
    r.pid = pid;

    if (pid == 0) {
        r.message = "inject_dll: invalid pid=0";
        return r;
    }
    if (dll_path.empty()) {
        r.message = "inject_dll: empty dll_path";
        return r;
    }

    // 1. Open the target with the rights LoadLibrary injection needs.
    process::ProcessHandle proc = process::open_for_injection(pid);
    if (proc == process::kInvalidHandle) {
        // The wrapper logs the specific Win32 error code; we just surface
        // a user-friendly message here.
        r.message = "OpenProcess failed (pid=" + std::to_string(pid) +
                    ") — try running caster.exe as Administrator";
        return r;
    }

    // 2. Use the wrapper for the classic LoadLibraryW remote-thread
    //    injection. 5 s timeout — enough for the remote thread to
    //    complete DllMain under normal conditions.
    std::string err_msg;
    std::uintptr_t hmodule = memory::inject_dll_w(proc, dll_path,
                                                   5000, err_msg);
    if (hmodule == 0) {
        r.message = err_msg;
        process::close_handle(reinterpret_cast<void*>(proc));
        return r;
    }

    r.success = true;
    r.message = "Injected hook.dll into PID " + std::to_string(pid) +
                " (HMODULE=0x" + std::to_string(hmodule) + ")";

    process::close_handle(reinterpret_cast<void*>(proc));
    return r;
}

} // namespace caster::exe
