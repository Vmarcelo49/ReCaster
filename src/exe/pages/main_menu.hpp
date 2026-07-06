// src/exe/pages/main_menu.hpp
//
// Top-level launcher UI. Owns the UiState + MenuPage + GameRunner and
// orchestrates the three regions of the main menu:
//   - Header bar (top 64px): logo + version
//   - Sidebar (left 56px): P / C / M nav buttons + Q at the bottom
//   - Content area: the currently-selected page
//
// Also dispatches to the full-screen views (WaitingForPeer / InGame /
// ErrorState) when UiState != Idle.
//
// Phase 5: GameRunner is now wired up. The Play page can launch the game
// in offline mode (Training / Versus), which transitions to InGame.
// The InGame state polls GameRunner::update() each frame and shows the
// PID + Force Kill button.

#pragma once

#include "../ui_state.hpp"
#include "../launcher/game_runner.hpp"
#include "../../common/config.hpp"

#include <string>

namespace caster::exe::pages {

class MainMenu {
public:
    MainMenu();

    // Draw one frame of the launcher UI. Call this from the GuiWindow's
    // pump_frame callback. Returns true to keep running; false to quit.
    bool draw(const caster::common::config::Config& cfg);

    // External triggers (called by netplay/launcher code in later phases).
    void transition_to(UiState new_state);
    void set_error(const std::string& message);
    void clear_error();
    void request_quit() { quit_requested_ = true; }

    UiState   state()     const { return state_; }
    MenuPage  page()      const { return page_; }
    bool      quit_requested() const { return quit_requested_; }

    // Accessor used by play_page to trigger launches.
    launcher::GameRunner& game_runner() { return game_runner_; }

private:
    UiState       state_           = UiState::Idle;
    MenuPage      page_            = MenuPage::Play;
    bool          quit_requested_  = false;
    std::string   error_message_;  // populated when state_ == ErrorState
    launcher::GameRunner game_runner_;

    // Sub-draw methods, called by draw() depending on state_.
    void drawIdle(const caster::common::config::Config& cfg);
    void drawWaitingForPeer();
    void drawInGame();
    void drawErrorState();

    // Region draws for the idle layout.
    void drawHeader();
    void drawSidebar();
    void drawContent(const caster::common::config::Config& cfg);
};

} // namespace caster::exe::pages
