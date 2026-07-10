// src/common/config.hpp
//
// Launcher configuration. Mirrors the fields of zzcaster's `common/config.zig`.
// Stored in `caster/config.ini` next to the .exe (NOT in %LOCALAPPDATA% —
// per author's choice so the .dll ships next to the .exe and shares the dir).
//
// Default values match zzcaster's defaults exactly so behaviour is identical.

#pragma once

#include <string>
#include <vector>

namespace caster::common::config {

// Bumped whenever the IPC wire format changes. Both launcher and DLL must
// agree on this number. zzcaster is at v3 (with match_seed + local_udp_port).
inline constexpr int kIpcVersion = 3;

// Default UDP port for netplay. Matches zzcaster's `default_port = 46318`.
inline constexpr unsigned short kDefaultPort = 46318;

// Version string exchanged during handshake. Must match between peers.
// zzcaster uses "4.1-zig"; we use "4.1-cpp" to detect cross-language peers
// (they'd fail version exchange, which is the right behaviour — protocols
// are not byte-compatible even though they share the layout).
inline constexpr const char* kVersionString = "4.1-cpp";

// Max name length (excluding null terminator). Matches zzcaster's
// `max_name_len = 31`.
inline constexpr int kMaxNameLen = 31;

// Hardcoded target process name. ASM patches are currently a no-op
// (see docs/stubs.md); this just controls what process
// the injector looks for when falling back to attach-to-running mode.
inline constexpr const char* kTargetProcess = "MBAA.exe";

// All fields are public — this is a plain data struct. Use load() to
// populate from disk, save() to write back.
struct Config {
    // [paths]
    std::string game_dir;               // empty = auto-detect (<exe_dir>/game/ or <exe_dir>/)

    // [player]
    std::string display_name;            // empty → fallback (not yet implemented)

    // [match]
    int  versus_win_count   = 2;
    int  default_rollback   = 4;
    int  max_real_delay     = 254;
    bool high_cpu_priority  = true;

    // [game]
    bool stage_animations_off = false;
    bool auto_replay_save     = true;

    // [system]
    bool auto_check_updates = true;
    bool log_to_stdout      = false;

    // [network]
    // Multi-line: one relay URL per line. Empty → use DEFAULT_RELAY_LIST.
    std::vector<std::string> relay_servers;

    // Path of the config.ini we were loaded from. Used by save() to write
    // back to the same file. Empty if defaults were used and nothing was
    // saved yet.
    std::string source_path;
};

// Load config from `path`. Missing file → returns defaults with source_path
// set (so a subsequent save() will create it). Malformed file → falls back
// to defaults for missing/invalid fields, logs a warning.
Config load(const std::string& path);

// Save config back to `cfg.source_path` (or `path` if non-empty).
// Atomic write: writes to .tmp then renames.
bool save(Config& cfg, const std::string& path = "");

// Default relay list, mirroring zzcaster's `net/relay_config.zig`.
// Returned as a vector so callers can append/override before saving.
std::vector<std::string> defaultRelayList();

// Convenience: format the relay list as a single multi-line string for
// display in the UI.
std::string relayListAsString(const std::vector<std::string>& relays);

} // namespace caster::common::config
