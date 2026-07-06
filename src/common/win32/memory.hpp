// src/common/win32/memory.hpp
//
// Win32 memory wrappers for cross-process operations: VirtualAllocEx,
// WriteProcessMemory, ReadProcessMemory, VirtualProtectEx, patchMemory,
// and the classic LoadLibraryW-based DLL injector.
//
// All functions take a process::ProcessHandle (opaque uintptr_t) to avoid
// leaking windows.h into this header.

#pragma once

#include "process.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace caster::common::win32::memory {

// Allocate a buffer in the target process. Returns the remote address
// (0 on failure). Caller must free() it via free_remote().
std::uintptr_t alloc_remote(process::ProcessHandle proc,
                            std::size_t size);

// Free a buffer previously allocated via alloc_remote().
bool free_remote(process::ProcessHandle proc, std::uintptr_t remote_addr);

// Write `data` to `remote_addr` in the target process.
bool write_remote(process::ProcessHandle proc,
                  std::uintptr_t remote_addr,
                  const void* data, std::size_t size);

// Read `size` bytes from `remote_addr` in the target process into `out`.
bool read_remote(process::ProcessHandle proc,
                 std::uintptr_t remote_addr,
                 void* out, std::size_t size);

// Change the protection of a memory region in the target process.
// Returns the previous protection via `old_protect_out` (may be nullptr).
bool protect_remote(process::ProcessHandle proc,
                    std::uintptr_t addr, std::size_t size,
                    std::uint32_t new_protect,
                    std::uint32_t* old_protect_out);

// Flush the instruction cache for a patched region — required after
// writing code bytes so the CPU doesn't execute stale cache lines.
bool flush_instruction_cache(process::ProcessHandle proc,
                             std::uintptr_t addr, std::size_t size);

// Patch `bytes` at `addr` in the target process. Saves the old protection,
// switches to PAGE_EXECUTE_READWRITE, writes, restores protection, then
// flushes the instruction cache. This is the high-level helper used by
// the launcher for ASM patches.
bool patch_memory(process::ProcessHandle proc,
                  std::uintptr_t addr,
                  const std::vector<std::uint8_t>& bytes);

// Classic LoadLibraryW injection:
//   1. alloc_remote(buffer for wide DLL path)
//   2. write_remote(path bytes)
//   3. GetModuleHandleA("kernel32.dll") + GetProcAddress(LoadLibraryW)
//   4. CreateRemoteThread(LoadLibraryW, remote_path)
//   5. WaitForSingleObject(thread, timeout_ms)
//   6. GetExitCodeThread → 0 = failure, non-zero = HMODULE on success
//
// Returns the HMODULE (cast to uintptr_t) on success, 0 on failure.
// `error_message` is populated on failure.
std::uintptr_t inject_dll_w(process::ProcessHandle proc,
                            const std::wstring& dll_path_wide,
                            std::uint32_t timeout_ms,
                            std::string& error_message);

// Convenience overload taking a UTF-8 path. Internally converts to wide.
std::uintptr_t inject_dll_w(process::ProcessHandle proc,
                            const std::string& dll_path_utf8,
                            std::uint32_t timeout_ms,
                            std::string& error_message);

} // namespace caster::common::win32::memory
