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

        // Launch the game with the netplay config.
        auto launch_r = game_runner_.launch_after_handshake(cfg, np_cfg);
        if (launch_r.success) {
            transition_to(UiState::InGame);
        } else {
            set_error(launch_r.error_message);
        }
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

void MainMenu::drawInGame() {
    namespace ut = caster::common::ui_theme;

    // Poll the game runner. If the game has exited naturally, check if
    // the DLL sent a stop reason (desync, timeout, disconnect, etc.) and
    // show it to the user before returning to Idle.
    if (!game_runner_.update()) {
        auto reason = game_runner_.stop_reason();
        if (!reason.empty()) {
            set_error("Game stopped: " + reason);
        } else {
            transition_to(UiState::Idle);
        }
        return;
    }

    const std::uint32_t pid = game_runner_.pid();

    // Centered card showing PID + Force Kill button.
    constexpr float card_w = 560.0f;
    constexpr float card_h = 240.0f;
    if (ut::beginCenteredCard("##in_game", card_w, card_h, false)) {
        ut::cardTitle("GAME RUNNING");

        ImGui::BulletText("PID              : %u", pid);
        ImGui::BulletText("IPC handshake   : %s",
                          game_runner_.ipc_handshake_done() ? "complete"
                                                            : "pending");
        ImGui::BulletText("Process state    : alive");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextWrapped("The game is running with hook.dll injected. "
                           "Click 'Force Kill' to terminate it and return "
                           "to the menu.");

        ImGui::Spacing();
        if (ut::primaryButton("Force Kill", 160, 36)) {
            game_runner_.force_kill();
            // The next update() call will detect the exit and transition
            // back to Idle.
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
