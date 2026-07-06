// src/exe/cli.hpp
//
// CLI mode dispatcher. Handles --training / --versus / --host / --join /
// --spec when caster.exe is run without the GUI.
//
// All modes share the same pattern:
//   1. Load config
//   2. For offline (training/versus): just launch the game
//   3. For netplay (host/join/spec): start a session, drive step() in a
//      16ms loop, auto-confirm (host), launch game after handshake
//   4. Wait for the game to exit (loop is_alive())
//   5. Cleanup

#pragma once

#include "cli_args.hpp"
#include "../common/config.hpp"

namespace caster::exe::cli {

// Run the CLI mode specified in `args`. Returns the exit code.
// `cfg` is the loaded config (with CLI overrides already applied).
int run(const Args& args, const caster::common::config::Config& cfg);

} // namespace caster::exe::cli
