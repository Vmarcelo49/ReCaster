// src/exe/pages/main_menu.cpp

#include "main_menu.hpp"
#include "header.hpp"
#include "play_page.hpp"
#include "config_page.hpp"
#include "controllers_page.hpp"
#include "waiting_for_peer.hpp"

#include "../../common/config.hpp"
#include "../../common/logger.hpp"
#include "../../common/ui_theme.hpp"

#include <SDL2/SDL.h>
#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;

namespace caster::exe::pages {

namespace ut = caster::common::ui_theme;

// Window dimensions are in ui_theme.hpp (WINDOW_W / WINDOW_H).

MainMenu::MainMenu() = default;
MainMenu::~MainMenu() = default;

void MainMenu::init_controller_state() {
    const char* base = SDL_GetBasePath();
    if (base) {
        controllers_state_.mapping_path =
            (fs::path(base) / "caster" / "mapping.ini").string();
    } else {
        controllers_state_.mapping_path =
            (fs::current_path() / "caster" / "mapping.ini").string();
    }
    // Don't load here — controllers_page::draw() does lazy load on first frame.
}

void MainMenu::shutdown_controller_state() {
    controllers_page::close_joysticks(controllers_state_);
}

void MainMenu::transition_to(UiState new_state) {
    if (state_ == new_state) return;
    caster::common::logger::info("UI: {} -> {}", static_cast<int>(state_),
                                 static_cast<int>(new_state));
    state_ = new_state;
}

void MainMenu::set_error(const std::string& message) {
    error_message_ = message;
    transition_to(UiState::ErrorState);
}

void MainMenu::clear_error() {
    error_message_.clear();
    transition_to(UiState::Idle);
}

bool MainMenu::draw(caster::common::config::Config& cfg) {
    // Full-window root: fills the entire SDL window (resizable).
    const float win_w = ut::window_width();
    const float win_h = ut::window_height();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(win_w, win_h));
    ImGui::Begin("##caster_root", nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoNavFocus);

    // Vertical gradient background — covers the whole window area.
    caster::common::ui_theme::drawGradientBackground();

    switch (state_) {
        case UiState::Idle:
            drawIdle(cfg);
            break;
        case UiState::WaitingForPeer:
            drawWaitingForPeer(cfg);
            break;
        case UiState::TrainingWhileHosting:
            drawTrainingWhileHosting(cfg);
            break;
        case UiState::InGame:
            drawInGame();
            break;
        case UiState::ErrorState:
            drawErrorState();
            break;
    }

    ImGui::End();
    return !quit_requested_;
}

void MainMenu::drawIdle(caster::common::config::Config& cfg) {
    drawHeader();
    drawContent(cfg);
}

void MainMenu::drawHeader() {
    header::draw(page_);
}

void MainMenu::drawContent(caster::common::config::Config& cfg) {
    namespace ut = caster::common::ui_theme;

    // Content area: starts just below the header, full width, with the
    // theme-defined padding. Uses actual window dimensions so the layout
    // adapts when the user resizes the window.
    //
    // The Controllers page uses a smaller top padding (8px instead of 40px)
    // because it has a lot of vertical content (2 players × 13 bindings +
    // SOCD + deadzone) and benefits from starting higher.
    const float win_w = ut::window_width();
    const float win_h = ut::window_height();
    const float x = ut::CONTENT_PAD_X;
    const float pad_y = (page_ == MenuPage::Controllers) ? 8.0f : ut::CONTENT_PAD_Y;
    const float y = ut::HEADER_H + pad_y;
    const float w = win_w - 2 * ut::CONTENT_PAD_X;
    const float h = win_h - ut::HEADER_H - pad_y - ut::CONTENT_PAD_Y;

    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::BeginChild("##content", ImVec2(w, h),
                      ImGuiChildFlags_AlwaysUseWindowPadding);

    switch (page_) {
        case MenuPage::Play:
            play_page::draw(cfg, this, play_state_);
            break;
        case MenuPage::GameConfig:
            config_page::draw(cfg, config_state_);
            break;
        case MenuPage::Controllers:
            controllers_page::draw(controllers_state_);
            break;
    }

    ImGui::EndChild();
}

