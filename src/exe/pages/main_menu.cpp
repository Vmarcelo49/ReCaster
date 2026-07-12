// src/exe/pages/main_menu.cpp

#include "main_menu.hpp"
#include "header.hpp"
#include "sidebar.hpp"
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
    // Full-window root: covers 1024×768, no chrome.
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(ut::WINDOW_W, ut::WINDOW_H));
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
    drawSidebar();
    drawContent(cfg);
}

void MainMenu::drawHeader() {
    header::draw();
}

void MainMenu::drawSidebar() {
    bool quit_clicked = false;
    sidebar::draw(page_, quit_clicked);
    if (quit_clicked) {
        quit_requested_ = true;
    }
}

void MainMenu::drawContent(caster::common::config::Config& cfg) {
    namespace ut = caster::common::ui_theme;

    // Content area: starts at (SIDEBAR_W, HEADER_H), extends to (1024, 768).
    const float x = ut::SIDEBAR_W + ut::CONTENT_PAD;
    const float y = ut::HEADER_H + ut::CONTENT_PAD;
    const float w = ut::WINDOW_W - ut::SIDEBAR_W - 2 * ut::CONTENT_PAD;
    const float h = ut::WINDOW_H - ut::HEADER_H - 2 * ut::CONTENT_PAD;

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

    if (!session_ || !training_runner_) {
        if (training_runner_) training_runner_->force_kill_async();
        training_runner_.reset();
        training_phase_ = TrainingPhase::Idle;
        transition_to(UiState::Idle);
        return;
    }

    // Read both snapshots once per frame.
    auto ses = session_->snapshot();
    auto gs  = training_runner_->snapshot();

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
                training_phase_ = TrainingPhase::FreezingTraining;
                training_runner_->suspend_async();
                training_runner_->minimize_window_async();
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
            // transitions to FreezingTraining).
            if (ses.state == session::SessionState::Launching) {
                // User clicked Start Match, session moved to Launching.
                pending_np_cfg_ = ses.config;
                training_phase_ = TrainingPhase::FreezingTraining;
                training_runner_->suspend_async();
                training_runner_->minimize_window_async();
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

        case TrainingPhase::FreezingTraining: {
            // Wait for the training to be suspended (non-blocking — just
            // check once per frame).
            if (gs.is_suspended) {
                // Training is frozen. Now deinit the session.
                if (!pending_np_cfg_.is_host) {
                    // Client: sleep 500ms so host gets our confirm.
                    // We can't sleep on the UI thread, so we just proceed
                    // — the 500ms is a safety margin, not a hard requirement.
                }
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
            break;
        }

        case TrainingPhase::ResumingTraining:
            // Handled in drawInGame.
            break;

        case TrainingPhase::Done:
            training_phase_ = TrainingPhase::Idle;
            break;
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

    if (training_phase_ == TrainingPhase::FreezingTraining ||
        training_phase_ == TrainingPhase::Deinitsession ||
        training_phase_ == TrainingPhase::LaunchingNetplay) {
        // Transition in progress — show a spinner.
        constexpr float card_w = 480.0f;
        constexpr float card_h = 180.0f;
        if (ut::beginCenteredCard("##transition", card_w, card_h, false)) {
            ut::cardTitle("STARTING NETPLAY MATCH...");
            ImGui::TextDisabled("Freezing training and launching netplay.");
            ImGui::Spacing();
            const char* phase_label = "";
            switch (training_phase_) {
                case TrainingPhase::FreezingTraining: phase_label = "Freezing training..."; break;
                case TrainingPhase::Deinitsession:    phase_label = "Releasing network..."; break;
                case TrainingPhase::LaunchingNetplay: phase_label = "Launching netplay game..."; break;
                default: break;
            }
            ImGui::TextUnformatted(phase_label);
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
        // If we have a suspended training game (training-while-hosting),
        // resume it and restore its window, then go back to
        // TrainingWhileHosting with a fresh session.
        if (training_runner_ && training_runner_->snapshot().is_suspended) {
            training_runner_->resume_async();
            training_runner_->restore_window_async();
            // Restart the session listener so the player can accept
            // another opponent.
            start_session();
            if (session_) {
                // Use a default name + connection type — the original cfg
                // isn't available here, but these are just for display.
                // The peer will see whatever name the session sends.
                session_->start_smart_host_async(/*relay_source=*/"",
                    caster::common::config::kDefaultPort, /*training=*/false);
            }
            transition_to(UiState::TrainingWhileHosting);
            return;
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

    // Game is running — show PID + Force Kill button.
    const std::uint32_t pid = gs.pid;

    constexpr float card_w = 560.0f;
    constexpr float card_h = 240.0f;
    if (ut::beginCenteredCard("##in_game", card_w, card_h, false)) {
        ut::cardTitle("GAME RUNNING");

        ImGui::BulletText("PID              : %u", pid);
        ImGui::BulletText("IPC handshake   : %s",
                          gs.ipc_handshake_done ? "complete" : "pending");
        ImGui::BulletText("Process state    : alive");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextWrapped("The game is running with hook.dll injected. "
                           "Click 'Force Kill' to terminate it and return "
                           "to the menu.");

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
    ut::pushStyleColor(ImGuiCol_Border, ut::COL_RED);
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
