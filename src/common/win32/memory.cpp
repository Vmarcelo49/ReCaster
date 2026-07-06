// src/common/win32/memory.cpp

#include "memory.hpp"
#include "../logger.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>

namespace caster::common::win32::memory {

namespace {

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

std::uintptr_t alloc_remote(process::ProcessHandle proc, std::size_t size) {
    HANDLE h = reinterpret_cast<HANDLE>(proc);
    LPVOID p = VirtualAllocEx(h, nullptr, size,
                              MEM_COMMIT | MEM_RESERVE,
                              PAGE_READWRITE);
    return reinterpret_cast<std::uintptr_t>(p);
}

bool free_remote(process::ProcessHandle proc, std::uintptr_t remote_addr) {
    HANDLE h = reinterpret_cast<HANDLE>(proc);
    return VirtualFreeEx(h, reinterpret_cast<LPVOID>(remote_addr),
                         0, MEM_RELEASE) != 0;
}

bool write_remote(process::ProcessHandle proc,
                  std::uintptr_t remote_addr,
                  const void* data, std::size_t size) {
    HANDLE h = reinterpret_cast<HANDLE>(proc);
    SIZE_T written = 0;
    return WriteProcessMemory(h, reinterpret_cast<LPVOID>(remote_addr),
                              data, size, &written) != 0
           && written == size;
}

bool read_remote(process::ProcessHandle proc,
                 std::uintptr_t remote_addr,
                 void* out, std::size_t size) {
    HANDLE h = reinterpret_cast<HANDLE>(proc);
    SIZE_T got = 0;
    return ReadProcessMemory(h, reinterpret_cast<LPCVOID>(remote_addr),
                             out, size, &got) != 0
           && got == size;
}

bool protect_remote(process::ProcessHandle proc,
                    std::uintptr_t addr, std::size_t size,
                    std::uint32_t new_protect,
                    std::uint32_t* old_protect_out) {
    HANDLE h = reinterpret_cast<HANDLE>(proc);
    DWORD old_prot = 0;
    bool ok = VirtualProtectEx(h, reinterpret_cast<LPVOID>(addr),
                               size, new_protect, &old_prot) != 0;
    if (old_protect_out) *old_protect_out = old_prot;
    return ok;
}

bool flush_instruction_cache(process::ProcessHandle proc,
                             std::uintptr_t addr, std::size_t size) {
    HANDLE h = reinterpret_cast<HANDLE>(proc);
    return FlushInstructionCache(h, reinterpret_cast<LPCVOID>(addr),
                                 size) != 0;
}

bool patch_memory(process::ProcessHandle proc,
                  std::uintptr_t addr,
                  const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) return false;

