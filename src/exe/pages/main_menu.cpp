// src/exe/pages/main_menu.cpp

#include "main_menu.hpp"
#include "header.hpp"
#include "sidebar.hpp"
#include "play_page.hpp"
#include "config_page.hpp"
#include "controllers_page.hpp"

#include "../../common/config.hpp"
#include "../../common/logger.hpp"
#include "../../common/ui_theme.hpp"

#include <SDL2/SDL.h>
#include <imgui.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace caster::exe::pages {

namespace {

constexpr int kWindowW = 1024;
constexpr int kWindowH = 768;

} // namespace

MainMenu::MainMenu() = default;

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
    ImGui::SetNextWindowSize(ImVec2(kWindowW, kWindowH));
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
            drawWaitingForPeer();
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
    const float w = kWindowW - ut::SIDEBAR_W - 2 * ut::CONTENT_PAD;
    const float h = kWindowH - ut::HEADER_H - 2 * ut::CONTENT_PAD;

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

void MainMenu::drawWaitingForPeer() {
    namespace ut = caster::common::ui_theme;

    // Phase 8: full netplay handshake UI. For now, placeholder card.
    const float card_w = 560.0f;
    const float card_h = 200.0f;
    ImGui::SetCursorPos(ImVec2((kWindowW - card_w) / 2,
                               (kWindowH - card_h) / 2));
    if (ut::beginCard("##waiting", card_w, card_h, false)) {
        ut::cardTitle("WAITING FOR PEER");
        ImGui::TextWrapped("Phase 8 will add the full handshake UI here "
                           "(room code, ping, delay, start match).");
        ImGui::Spacing();
        if (ut::secondaryButton("Cancel", 120, 32)) {
            transition_to(UiState::Idle);
        }
        ut::endCard();
    }
}

void MainMenu::drawInGame() {
    namespace ut = caster::common::ui_theme;

    // Poll the game runner. If the game has exited naturally, transition
    // back to Idle. This must be called every frame while in InGame state.
    if (!game_runner_.update()) {
        // Game exited (either naturally or via Force Kill).
        transition_to(UiState::Idle);
        return;
    }

    const std::uint32_t pid = game_runner_.pid();

    // Centered card showing PID + Force Kill button.
    const float card_w = 560.0f;
    const float card_h = 240.0f;
    ImGui::SetCursorPos(ImVec2((kWindowW - card_w) / 2,
                               (kWindowH - card_h) / 2));
    if (ut::beginCard("##in_game", card_w, card_h, false)) {
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
            // The next drawInGame() call will detect !is_running() and
            // transition back to Idle.
        }

        ut::endCard();
    }
}

void MainMenu::drawErrorState() {
    namespace ut = caster::common::ui_theme;

    // Red-bordered card centered in the window.
    const float card_w = 600.0f;
    const float card_h = 200.0f;
    ImGui::SetCursorPos(ImVec2((kWindowW - card_w) / 2,
                               (kWindowH - card_h) / 2));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.75f, 0.20f, 0.15f, 1.0f));
    if (ut::beginCard("##error", card_w, card_h, false)) {
        ut::cardTitle("ERROR");
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(error_message_.empty()
                               ? "(no error message)"
                               : error_message_.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if (ut::primaryButton("OK", 120, 32)) {
            clear_error();
        }
        ut::endCard();
    }
    ImGui::PopStyleColor();
}

} // namespace caster::exe::pages
