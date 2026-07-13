// src/exe/pages/config_page.hpp
//
// The "Game Config" page — player profile, match rules, network settings.
//
// real InputText fields + Apply buttons that modify the Config
// and persist it to disk via config::save().

#pragma once

#include "../../common/config.hpp"

#include <cstdint>
#include <string>

namespace caster::exe::pages::config_page {

// State held across frames. Owned by MainMenu.
struct State {
    // Input buffers (ImGui wants raw char arrays).
    char name_buf[40]      = {0};  // display_name (max 31 chars + NUL)
    char wincount_buf[8]   = {0};  // versus_win_count (1..99)
    char relay_buf[1024]   = {0};  // relay_servers (multi-line)

    // Have we initialized the buffers from the loaded Config?
    bool initialized = false;

    // "Saved!" feedback — cleared after 2 seconds.
    std::string  last_saved_field;
    std::int64_t saved_feedback_until_ms = 0;
};

// Draw the config page. `cfg` is modified in place when the user clicks
// Apply; the caller is responsible for persisting via config::save() (but
// we also call it directly here for immediacy).
void draw(caster::common::config::Config& cfg, State& state);

} // namespace caster::exe::pages::config_page