    std::uint32_t old_prot = 0;
    if (!protect_remote(proc, addr, bytes.size(),
                        PAGE_EXECUTE_READWRITE, &old_prot)) {
        logger::err("patch_memory: VirtualProtectEx failed at 0x{:x}", addr);
        return false;
    }
    if (!write_remote(proc, addr, bytes.data(), bytes.size())) {
        logger::err("patch_memory: WriteProcessMemory failed at 0x{:x}", addr);
        // Try to restore protection anyway.
        protect_remote(proc, addr, bytes.size(), old_prot, nullptr);
        return false;
    }
    if (!flush_instruction_cache(proc, addr, bytes.size())) {
        logger::warn("patch_memory: FlushInstructionCache failed (non-fatal)");
    }
    // Restore the original protection.
    if (!protect_remote(proc, addr, bytes.size(), old_prot, nullptr)) {
        logger::warn("patch_memory: failed to restore protection at 0x{:x}",
                     addr);
    }
    return true;
}

std::uintptr_t inject_dll_w(process::ProcessHandle proc,
                            const std::wstring& dll_path_wide,
                            std::uint32_t timeout_ms,
                            std::string& error_message) {
    HANDLE h = reinterpret_cast<HANDLE>(proc);
    if (dll_path_wide.empty()) {
        error_message = "inject_dll_w: empty path";
        return 0;
    }

    // 1. Allocate a buffer in the target big enough for the wide path
    //    (including null terminator).
    SIZE_T path_bytes = (dll_path_wide.size() + 1) * sizeof(wchar_t);
    LPVOID remote_buf = VirtualAllocEx(h, nullptr, path_bytes,
                                       MEM_COMMIT | MEM_RESERVE,
                                       PAGE_READWRITE);
    if (!remote_buf) {
        DWORD err = GetLastError();
        error_message = "VirtualAllocEx failed (err=" + std::to_string(err) +
                        ")";
        return 0;
    }

    // 2. Write the path (with null terminator).
    std::vector<wchar_t> path_buf(dll_path_wide.begin(), dll_path_wide.end());
    path_buf.push_back(L'\0');
    SIZE_T written = 0;
    if (!WriteProcessMemory(h, remote_buf, path_buf.data(),
                            path_bytes, &written) || written != path_bytes) {
        DWORD err = GetLastError();
        error_message = "WriteProcessMemory failed (err=" +
                        std::to_string(err) + ")";
        VirtualFreeEx(h, remote_buf, 0, MEM_RELEASE);
        return 0;
    }

    // 3. Resolve LoadLibraryW in kernel32.dll. Because kernel32 loads at the
    //    same base in (almost) every Win32 process, our local address is
    //    valid in the target too.
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (!k32) {
        error_message = "GetModuleHandleA(kernel32) failed";
        VirtualFreeEx(h, remote_buf, 0, MEM_RELEASE);
        return 0;
    }
    FARPROC load_lib = GetProcAddress(k32, "LoadLibraryW");
    if (!load_lib) {
        error_message = "GetProcAddress(LoadLibraryW) failed";
        VirtualFreeEx(h, remote_buf, 0, MEM_RELEASE);
        return 0;
    }

    // 4. CreateRemoteThread that calls LoadLibraryW(remote_buf).
    HANDLE thread = CreateRemoteThread(
        h, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(load_lib),
        remote_buf, 0, nullptr);
    if (!thread) {
        DWORD err = GetLastError();
        error_message = "CreateRemoteThread failed (err=" +
                        std::to_string(err) + ")";
        VirtualFreeEx(h, remote_buf, 0, MEM_RELEASE);
        return 0;
    }

    // 5. Wait for LoadLibraryW to return (or timeout).
    DWORD wait = WaitForSingleObject(thread, timeout_ms);
    if (wait == WAIT_TIMEOUT) {
        error_message = "Remote LoadLibraryW timed out (" +
                        std::to_string(timeout_ms) + " ms)";
        CloseHandle(thread);
        VirtualFreeEx(h, remote_buf, 0, MEM_RELEASE);
        return 0;
    }
    if (wait != WAIT_OBJECT_0) {
        DWORD err = GetLastError();
        error_message = "WaitForSingleObject failed (err=" +
                        std::to_string(err) + ")";
        CloseHandle(thread);
        VirtualFreeEx(h, remote_buf, 0, MEM_RELEASE);
        return 0;
    }

    // 6. Read the exit code — LoadLibraryW returns the HMODULE on success
    //    or NULL on failure.
    DWORD exit_code = 0;
    if (!GetExitCodeThread(thread, &exit_code)) {
        DWORD err = GetLastError();
        error_message = "GetExitCodeThread failed (err=" +
                        std::to_string(err) + ")";
        CloseHandle(thread);
        VirtualFreeEx(h, remote_buf, 0, MEM_RELEASE);
        return 0;
    }

    CloseHandle(thread);
    VirtualFreeEx(h, remote_buf, 0, MEM_RELEASE);

    if (exit_code == 0) {
        error_message = "Remote LoadLibraryW returned NULL — DLL failed to "
                        "load (check architecture: 32-bit DLL into 32-bit "
                        "process? missing dependencies?)";
        return 0;
    }

    return static_cast<std::uintptr_t>(exit_code);
}

std::uintptr_t inject_dll_w(process::ProcessHandle proc,
                            const std::string& dll_path_utf8,
                            std::uint32_t timeout_ms,
                            std::string& error_message) {
    return inject_dll_w(proc, utf8_to_wide(dll_path_utf8),
                        timeout_ms, error_message);
}

} // namespace caster::common::win32::memory
