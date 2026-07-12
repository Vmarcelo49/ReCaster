// src/common/win32/process.cpp

#include "process.hpp"
#include "../logger.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <cstring>
#include <vector>

namespace caster::common::win32::process {

namespace {

// Convert a UTF-8 string to a UTF-16LE wide string. Empty input → empty output.
std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                  static_cast<int>(s.size()),
                                  nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                        static_cast<int>(s.size()),
                        out.data(), len);
    return out;
}

} // namespace

LaunchResult create_suspended(const std::string& exe_path,
                              const std::string& cwd,
                              bool high_priority) {
    LaunchResult r;

    std::wstring exe_w   = utf8_to_wide(exe_path);
    std::wstring cwd_w   = utf8_to_wide(cwd);
    if (exe_w.empty()) {
        r.error_message = "create_suspended: empty exe_path";
        return r;
    }

    // CreateProcessW wants a mutable command line. We use just the exe
    // path quoted (so paths with spaces work).
    std::wstring cmdline = L"\"" + exe_w + L"\"";
    std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    DWORD flags = CREATE_SUSPENDED;
    if (high_priority) flags |= HIGH_PRIORITY_CLASS;

    const wchar_t* cwd_ptr = cwd_w.empty() ? nullptr : cwd_w.c_str();

    if (!CreateProcessW(exe_w.c_str(),
                        cmd_buf.data(),
                        nullptr, nullptr, FALSE,
                        flags, nullptr, cwd_ptr,
                        &si, &pi)) {
        DWORD err = GetLastError();
        r.error_message = "CreateProcessW failed (err=" +
                          std::to_string(err) + ") for " + exe_path;
        return r;
    }

    r.success        = true;
    r.process_handle = reinterpret_cast<ProcessHandle>(pi.hProcess);
    r.pid            = pi.dwProcessId;
    r.thread_id      = pi.dwThreadId;
    r.thread_handle  = pi.hThread;
    return r;
}

bool resume_thread(void* thread_handle) {
    if (!thread_handle) return false;
    DWORD prev = ::ResumeThread(thread_handle);
    return prev != static_cast<DWORD>(-1);
}

bool is_alive(ProcessHandle handle) {
    if (handle == kInvalidHandle) return false;
    HANDLE h = reinterpret_cast<HANDLE>(handle);
    DWORD code = 0;
    if (!GetExitCodeProcess(h, &code)) return false;
    return code == STILL_ACTIVE;
}

bool terminate(ProcessHandle handle) {
    if (handle == kInvalidHandle) return false;
    return TerminateProcess(reinterpret_cast<HANDLE>(handle), 0) != 0;
}

void close_handle(void* handle) {
    if (!handle) return;
    ::CloseHandle(handle);
}

std::uint32_t current_pid() {
    return static_cast<std::uint32_t>(GetCurrentProcessId());
}

std::uint32_t find_by_name(const std::string& name) {
    if (name.empty()) return 0;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);

    std::uint32_t pid = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, name.c_str()) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }

    CloseHandle(snap);
    return pid;
}

ProcessHandle open_for_injection(std::uint32_t pid) {
    // Try to enable SeDebugPrivilege — this bypasses integrity-level
    // checks that can cause WriteProcessMemory to fail with err=5
    // (ERROR_ACCESS_DENIED) when the target process runs at a higher
    // integrity level than the injector. Common when the user launched
    // MBAA.exe as Administrator but caster.exe is running unelevated.
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(),
                         TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        if (LookupPrivilegeValueA(nullptr, SE_DEBUG_NAME,
                                  &tp.Privileges[0].Luid)) {
            AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp),
                                  nullptr, nullptr);
        }
        CloseHandle(token);
    }

    DWORD access = PROCESS_CREATE_THREAD
                 | PROCESS_QUERY_INFORMATION
                 | PROCESS_VM_OPERATION
                 | PROCESS_VM_WRITE
                 | PROCESS_VM_READ;
    HANDLE h = OpenProcess(access, FALSE, pid);
    if (!h) return kInvalidHandle;
    return reinterpret_cast<ProcessHandle>(h);
}

// ---- Suspend / Resume via NtSuspendProcess / NtResumeProcess ----
//
// These are undocumented ntdll functions, but they're the standard way to
// suspend/resume an entire process (used by Process Explorer, Visual Studio,
// etc.). They're simpler and more reliable than enumerating threads and
// calling SuspendThread on each (which has race conditions — new threads
// can be created between enumeration and suspension).
//
// Function signatures:
//   NTSTATUS NtSuspendProcess(HANDLE ProcessHandle);
//   NTSTATUS NtResumeProcess(HANDLE ProcessHandle);
// Returns 0 (STATUS_SUCCESS) on success.

using NtSuspendResumeFn = long(__stdcall*)(void*);

static NtSuspendResumeFn get_ntdll_func(const char* name) {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) return nullptr;
    return reinterpret_cast<NtSuspendResumeFn>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, name)));
}

bool suspend_process(ProcessHandle handle) {
    if (handle == kInvalidHandle) return false;
    auto fn = get_ntdll_func("NtSuspendProcess");
    if (!fn) return false;
    long status = fn(reinterpret_cast<void*>(handle));
    return status == 0;  // STATUS_SUCCESS
}

bool resume_process(ProcessHandle handle) {
    if (handle == kInvalidHandle) return false;
    auto fn = get_ntdll_func("NtResumeProcess");
    if (!fn) return false;
    long status = fn(reinterpret_cast<void*>(handle));
    return status == 0;  // STATUS_SUCCESS
}

} // namespace caster::common::win32::process
