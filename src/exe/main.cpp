// src/exe/main.cpp
//
// caster.exe entry point. Sets up logger + config + CLI parser,
// then dispatches to either GUI mode (Menu) or CLI mode (Training/Versus/
// Host/Join/Spectate).
//
// The Menu mode opens the themed SDL2+ImGui window.

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
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace fs = std::filesystem;

namespace {

// Shorter aliases so function signatures stay readable.
namespace cmn = caster::common;
namespace cli = caster::exe::cli;
namespace pages = caster::exe::pages;

// Detect if we're running under Wine by looking for wine_get_version in
// ntdll.dll. This is the same check used in connection_type.cpp and
// lifecycle.cpp.
bool running_under_wine() {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) return false;
    return GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

// Suppress Wine's verbose debug output (radv warnings, xinput fixme,
// etc.) by setting WINEDEBUG=-all if not already set by the user.
//
// Wine prints these warnings to stderr, which interleaves with the
// caster's own stdout/stderr output and makes --help and error
// messages hard to read. The user can override this by setting
// WINEDEBUG themselves before launching caster.exe — we only set it
// if it's not already in the environment.
void suppress_wine_debug_if_needed() {
    if (!running_under_wine()) return;
    // Only set WINEDEBUG if the user hasn't already configured it.
    // This preserves the user's ability to enable verbose Wine debug
    // output for troubleshooting.
    if (GetEnvironmentVariableA("WINEDEBUG", nullptr, 0) > 0) return;
    // WINEDEBUG=-all suppresses all Wine debug messages. This must be
    // set BEFORE any Wine code runs — putenv makes it visible to the
    // Wine runtime which checks the environment on startup. We also
    // call SetEnvironmentVariableA for good measure (covers both the
    // C runtime env and the Win32 env).
    putenv(const_cast<char*>("WINEDEBUG=-all"));
    SetEnvironmentVariableA("WINEDEBUG", "-all");
}

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
// fully implemented — offline modes launch the game, netplay modes
// drive a NetplaySession to completion then launch.
int run_cli_mode(const cli::Args& args,
                 const cmn::config::Config& cfg) {
    return cli::run(args, cfg);
}

// GUI mode (default). themed launcher with state machine UI.
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

    // ---- 0. Suppress Wine debug output BEFORE anything else ----------
    // Wine prints verbose warnings (radv, xinput fixme, etc.) to stderr
    // which interleave with --help and error messages. Setting
    // WINEDEBUG=-all suppresses them. This is a no-op on native Windows.
    // Must happen before any SDL/Wine code runs. The user can override
    // by setting WINEDEBUG themselves (we only set it if unset).
    suppress_wine_debug_if_needed();

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
