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

// Default UDP port for netplay. Matches zzcaster's `default_port = 46318`.
inline constexpr unsigned short kDefaultPort = 46318;

// Version string exchanged during handshake. Must match between peers.

inline constexpr const char* kVersionString = "0";

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
    int  max_real_delay     = 254;
    bool high_cpu_priority  = true;

    // [game]
    bool stage_animations_off = false;
    bool auto_replay_save     = true;

    // [system]
    bool auto_check_updates = true;
    bool log_to_stdout      = false;

    // [overlay]
    bool playername_enabled        = true;   // show player names during netplay
    bool playername_position_bottom = false;  // false=top, true=bottom

    // [ui]
    // Active theme id: 0 = Default, 1 = Modern, 2 = Elegant.
    // Stored as int for INI simplicity. See ui_theme::ThemeId.
    int theme = 2;  // Elegant by default — matches the HTML reference

    // [ui]
    // Global rounded-corners toggle. When true, widgets use the theme's
    // native btn_radius/input_radius. When false, all corners are forced
    // to 0 (sharp) regardless of theme. Lets the user opt into rounded
    // corners on Default/Elegant or opt out on Modern.
    bool rounded_corners = false;  // off by default — sharp looks cleaner

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
