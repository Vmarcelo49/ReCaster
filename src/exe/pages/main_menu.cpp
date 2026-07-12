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
        // User clicked "Launch Training" — start a training game in the
        // background while the session keeps listening for peers.
        game_runner_.launch_offline_async(cfg, {/*training=*/true});
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

    if (!session_) {
        // Shouldn't happen — but be safe.
        game_runner_.force_kill_async();
        transition_to(UiState::Idle);
        return;
    }

    // Read both snapshots once per frame.
    auto ses = session_->snapshot();
    auto gs  = game_runner_.snapshot();

    // ---- Handle session terminal states ----
    if (ses.state == session::SessionState::Launching) {
        // Peer connected + handshake completed. Kill the training game,
        // wait for it to exit, then launch the netplay game.
        game_runner_.force_kill_async();
        // Wait for the training game to fully exit before relaunching
        // (TerminateProcess is async — pipe name would conflict otherwise).
        while (game_runner_.snapshot().is_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // Snapshot the netplay config BEFORE deinit.
        auto np_cfg = ses.config;
        // Client: sleep 500ms before deinit so the host receives our confirm.
        if (!np_cfg.is_host) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        session_->deinit_async();
        while (session_->snapshot().state != session::SessionState::Idle) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        end_session();

        // Launch the netplay game.
        game_runner_.launch_after_handshake_async(cfg, np_cfg);
        while (true) {
            auto ngs = game_runner_.snapshot();
            if (ngs.is_running) {
                transition_to(UiState::InGame);
                break;
            }
            if (!ngs.launch_in_progress && !ngs.last_error.empty()) {
                set_error(ngs.last_error);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        return;
    }
    if (ses.state == session::SessionState::Failed) {
        game_runner_.force_kill_async();
        set_error(ses.error_message);
        end_session();
        return;
    }
    if (ses.state == session::SessionState::Cancelled) {
        game_runner_.force_kill_async();
        end_session();
        transition_to(UiState::Idle);
        return;
    }

    // ---- Handle training game exit ----
    // If the training game exited on its own (user closed it), fall back
    // to plain WaitingForPeer. The session is still listening.
    if (!gs.is_running && !gs.launch_in_progress) {
        if (!gs.stop_reason.empty()) {
            // Training game sent a stop reason — show it briefly, then
            // fall back to WaitingForPeer.
            caster::common::logger::info("training while hosting: game stopped: {}",
                                         gs.stop_reason);
        }
        transition_to(UiState::WaitingForPeer);
        return;
    }

    // ---- Render the combined card ----
    constexpr float card_w = 720.0f;
    constexpr float card_h = 460.0f;
    if (ut::beginCenteredCard("##training_hosting", card_w, card_h, false)) {
        ut::cardTitle("TRAINING WHILE HOSTING");

        // ---- Training game status (left side) ----
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

        // ---- Host session status (right side) ----
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
                           "closed automatically and the netplay match will "
                           "start.");

        ImGui::Spacing();

        // ---- Buttons ----
        if (ut::secondaryButton("Stop Training", 160, 32)) {
            game_runner_.force_kill_async();
            // The next frame will detect !is_running and fall back to
            // WaitingForPeer.
        }
        ImGui::SameLine();
        if (ut::secondaryButton("Cancel Host", 140, 32)) {
            game_runner_.force_kill_async();
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
