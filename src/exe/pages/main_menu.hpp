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
// GameRunner is now wired up. The Play page can launch the game
// in offline mode (Training / Versus), which transitions to InGame.
// The InGame state polls GameRunner::update() each frame and shows the
// PID + Force Kill button.

#pragma once

#include "../ui_state.hpp"
#include "../launcher/game_runner.hpp"
#include "../session/session.hpp"
#include "controllers_page.hpp"
#include "config_page.hpp"
#include "play_page.hpp"
#include "../../common/config.hpp"

#include <memory>
#include <string>

namespace caster::exe::pages {

class MainMenu {
public:
    MainMenu();
    ~MainMenu();

    // Draw one frame of the launcher UI. Call this from the GuiWindow's
    // pump_frame callback. Returns true to keep running; false to quit.
    //
    // `cfg` is non-const because the config page modifies it on Apply.
    bool draw(caster::common::config::Config& cfg);

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

    // Accessor used by play_page to start netplay sessions.
    session::NetplaySession* session() { return session_.get(); }
    void start_session() {
        if (!session_) session_ = std::make_unique<session::NetplaySession>();
    }
    void end_session() { session_.reset(); }

    // Called once after construction to set the mapping.ini path. Uses
    // SDL_GetBasePath() to resolve <exe_dir>/caster/mapping.ini.
    void init_controller_state();

    // Called on shutdown to close any open SDL_Joystick handles.
    void shutdown_controller_state();

private:
    UiState       state_           = UiState::Idle;
    MenuPage      page_            = MenuPage::Play;
    bool          quit_requested_  = false;
    std::string   error_message_;  // populated when state_ == ErrorState
    launcher::GameRunner game_runner_;
    std::unique_ptr<session::NetplaySession> session_;
    controllers_page::State controllers_state_;
    config_page::State      config_state_;
    play_page::State        play_state_;

    // Sub-draw methods, called by draw() depending on state_.
    void drawIdle(caster::common::config::Config& cfg);
    void drawWaitingForPeer(caster::common::config::Config& cfg);
    void drawInGame();
    void drawErrorState();

    // Region draws for the idle layout.
    void drawHeader();
    void drawSidebar();
    void drawContent(caster::common::config::Config& cfg);
};

} // namespace caster::exe::pages
