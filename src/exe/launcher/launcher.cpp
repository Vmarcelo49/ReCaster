// src/exe/launcher/launcher.cpp

#include "launcher.hpp"
#include "../../common/win32/memory.hpp"
#include "../../common/win32/pe_parser.hpp"
#include "../../common/logger.hpp"

#include <string>

namespace caster::exe::launcher {

WindowsLauncher::~WindowsLauncher() {
    terminate();
}

void WindowsLauncher::reset() {
    if (thread_handle_) {
        common::win32::process::close_handle(thread_handle_);
        thread_handle_ = nullptr;
    }
    if (proc_handle_ != common::win32::process::kInvalidHandle) {
        common::win32::process::close_handle(
            reinterpret_cast<void*>(proc_handle_));
        proc_handle_ = common::win32::process::kInvalidHandle;
    }
    pid_      = 0;
    launched_ = false;
}

bool WindowsLauncher::launch(const LaunchConfig& cfg,
                             std::string& error_message) {
    using namespace common::win32;

    if (launched_) {
        error_message = "WindowsLauncher::launch: already launched";
        return false;
    }
    if (cfg.game_exe_path.empty()) {
        error_message = "WindowsLauncher::launch: empty game_exe_path";
        return false;
    }
    if (cfg.dll_path.empty()) {
        error_message = "WindowsLauncher::launch: empty dll_path";
        return false;
    }

    // 1. CreateProcessW(CREATE_SUSPENDED).
    auto r = process::create_suspended(cfg.game_exe_path,
                                       cfg.working_dir,
                                       cfg.high_priority);
    if (!r.success) {
        error_message = r.error_message;
        return false;
    }

    proc_handle_   = r.process_handle;
    thread_handle_ = r.thread_handle;
    pid_           = r.pid;
    common::logger::info("launcher: created PID {} suspended", pid_);

    // 2. Read PE header for diagnostic logging (matches zzcaster behavior).
    //    MBAACC image base is the standard 0x00400000 for 32-bit exes.
    constexpr std::uint32_t kImageBase = 0x00400000;
    auto pe = pe_parser::read(proc_handle_, kImageBase);
    if (pe.valid) {
        common::logger::info("launcher: PE entry-point RVA = 0x{:08x}, image size = {} bytes",
                     pe.entry_point_rva, pe.image_size);
        // Entry point VA = image_base + entry_point_rva. Useful to know
        // for setting breakpoints or verifying patches landed correctly.
        const std::uint32_t entry_va = kImageBase + pe.entry_point_rva;
        common::logger::info("launcher: entry-point VA = 0x{:08x}", entry_va);
    } else {
        common::logger::warn("launcher: PE parse failed: {}", pe.error_message);
        // Non-fatal — we proceed even if we couldn't read the header.
    }

    // 3. Inject hook.dll via LoadLibraryW + CreateRemoteThread.
    std::string inject_err;
    std::uintptr_t hmodule = memory::inject_dll_w(proc_handle_, cfg.dll_path,
                                                   10000, inject_err);
    if (hmodule == 0) {
        error_message = "DLL injection failed: " + inject_err;
        // Kill the suspended process so we don't leave a zombie.
        process::terminate(proc_handle_);
        reset();
        return false;
    }
    common::logger::info("launcher: hook.dll injected (HMODULE=0x{:x})", hmodule);

    // 4. Apply game-specific ASM patches (currently a no-op).
    if (!apply_game_patches(proc_handle_)) {
        common::logger::warn("launcher: apply_game_patches returned false (non-fatal)");
    }

    // 5. Resume the main thread — game starts executing with hook.dll
    //    already loaded.
    if (!process::resume_thread(thread_handle_)) {
        error_message = "ResumeThread failed";
        process::terminate(proc_handle_);
        reset();
        return false;
    }
    common::logger::info("launcher: main thread resumed, game is running");

    launched_ = true;
    return true;
}

bool WindowsLauncher::is_alive() const {
    if (!launched_) return false;
    return common::win32::process::is_alive(proc_handle_);
}

void WindowsLauncher::terminate() {
    if (!launched_) {
        // Even if not launched, make sure handles are clean.
        reset();
        return;
    }

    if (is_alive()) {
        common::logger::info("launcher: terminating PID {}", pid_);
        common::win32::process::terminate(proc_handle_);
    }
    reset();
}

bool apply_game_patches(common::win32::process::ProcessHandle proc) {
    // Currently a no-op. Real MBAACC patches (skip config dialog etc.) will
    // be added later — see docs/non-implemented-stubs.md.
    //
    // Known zzcaster patches (NOT applied yet):
    //   0x04A1D42 ← [0xEB, 0x0E]  (JMP +14, skip config dialog 1)
    //   0x04A1D4A ← [0xEB]        (JMP short, skip config dialog 2)
    //
    // Image base for MBAACC: 0x00400000 (standard for 32-bit exes).
    // Patch addresses above are absolute VAs.

    common::logger::info("apply_game_patches: no patches applied (stub)");
    (void)proc;  // silence unused-parameter warning
    return true;
}

} // namespace caster::exe::launcher
