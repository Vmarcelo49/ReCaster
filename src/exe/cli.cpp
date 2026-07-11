// src/exe/cli.cpp

#include "cli.hpp"
#include "cli_args.hpp"
#include "launcher/game_runner.hpp"
#include "session/session.hpp"
#include "../common/config.hpp"
#include "../common/logger.hpp"
#include "../common/net/relay/relay_config.hpp"

#include <chrono>
#include <thread>

namespace caster::exe::cli {

namespace {

namespace ss = session;
namespace lg = caster::common::logger;
namespace cfg_ns = caster::common::config;

// Build a relay_source string from cfg.relay_servers (newline-joined).
std::string relay_source_from_cfg(const cfg_ns::Config& cfg) {
    std::string s;
    for (const auto& r : cfg.relay_servers) {
        if (!s.empty()) s += '\n';
        s += r;
    }
    return s;
}

// Launch the game offline (training or versus). Blocks until the game exits.
int run_offline(const cfg_ns::Config& cfg, bool training) {
    launcher::GameRunner runner;
    launcher::LaunchOfflineParams params;
    params.training = training;

    // Launch is async — the worker does CreateProcess + inject + IPC.
    runner.launch_offline_async(cfg, params);

    // Wait for the launch to complete by polling the snapshot.
    std::uint32_t pid = 0;
    while (true) {
        auto gs = runner.snapshot();
        if (gs.is_running) {
            pid = gs.pid;
            break;
        }
        if (!gs.launch_in_progress && !gs.last_error.empty()) {
            lg::err("CLI: launch failed: {}", gs.last_error);
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    lg::info("CLI: game launched (PID {}), waiting for exit...", pid);

    // Block until the game exits (poll snapshot, worker detects exit).
    while (runner.snapshot().is_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    lg::info("CLI: game exited");
    return 0;
}

// Drive a netplay session to completion (or failure/cancel). Auto-confirms
// on the host side when the handshake reaches WaitingConfirmation.
// On Launching: snapshots config, deinits session, launches game.
// Returns the LaunchResult from the game launch (or a failed result).
launcher::LaunchResult drive_session_and_launch(
    const cfg_ns::Config& cfg,
    ss::NetplaySession& session,
    launcher::GameRunner& runner) {

    using namespace caster::common;

    while (true) {
        auto snap = session.snapshot();

        switch (snap.state) {
            case ss::SessionState::Failed:
                return {.error_message = snap.error_message};

            case ss::SessionState::Cancelled:
                return {.error_message = "Cancelled by user"};

            case ss::SessionState::WaitingConfirmation:
                // Host: auto-confirm (CLI mode has no UI button).
                session.host_confirm_async();
                break;

            case ss::SessionState::Launching: {
                // Handshake complete — snapshot config, deinit, launch.
                auto np_cfg = snap.config;
                // Client: sleep 500ms so the host gets our confirm.
                if (!np_cfg.is_host) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                session.deinit_async();
                // Wait for the worker to process Deinit (state returns to Idle).
                while (session.snapshot().state != ss::SessionState::Idle) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                // Launch the game (async). The worker does the 1s UDP
                // release sleep + CreateProcess + inject + IPC handshake.
                runner.launch_after_handshake_async(cfg, np_cfg);

                // Wait for the launch to complete by polling the snapshot.
                while (true) {
                    auto gs = runner.snapshot();
                    if (gs.is_running) {
                        return {.success = true, .pid = gs.pid};
                    }
                    if (!gs.launch_in_progress && !gs.last_error.empty()) {
                        return {.error_message = gs.last_error};
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }
            }

            default:
                // Still in progress — sleep ~60fps.
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                break;
        }
    }
}

// Run a netplay CLI mode (host/join/spec). Starts the session, drives it,
// launches the game, waits for exit.
int run_netplay(const cfg_ns::Config& cfg, const Args& args) {
    ss::NetplaySession session;
    session.set_local_name_async(cfg.display_name);
    session.detect_connection_type_async();
    session.lookup_host_addresses_async();

    const std::string relay_source = relay_source_from_cfg(cfg);
    const int port = (args.port > 0)
        ? args.port
        : static_cast<int>(cfg_ns::kDefaultPort);
    const bool manual_delay = (args.delay >= 0);

    // Apply manual delay override BEFORE start_host/start_join so the
    // value is in place when the handshake exchanges the NetplayConfig
    // with the peer.
    if (manual_delay) {
        session.set_manual_delay_async(static_cast<std::uint8_t>(args.delay));
        lg::info("CLI: manual delay override = {} frames", args.delay);
    }

    // Apply rollback override BEFORE start_host/start_join so the value
    // is in place when the handshake exchanges the NetplayConfig with
    // the peer.
    if (args.rollback >= 0) {
        session.set_rollback_async(static_cast<std::uint8_t>(args.rollback));
        lg::info("CLI: rollback override = {} frames", args.rollback);
    }

    // Give the worker a moment to process the set_local_name/detect_connection
    // commands before we enqueue the start command. The worker processes
    // commands in order, so this is just a safety margin.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    switch (args.mode) {
        case Mode::Host:
            lg::info("CLI: Host on port {}{}", port,
                     manual_delay ? " (manual delay=" +
                     std::to_string(args.delay) + ")" : "");
            session.start_smart_host_async(relay_source,
                                            static_cast<std::uint16_t>(port), false);
            break;

        case Mode::Join:
            if (args.peer.starts_with('#')) {
                // Relay join.
                std::string code = args.peer.substr(1);
                lg::info("CLI: Join via relay room #{}", code);
                session.start_relay_join_async(relay_source, code, false);
            } else {
                // Direct join — parse host:port.
                auto colon = args.peer.rfind(':');
                if (colon == std::string::npos) {
                    lg::err("CLI: --join requires host:port or #room");
                    return 1;
                }
                std::string host = args.peer.substr(0, colon);
                std::string port_str = args.peer.substr(colon + 1);
                try {
                    int peer_port = std::stoi(port_str);
                    lg::info("CLI: Join direct {}:{}", host, peer_port);
                    session.start_join_async(host,
                        static_cast<std::uint16_t>(peer_port), false);
                } catch (...) {
                    lg::err("CLI: invalid port in '{}'", args.peer);
                    return 1;
                }
            }
            break;

        case Mode::Spectate:
            // Direct spectate only.
            if (args.peer.starts_with('#')) {
                lg::err("CLI: spectate via relay not supported yet");
                return 1;
            }
            {
                auto colon = args.peer.rfind(':');
                if (colon == std::string::npos) {
                    lg::err("CLI: --spec requires host:port");
                    return 1;
                }
                std::string host = args.peer.substr(0, colon);
                std::string port_str = args.peer.substr(colon + 1);
                try {
                    int peer_port = std::stoi(port_str);
                    lg::info("CLI: Spectate direct {}:{}", host, peer_port);
                    // Spectate uses the same join path but with a flag.
                    // For now, just join — the spectator flag is set in
                    // the IPC config later.
                    session.start_join_async(host,
                        static_cast<std::uint16_t>(peer_port), false);
                } catch (...) {
                    lg::err("CLI: invalid port in '{}'", args.peer);
                    return 1;
                }
            }
            break;

        default:
            return 0;
    }

    // Wait briefly for the start command to land and check for failure.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto snap = session.snapshot();
    if (snap.state == ss::SessionState::Failed) {
        lg::err("CLI: failed to start session: {}", snap.error_message);
        session.deinit_async();
        return 1;
    }

    // Drive the session to completion + launch the game.
    launcher::GameRunner runner;
    auto launch_r = drive_session_and_launch(cfg, session, runner);
    if (!launch_r.success) {
        lg::err("CLI: {}", launch_r.error_message);
        session.deinit_async();
        return 1;
    }

    lg::info("CLI: game launched (PID {}), waiting for exit...", launch_r.pid);

    // Block until the game exits (poll snapshot, worker detects exit).
    while (runner.snapshot().is_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    lg::info("CLI: game exited");
    return 0;
}

} // namespace

int run(const Args& args, const cfg_ns::Config& cfg) {
    switch (args.mode) {
        case Mode::Training:
            return run_offline(cfg, /*training=*/true);
        case Mode::Versus:
            return run_offline(cfg, /*training=*/false);
        case Mode::Host:
        case Mode::Join:
        case Mode::Spectate:
            return run_netplay(cfg, args);
        case Mode::Menu:
            return 0;
    }
    return 0;
}

} // namespace caster::exe::cli
