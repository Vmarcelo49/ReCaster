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
#include <winsock2.h>
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

// Suppress Wine's noisy fixme messages (xinput, etc.) while keeping
// err/warn channels visible for diagnostics.
//
// Wine prints fixme messages to stderr for unimplemented features, which
// interleave with the caster's output and make --help and error messages
// hard to read. We only suppress the fixme channel — err and warn stay
// visible so real problems aren't hidden. The user can override by
// setting WINEDEBUG themselves before launching caster.exe.
void suppress_wine_debug_if_needed() {
    if (!running_under_wine()) return;
    if (GetEnvironmentVariableA("WINEDEBUG", nullptr, 0) > 0) return;
    // fixme-all suppresses only the fixme channel. err/warn/trace stay.
    putenv(const_cast<char*>("WINEDEBUG=fixme-all"));
    SetEnvironmentVariableA("WINEDEBUG", "fixme-all");
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

    // ---- 0a. Suppress DXVK HUD (Wine only) -----------------------------
    // When running under Wine with DXVK (the D3D9→Vulkan translation layer,
    // installed by default in most modern Wine prefixes), DXVK renders a
    // HUD overlay on top of the game. One of its elements is "Compiling
    // shaders..." which appears at the bottom of the screen whenever DXVK
    // is compiling D3D9 shaders into Vulkan pipelines — typically during
    // game loading and the first time each shader is seen.
    //
    // This is harmless but visually intrusive, especially because it
    // overlaps with our own overlay. To disable it, set DXVK_HUD=0 in the
    // environment before launching MBAA.exe. Uncomment the lines below to
    // enable this behavior globally (the env var is inherited by the
    // child process via CreateProcessW).
    //
    // if (running_under_wine()) {
    //     SetEnvironmentVariableA("DXVK_HUD", "0");
    // }
    //
    // Alternative: keep specific HUD elements (e.g. FPS only):
    //   SetEnvironmentVariableA("DXVK_HUD", "fps");
    // Valid elements: fps, frametimes, submissions, drawcalls, pipelines,
    //                 descriptors, memory, gpuload, version, api, compiler
    // "compiler" is the one that shows "Compiling shaders...".

    // ---- 0b. Initialize Winsock BEFORE any network operation ----------
    // ENet calls WSAStartup(1.1) internally, but only when enet_initialize
    // runs (inside transport_.listen/bind_only). Some network operations
    // (e.g. ip_discovery::get_local_ip in lookup_host_addresses) run BEFORE
    // ENet is initialized and would fail without Winsock. We request 2.2
    // (needed for getaddrinfo) — ENet's 1.1 request is satisfied within
    // the 2.2 negotiation. Matches zzcaster's initWinsock() pattern.
    {
        WSADATA wsa_data;
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            std::fprintf(stderr, "caster: WSAStartup failed\n");
            return 1;
        }
    }

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
    WSACleanup();
    return rc;
}
