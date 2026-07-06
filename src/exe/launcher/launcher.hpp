// src/exe/launcher/launcher.hpp
//
// WindowsLauncher — low-level class that owns a launched game process.
//
// Flow:
//   1. CreateProcessW(CREATE_SUSPENDED | HIGH_PRIORITY_CLASS)
//   2. Read PE header for diagnostic logging (entry-point RVA, image size)
//   3. Inject hook.dll via LoadLibraryW + CreateRemoteThread
//   4. apply_game_patches() — STUB for now; real ASM patches for MBAACC
//      come in a future phase
//   5. ResumeThread on the main thread
//
// After launch(), the launcher owns the process + thread handles and
// cleans them up in ~WindowsLauncher() or via terminate().
//
// Threading: not thread-safe. Caller (GameRunner) must serialize calls.

#pragma once

#include "../../common/win32/process.hpp"

#include <cstdint>
#include <string>

namespace caster::exe::launcher {

struct LaunchConfig {
    std::string game_exe_path;   // absolute path to MBAA.exe (UTF-8)
    std::string dll_path;        // absolute path to hook.dll (UTF-8)
    std::string working_dir;     // empty = inherit from parent
    bool        high_priority    = true;
};

class WindowsLauncher {
public:
    WindowsLauncher() = default;
    ~WindowsLauncher();

    WindowsLauncher(const WindowsLauncher&)            = delete;
    WindowsLauncher& operator=(const WindowsLauncher&) = delete;
    WindowsLauncher(WindowsLauncher&&)                 = delete;
    WindowsLauncher& operator=(WindowsLauncher&&)      = delete;

    // Launch the game in suspended state, inject the DLL, apply patches,
    // resume. Returns true on success. On failure, fills `error_message`.
    bool launch(const LaunchConfig& cfg, std::string& error_message);

    // True if launch() succeeded and the process hasn't been terminated
    // or detected as exited.
    bool is_launched() const { return launched_; }

    // Poll the OS to check if the process is still running.
    // Returns false if not launched or if the process has exited.
    bool is_alive() const;

    // Forcefully terminate the process. Closes handles. After this,
    // is_launched() returns false.
    void terminate();

    // Get the PID of the launched process (0 if not launched).
    std::uint32_t pid() const { return pid_; }

private:
    // Release process + thread handles and reset state.
    void reset();

    common::win32::process::ProcessHandle proc_handle_ =
        common::win32::process::kInvalidHandle;
    void*         thread_handle_ = nullptr;
    std::uint32_t pid_           = 0;
    bool          launched_      = false;
};

// Apply game-specific ASM patches to the suspended process.
//
// STUB in Phase 5: the real MBAACC patches (skip config dialog etc.)
// will be added in a future phase. The offsets are version-specific:
//   0x04A1D42 ← [0xEB, 0x0E]  (JMP +14)
//   0x04A1D4A ← [0xEB]        (JMP short)
// Image base for MBAACC: 0x00400000 (standard for 32-bit exes).
//
// Returns true on success. The stub always returns true.
bool apply_game_patches(common::win32::process::ProcessHandle proc);

} // namespace caster::exe::launcher
