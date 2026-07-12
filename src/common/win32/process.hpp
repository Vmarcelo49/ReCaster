// src/common/win32/process.hpp
//
// Thin Win32 wrappers for process management: create suspended, resume,
// terminate, is-alive polling, current PID. Used by the launcher
// and by the existing injector.
//
// All functions are Windows-only; on non-Windows builds they're stubbed
// out so the rest of the codebase still compiles for syntax checks.

#pragma once

#include <cstdint>
#include <string>

namespace caster::common::win32::process {

// Opaque handle to a Win32 process. Internally a HANDLE cast to uintptr_t
// so we don't leak windows.h into this header.
using ProcessHandle = std::uintptr_t;

// Invalid handle sentinel (matches INVALID_HANDLE_VALUE on Windows).
inline constexpr ProcessHandle kInvalidHandle = static_cast<std::uintptr_t>(-1);

// Result of a CreateProcessSuspended call.
struct LaunchResult {
    bool          success       = false;
    ProcessHandle process_handle = kInvalidHandle;  // owned by caller, must Close()
    std::uint32_t pid           = 0;
    std::uint32_t thread_id     = 0;
    void*         thread_handle = nullptr;  // HANDLE of the main thread
    std::string   error_message;
};

// Create a process in suspended state. The caller is expected to do
// additional setup (DLL injection, memory patches) before calling
// ResumeThread() on the returned thread handle.
//
//   exe_path  : absolute or CWD-relative path to the .exe
//   cwd       : working directory (empty = inherit from parent)
//   high_priority : if true, set HIGH_PRIORITY_CLASS on the process
//
// On Windows, sets CREATE_SUSPENDED. The returned LaunchResult owns the
// process and thread handles — caller must Close() them when done.
LaunchResult create_suspended(const std::string& exe_path,
                              const std::string& cwd,
                              bool high_priority);

// Resume the main thread of a process created via create_suspended().
// Returns true on success. The thread handle is NOT closed by this call;
// caller still owns it.
bool resume_thread(void* thread_handle);

// Check if a process is still running. handle must have been obtained
// from create_suspended() or OpenProcess().
bool is_alive(ProcessHandle handle);

// Forcefully terminate a process. Returns true on success.
bool terminate(ProcessHandle handle);

// Close a process / thread handle. Safe to call on kInvalidHandle or nullptr.
void close_handle(void* handle);

// Get the PID of the current process. Needed because std::this_thread::get_id()
// returns a thread ID, not a process ID, and because under Wine the libc
// getpid() returns the Wine process group leader, not the Windows PID.
std::uint32_t current_pid();

// Find a process by its exe name (case-insensitive, e.g. "MBAA.exe").
// Returns 0 if not found. Uses CreateToolhelp32Snapshot under the hood.
// Note: this lives in process.cpp instead of a separate file because it
// shares the same Toolhelp32 plumbing.
std::uint32_t find_by_name(const std::string& name);

// Open an existing process with the given access rights (for injection).
// Returns kInvalidHandle on failure. Caller owns the handle.
ProcessHandle open_for_injection(std::uint32_t pid);

// Suspend all threads of a process. Uses NtSuspendProcess (ntdll) which
// is the simplest and most reliable way to freeze a Win32 process.
// Returns true on success.
//
// The process must have been opened with PROCESS_SUSPEND_RESUME access.
// `handle` should be a handle obtained from create_suspended() (which
// grants all access) or OpenProcess with appropriate rights.
bool suspend_process(ProcessHandle handle);

// Resume all threads of a previously-suspended process. Uses
// NtResumeProcess (ntdll). Returns true on success.
bool resume_process(ProcessHandle handle);

} // namespace caster::common::win32::process