void MainMenu::drawWaitingForPeer(caster::common::config::Config& cfg) {
    if (!session_) {
        // Shouldn't happen — but be safe.
        transition_to(UiState::Idle);
        return;
    }

    auto r = waiting_for_peer::draw(*session_);

    if (r.launching) {
        // Handshake complete — launch the game with the session's config.
        // Snapshot the config BEFORE deinit (deinit frees the transport).
        auto snap = session_->snapshot();
        auto np_cfg = snap.config;
        // Client: sleep 500ms before deinit so the host receives our confirm.
        if (!np_cfg.is_host) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        session_->deinit_async();
        // Wait for the worker to process Deinit (state returns to Idle).
        while (session_->snapshot().state != session::SessionState::Idle) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        end_session();

        // Launch the game with the netplay config (async — worker does the
        // 1s UDP release sleep + CreateProcess + inject + IPC handshake).
        game_runner_.launch_after_handshake_async(cfg, np_cfg);
        // Wait for the launch to complete (snapshot shows is_running or last_error).
        while (true) {
            auto gs = game_runner_.snapshot();
            if (gs.is_running) {
                transition_to(UiState::InGame);
                break;
            }
            if (!gs.launch_in_progress && !gs.last_error.empty()) {
                set_error(gs.last_error);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        return;
    }
    if (r.launch_training) {
        // User clicked "Launch Training" — start a training game in a
        // SEPARATE GameRunner (instance 1) so the primary game_runner_
        // is free for the netplay game when a peer connects.
        // The training game runs while the session keeps listening.
        // When a peer connects, we freeze the training (suspend +
        // minimize), launch netplay on game_runner_, and resume training
        // when the netplay match ends.
        training_runner_ = std::make_unique<launcher::GameRunner>(/*instance_id=*/1);
        training_runner_->launch_offline_async(cfg, {/*training=*/true});
        transition_to(UiState::TrainingWhileHosting);
        return;
    }
    if (!r.error_message.empty()) {
        set_error(r.error_message);
        end_session();
        return;
    }
    if (r.cancelled) {
        end_session();
        transition_to(UiState::Idle);
        return;
    }
}

void MainMenu::drawTrainingWhileHosting(caster::common::config::Config& cfg) {
    namespace ut = caster::common::ui_theme;

    // Only require training_runner_ during the Idle and WaitingForAccept
    // phases. During the transition phases (KillingTraining, Deinitsession,
    // LaunchingNetplay), training_runner_ may already be reset (we kill it
    // in KillingTraining), so we must NOT abort to Idle just because it's
    // null.
    if (training_phase_ == TrainingPhase::Idle ||
        training_phase_ == TrainingPhase::WaitingForAccept) {
        if (!session_ || !training_runner_) {
            if (training_runner_) training_runner_->force_kill_async();
            training_runner_.reset();
            training_phase_ = TrainingPhase::Idle;
            transition_to(UiState::Idle);
            return;
        }
    } else {
        // Transition phases: require session_ (for deinit), but
        // training_runner_ may be null.
        if (!session_ && training_phase_ != TrainingPhase::LaunchingNetplay) {
            training_phase_ = TrainingPhase::Idle;
            transition_to(UiState::Idle);
            return;
        }
    }

    // Read both snapshots once per frame.
    // training_runner_ may be null during transition phases (we reset it
    // in KillingTraining after the game exits).
    auto ses = session_ ? session_->snapshot() : session::SessionSnapshot{};
    auto gs  = training_runner_ ? training_runner_->snapshot()
                                : launcher::GameRunnerSnapshot{};

    // ---- Non-blocking state machine for the transition phases ----
    // Each phase checks the snapshots and advances when ready. No while
    // loops, no blocking sleeps — the UI thread must stay responsive.

    switch (training_phase_) {
        case TrainingPhase::Idle: {
            // Training running, session listening. Check for peer connection.
            if (ses.state == session::SessionState::WaitingConfirmation) {
                training_phase_ = TrainingPhase::WaitingForAccept;
            } else if (ses.state == session::SessionState::Launching) {
                // Auto-confirm (shouldn't happen for host, but handle it).
                session_->host_confirm_async();
                pending_np_cfg_ = ses.config;
                training_runner_->force_kill_async();
                training_phase_ = TrainingPhase::KillingTraining;
            } else if (ses.state == session::SessionState::Failed) {
                training_runner_->force_kill_async();
                set_error(ses.error_message);
                end_session();
                training_runner_.reset();
                training_phase_ = TrainingPhase::Idle;
                transition_to(UiState::Idle);
                return;
            } else if (ses.state == session::SessionState::Cancelled) {
                training_runner_->force_kill_async();
                end_session();
                training_runner_.reset();
                training_phase_ = TrainingPhase::Idle;
                transition_to(UiState::Idle);
                return;
            }
            // Check if training exited on its own.
            if (!gs.is_running && !gs.launch_in_progress) {
                training_runner_.reset();
                training_phase_ = TrainingPhase::Idle;
                transition_to(UiState::WaitingForPeer);
                return;
            }
            break;
        }

        case TrainingPhase::WaitingForAccept: {
            // Peer connected. Show "Start Match" button. Stay here until
            // the user clicks it (which calls host_confirm_async and
            // transitions to the launch sequence).
            if (ses.state == session::SessionState::Launching) {
                // User clicked Start Match, session moved to Launching.
                pending_np_cfg_ = ses.config;
                // Kill the training game (simpler than freeze — no
                // suspend/sound/input issues). The training state is lost
                // but the netplay match starts clean.
                training_runner_->force_kill_async();
                training_phase_ = TrainingPhase::KillingTraining;
            } else if (ses.state == session::SessionState::Failed ||
                       ses.state == session::SessionState::Cancelled) {
                training_runner_->force_kill_async();
                end_session();
                training_runner_.reset();
                training_phase_ = TrainingPhase::Idle;
                transition_to(UiState::Idle);
                return;
            }
            break;
        }

        case TrainingPhase::KillingTraining: {
            // Wait for the training game to fully exit (non-blocking —
            // check once per frame). TerminateProcess is async, so we
            // need to wait before launching the netplay game to avoid
            // pipe name conflicts.
            if (!gs.is_running && !gs.launch_in_progress) {
                // Training is dead. Clean up the runner and deinit session.
                training_runner_.reset();
                session_->deinit_async();
                training_phase_ = TrainingPhase::Deinitsession;
            }
            break;
        }

        case TrainingPhase::Deinitsession: {
            // Wait for session to return to Idle (non-blocking).
            if (session_->snapshot().state == session::SessionState::Idle) {
                end_session();
                // Launch the netplay game on the primary game_runner_.
                game_runner_.launch_after_handshake_async(cfg, pending_np_cfg_);
                training_phase_ = TrainingPhase::LaunchingNetplay;
            }
            break;
        }

        case TrainingPhase::LaunchingNetplay: {
            // Wait for the netplay game to start (non-blocking).
            auto ngs = game_runner_.snapshot();
            if (ngs.is_running) {
                training_phase_ = TrainingPhase::Idle;
                transition_to(UiState::InGame);
                return;
            }
            if (!ngs.launch_in_progress && !ngs.last_error.empty()) {
                // Launch failed — resume training and show error.
                training_runner_->resume_async();
                training_runner_->restore_window_async();
                set_error(ngs.last_error);
                training_phase_ = TrainingPhase::Idle;
                return;
            }
            // If launch_in_progress is false but no error and not running,
            // something is wrong — show a diagnostic.
            if (!ngs.launch_in_progress && !ngs.is_running && ngs.last_error.empty()) {
                set_error("Netplay launch completed but game is not running (unknown reason)");
                training_runner_->resume_async();
                training_runner_->restore_window_async();
                training_phase_ = TrainingPhase::Idle;
                return;
            }
            break;
        }

        // No default case needed — all enum values are handled above.
    }

    // ---- Render ----
    // Show different cards depending on the phase.
    if (training_phase_ == TrainingPhase::WaitingForAccept) {
        // "OPPONENT CONNECTED!" card with Start Match button.
        constexpr float card_w = 560.0f;
        constexpr float card_h = 280.0f;
        if (ut::beginCenteredCard("##accept_match", card_w, card_h, false)) {
            ut::cardTitle("OPPONENT CONNECTED!");
            ImGui::Spacing();
            if (!ses.config.remote_name.empty()) {
                ImGui::BulletText("Opponent: %s", ses.config.remote_name.c_str());
            }
            ImGui::BulletText("Ping (avg/min/max): %.0f / %.0f / %.0f ms",
                              ses.stats.avg_ms, ses.stats.min_ms, ses.stats.max_ms);
            ImGui::BulletText("Auto input delay: %d frames", ses.config.delay);
            ImGui::Spacing();
            ImGui::TextWrapped("Click Start Match to begin. The training "
                               "game will be frozen and minimized, and the "
                               "netplay match will start.");
            ImGui::Spacing();
            if (ut::primaryButton("Start Match", 200, 40)) {
                session_->host_confirm_async();
            }
            ImGui::SameLine();
            if (ut::secondaryButton("Cancel", 120, 32)) {
                training_runner_->force_kill_async();
                session_->cancel_async();
            }
            ut::endCard();
        }
        return;
    }

    if (training_phase_ == TrainingPhase::KillingTraining ||
        training_phase_ == TrainingPhase::Deinitsession ||
        training_phase_ == TrainingPhase::LaunchingNetplay) {
        // Transition in progress — show a spinner with diagnostics.
        auto ngs = game_runner_.snapshot();
        constexpr float card_w = 520.0f;
        constexpr float card_h = 240.0f;
        if (ut::beginCenteredCard("##transition", card_w, card_h, false)) {
            ut::cardTitle("STARTING NETPLAY MATCH...");
            const char* phase_label = "";
            switch (training_phase_) {
                case TrainingPhase::KillingTraining:  phase_label = "Closing training game..."; break;
                case TrainingPhase::Deinitsession:    phase_label = "Releasing network..."; break;
                case TrainingPhase::LaunchingNetplay: phase_label = "Launching netplay game..."; break;
                default: break;
            }
            ImGui::TextUnformatted(phase_label);
            ImGui::Spacing();
            // Diagnostics — show the game_runner snapshot state so we can
            // see what's happening if it gets stuck.
            ImGui::BulletText("launch_in_progress: %s", ngs.launch_in_progress ? "true" : "false");
            ImGui::BulletText("is_running: %s", ngs.is_running ? "true" : "false");
            ImGui::BulletText("pid: %u", ngs.pid);
            if (!ngs.last_error.empty()) {
                ImGui::BulletText("last_error: %s", ngs.last_error.c_str());
            }
            ut::endCard();
        }
        return;
    }

    // Default: training running + session listening.
    constexpr float card_w = 720.0f;
    constexpr float card_h = 460.0f;
    if (ut::beginCenteredCard("##training_hosting", card_w, card_h, false)) {
        ut::cardTitle("TRAINING WHILE HOSTING");

        ImGui::Separator();
        ImGui::TextDisabled("Training game");
        if (gs.launch_in_progress) {
            ImGui::TextDisabled("  Launching...");
        } else if (gs.is_running) {
            ImGui::BulletText("PID: %u", gs.pid);
            ImGui::BulletText("IPC: %s",
                              gs.ipc_handshake_done ? "complete" : "pending");
        } else {
            ImGui::TextDisabled("  (not running)");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextDisabled("Host session");
        ImGui::BulletText("State: %s", ses.status_message.c_str());
        if (ses.room_code) {
            ImGui::BulletText("Room code: #%s", ses.room_code->c_str());
        }
        if (ses.public_ip) {
            ImGui::BulletText("Public IP: %s:%d", ses.public_ip->c_str(),
                              ses.config.peer_port);
        }
        if (ses.remaining_seconds) {
            ImGui::BulletText("Timeout in: %us", *ses.remaining_seconds);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextWrapped("Training is running while waiting for an opponent. "
                           "When a peer connects, the training game will be "
                           "frozen and minimized, and the netplay match will "
                           "start. When the match ends, training resumes.");

        ImGui::Spacing();
        if (ut::secondaryButton("Stop Training", 160, 32)) {
            training_runner_->force_kill_async();
        }
        ImGui::SameLine();
        if (ut::secondaryButton("Cancel Host", 140, 32)) {
            training_runner_->force_kill_async();
            session_->cancel_async();
        }

        ut::endCard();
    }
}

void MainMenu::drawInGame() {
    namespace ut = caster::common::ui_theme;

    // Read the game runner snapshot. The worker polls is_alive + IPC
    // continuously; we just read the result.
    auto gs = game_runner_.snapshot();

    // If the game has exited naturally, check if the DLL sent a stop
    // reason (desync, timeout, disconnect, etc.) and show it to the user
    // before returning to Idle.
    if (!gs.is_running && !gs.launch_in_progress) {
        // If we came from training-while-hosting, clean up the training
        // runner (it was killed when the match started). Go back to Idle
        // — the user can start a new host+training if they want.
        if (training_runner_) {
            training_runner_.reset();
            training_phase_ = TrainingPhase::Idle;
        }
        if (!gs.stop_reason.empty()) {
            set_error("Game stopped: " + gs.stop_reason);
        } else if (!gs.last_error.empty()) {
            set_error(gs.last_error);
        } else {
            transition_to(UiState::Idle);
        }
        return;
    }

    // If a launch is in progress, show a launching screen.
    if (gs.launch_in_progress) {
        constexpr float card_w = 480.0f;
        constexpr float card_h = 180.0f;
        if (ut::beginCenteredCard("##launching", card_w, card_h, false)) {
            ut::cardTitle("LAUNCHING GAME...");
            ImGui::TextDisabled("Please wait while the game starts.");
            ImGui::Spacing();
            // Simple spinner using dots.
            const int dots = (static_cast<int>(ImGui::GetTime() * 4.0f) % 4);
            std::string spinner = "Starting";
            for (int i = 0; i < dots; ++i) spinner += '.';
            ImGui::TextUnformatted(spinner.c_str());
            ut::endCard();
        }
        return;
    }

    // Game is running — show Process ID + Force Kill button.
    const std::uint32_t pid = gs.pid;

    constexpr float card_w = 520.0f;
    constexpr float card_h = 200.0f;
    if (ut::beginCenteredCard("##in_game", card_w, card_h, false)) {
        ut::cardTitle("GAME RUNNING");

        ImGui::BulletText("Process ID: %u", pid);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextWrapped("Click \"Force Kill\" to close the game and go "
                           "back to the menu.");

        ImGui::Spacing();
        if (ut::primaryButton("Force Kill", 160, 36)) {
            game_runner_.force_kill_async();
            // The next snapshot read will show is_running=false.
        }

        ut::endCard();
    }
}

void MainMenu::drawErrorState() {
    namespace ut = caster::common::ui_theme;

    // Red-bordered card centered in the window.
    constexpr float card_w = 600.0f;
    constexpr float card_h = 200.0f;
    ut::pushStyleColor(ImGuiCol_Border, ut::active_theme().accent);
    if (ut::beginCenteredCard("##error", card_w, card_h, false)) {
        ut::cardTitle("ERROR");
        if (error_message_.empty()) {
            ut::drawErrorText("(no error message)");
        } else {
            ut::drawErrorText("%s", error_message_.c_str());
        }
        ImGui::Spacing();
        if (ut::primaryButton("OK", 120, 32)) {
            clear_error();
        }
        ut::endCard();
    }
    ut::popStyleColor();
}

} // namespace caster::exe::pages
