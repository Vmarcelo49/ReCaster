// src/exe/main.cpp
//
// caster.exe entry point. Phase 3: themed launcher with state machine UI.
// The main menu has header + sidebar + 3 placeholder pages (Play, Config,
// Controllers). Real page content lands in later phases (5/6/7/8).
//
// CLI modes (Training/Versus/Host/Join/Spectate) are still stubs that log
// a TODO message and exit. The Menu mode opens the themed window.

#include "cli_args.hpp"
#include "cli.hpp"
#include "injector.hpp"
#include "pages/main_menu.hpp"
#include "ui_state.hpp"
#include "../common/config.hpp"
#include "../common/gui_window.hpp"
#include "../common/logger.hpp"

#include <imgui.h>
#include <SDL2/SDL.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

// Shorter aliases so function signatures stay readable.
namespace cmn = caster::common;
namespace cli = caster::exe::cli;
namespace pages = caster::exe::pages;

// Resolve config.ini path: <dir of caster.exe>/caster/config.ini
fs::path resolve_config_path() {
    const char* base = SDL_GetBasePath();
    if (!base) {
        return fs::current_path() / "caster" / "config.ini";
    }
    return fs::path(base) / "caster" / "config.ini";
}

// Apply CLI overrides (--name, --rollback) onto a loaded Config.
void apply_cli_overrides(cmn::config::Config& cfg,
                         const cli::Args& args) {
    if (!args.name.empty()) {
        cfg.display_name = args.name;
    }
    if (args.rollback >= 0) {
        cfg.default_rollback = args.rollback;
    }
}

// Dispatch a CLI mode (Training/Versus/Host/Join/Spectate).
// Phase 9: fully implemented — offline modes launch the game, netplay modes
// drive a NetplaySession to completion then launch.
int run_cli_mode(const cli::Args& args,
                 const cmn::config::Config& cfg) {
    return cli::run(args, cfg);
}

// GUI mode (default). Phase 3: themed launcher with state machine UI.
// The MainMenu owns UiState + MenuPage and renders header/sidebar/content.
int run_gui_mode(cmn::config::Config& cfg) {
    using namespace caster::common;

    GuiWindow win("caster", 1024, 768);
    if (!win.is_open()) {
        // Construction already showed an error dialog via SDL_ShowSimpleMessageBox.
        logger::err("GuiWindow construction failed; aborting GUI mode");
        maybe_shutdown_sdl();
        return 1;
    }

    pages::MainMenu menu;
    menu.init_controller_state();

    while (win.pump_frame([&] {
        // Poll SDL joystick events for hot-plug detection. The GuiWindow's
        // pump_frame already called SDL_PollEvent for us and forwarded
        // events to ImGui, but joystick add/remove events need explicit
        // handling. We re-poll here for any events ImGui didn't consume.
        // (SDL_PollEvent is safe to call multiple times per frame; the
        // second call just returns 0 if the queue is empty.)
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_JOYDEVICEADDED:
                    logger::info("Joystick added: index={}", ev.jdevice.which);
                    break;
                case SDL_JOYDEVICEREMOVED:
                    logger::info("Joystick removed: instance={}",
                                 ev.jdevice.which);
                    break;
                default:
                    break;
            }
        }

        // draw() returns false when the user clicked Quit (sidebar Q button)
        // or the OS asked us to close. We use that to break out of the loop.
        if (!menu.draw(cfg)) {
            // Force-close the SDL window on the next pump_frame.
            win.request_close();
        }
    })) {
        // Continue pumping.
    }

    menu.shutdown_controller_state();
    maybe_shutdown_sdl();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    using namespace caster::common;

    // ---- 1. Parse CLI args first (we need --help before any init) ------
    cli::Args args;
    try {
        args = cli::parse(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "caster: %s\n\n", e.what());
        std::fprintf(stderr, "%s", cli::helpText().c_str());
        return 2;
    }

    if (args.help_requested) {
        std::fputs(args.help_message.c_str(), stdout);
        return 0;
    }

    // ---- 2. Load config from <exe_dir>/caster/config.ini ---------------
    const fs::path config_path = resolve_config_path();
    config::Config cfg = config::load(config_path.string());
    apply_cli_overrides(cfg, args);

    // ---- 3. Initialize logger (path depends on config) -----------------
    logger::init({}, cfg.log_to_stdout);
    logger::info("caster v{} starting (mode={})",
                 config::kVersionString,
                 static_cast<int>(args.mode));
    logger::info("config path: {}", config_path.string());
    logger::info("target process: {}", config::kTargetProcess);
    logger::info("default netplay port: {}", config::kDefaultPort);

    // ---- 4. Dispatch ---------------------------------------------------
    int rc = 0;
    try {
        if (args.mode == cli::Mode::Menu) {
            rc = run_gui_mode(cfg);
        } else {
            rc = run_cli_mode(args, cfg);
        }
    } catch (const std::exception& e) {
        logger::err("fatal: {}", e.what());
        rc = 1;
    }

    logger::info("caster exiting (rc={})", rc);
    logger::shutdown();
    return rc;
}
