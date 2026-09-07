// src/common/win32/paths.hpp
//
// Single place that answers "where is the current process image?".
//
// Before this helper, every launcher call site reimplemented the same
// logic with SDL_GetBasePath() + a silent current_path() fallback, each
// with slightly different logging (warn here, silent there). Worse:
// SDL_GetBasePath() can return null (observed in the wild), and the
// silent CWD fallback then resolved a DIFFERENT install's files —
// e.g. launching another folder's MBAA.exe + hook.dll without a word.
//
// Rules going forward:
//   - Exe-side code resolves files via exe_dir()/exe_file().
//   - When exe_dir() fails, callers log at err level and say which
//     fallback they took. No silent cross-install resolution.

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace caster::common::win32::paths {

// Directory containing the current process image, resolved with
// GetModuleFileNameW (unicode-safe, unlike SDL_GetBasePath which
// additionally can return null). Returns nullopt only if the OS call
// itself fails, which is practically impossible.
std::optional<std::filesystem::path> exe_dir();

// exe_dir() / filename, or nullopt when exe_dir() is unavailable.
std::optional<std::filesystem::path> exe_file(const std::string& filename);

} // namespace caster::common::win32::paths
