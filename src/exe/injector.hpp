// src/exe/injector.hpp
//
// High-level DLL injection helpers used by the GUI when the user wants
// to attach to an already-running process (debugging workflow). The main
// launcher flow (Phase 5) uses WindowsLauncher which creates the process
// suspended + injects + resumes — but this attach path is useful for
// attaching hook.dll to a game that was started independently.
//
// Implementation is now factored through win32::process + win32::memory
// wrappers (Phase 4 refactor). The public API stays the same.

#pragma once

#include <cstdint>
#include <string>

namespace caster::exe {

struct InjectionResult {
    bool        success  = false;
    uint32_t    pid      = 0;
    std::string message;
};

// Find the PID of the first process whose exe name (case-insensitive)
// matches `name`. Returns 0 if not found or on error.
uint32_t find_process_by_name(const std::string& name);

// Inject the DLL at `dll_path` (must be absolute) into process `pid`.
// Blocks until the remote LoadLibraryW call returns (or 5 s timeout).
InjectionResult inject_dll(uint32_t pid, const std::string& dll_path);

} // namespace caster::exe
