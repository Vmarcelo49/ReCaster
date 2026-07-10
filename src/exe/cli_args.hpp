// src/exe/cli_args.hpp
//
// CLI argument parser for caster.exe. Mirrors zzcaster's `main.zig` flag
// set exactly so behaviour is identical between the Zig and C++ versions.
//
// Flags (all optional, all may be combined):
//   -h, --help, -help, /?      Show usage and exit 0
//   --training                 Launch in Training mode (offline, no netplay)
//   --versus                   Launch in Versus mode (offline, no netplay)
//   --host                     Host a netplay session (interactive if no peer)
//   --join=PEER                Join a peer. PEER may be:
//                                - "host:port"   → direct ENet connect
//                                - "#ABCD"       → relay room code
//   --spec=PEER                Spectate a peer. Same PEER format as --join,
//                              but relay spectator not yet implemented (direct
//                              spectate via host:port works).
//   --port=N                   Override default UDP port (default: 46318)
//   --delay=N                  Override auto input delay (frames, 0..8)
//   --rollback=N               Override rollback window (frames, default: 4)
//   --name=NAME                Override display name (max 31 chars)
//
// Mode selection rules (same as zzcaster):
//   - If --help is present, show usage and exit. No mode is run.
//   - If --training is present → mode = Training (CLI).
//   - If --versus   is present → mode = Versus (CLI).
//   - If --host     is present → mode = Host (CLI).
//   - If --join     is present → mode = Join (CLI).
//   - If --spec     is present → mode = Spectate (CLI).
//   - If multiple mode flags are present, the LAST one on the command line
//     wins (mirrors zzcaster's override semantics).
//   - If no mode flag is present → mode = Menu (interactive GUI).
//
// CLI modes (Training/Versus/Host/Join/Spectate). All modes are fully
// implemented — see src/exe/cli.cpp for the dispatcher. The Menu mode
// (no flags) opens the interactive GUI.

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace caster::exe::cli {

enum class Mode {
    Menu,        // interactive GUI (default)
    Training,    // --training (CLI)
    Versus,      // --versus   (CLI)
    Host,        // --host     (CLI)
    Join,        // --join=PEER (CLI)
    Spectate,    // --spec=PEER (CLI)
};

struct Args {
    Mode        mode             = Mode::Menu;

    // Netplay overrides (only meaningful in Host/Join/Spectate).
    int         port             = -1;       // -1 = use default (46318)
    int         delay            = -1;       // -1 = auto (computed from RTT)
    int         rollback         = -1;       // -1 = use config default
    std::string name;                        // empty = use config display_name
    std::string peer;                        // --join / --spec value (raw)

    bool        help_requested   = false;
    std::string help_message;                // filled if help_requested
};

// Parse argv. Returns Args on success. On parse error, throws
// std::runtime_error with a user-facing message.
Args parse(int argc, char** argv);

// Render the --help text. Used by main() when --help is given.
std::string helpText();

} // namespace caster::exe::cli
