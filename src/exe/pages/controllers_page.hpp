// src/exe/pages/controllers_page.hpp
//
// The "Controllers" page — P1/P2 controller mapping UI.
//
// full UI with click-to-bind, SOCD radios, deadzone slider,
// air dash macro toggle, Default Bindings / Clear buttons, list/grid
// view toggle, and auto-save to caster/mapping.ini on any change.

#pragma once

#include "../../common/controller/mapping.hpp"

#include <string>

// Forward decl — SDL_Joystick is in <SDL2/SDL_joystick.h>.
// SDL2 declares `typedef struct _SDL_Joystick SDL_Joystick;`.
struct _SDL_Joystick;
using SDL_Joystick = struct _SDL_Joystick;

namespace caster::exe::pages::controllers_page {

// State held across frames for the controllers page. The MainMenu owns
// one instance and passes it to draw() each frame.
struct State {
    caster::common::controller::ControllerMapping p1;
    caster::common::controller::ControllerMapping p2;

    // Which slot is currently in bind mode for each player.
    caster::common::controller::BindingTarget p1_bind_target =
        caster::common::controller::BindingTarget::None;
    caster::common::controller::BindingTarget p2_bind_target =
        caster::common::controller::BindingTarget::None;

    // Currently-open SDL_Joystick for each player (closed when device
    // changes, opened via SDL_JoystickOpen).
    SDL_Joystick* p1_joy = nullptr;
    SDL_Joystick* p2_joy = nullptr;

    // Index into the device combo (0 = Keyboard, 1+ = joystick N).
    // Differs from mapping.device_index by 1.
    int p1_device_sel = 0;
    int p2_device_sel = 0;

    // Cooldown timestamps (wall-clock ms) — bind mode is suppressed while
    // now < cooldown_until_ms. Set when a bind button is clicked, to avoid
    // the click itself being detected as an input.
    std::int64_t p1_cooldown_until_ms = 0;
    std::int64_t p2_cooldown_until_ms = 0;

    // Toggle between grid view (false) and list view (true).
    bool list_view = false;

    // Path to the mapping.ini file (resolved by the caller).
    std::string mapping_path;

    // Have we loaded the mapping file at least once?
    bool loaded = false;
};

// Draw the controllers page inside the content area.
// `state` is owned by the caller (MainMenu) and persists across frames.
void draw(State& state);

// Load mapping.ini into state. Called once on first draw.
void load_mapping(State& state);

// Close any open SDL_Joystick handles. Called on shutdown.
void close_joysticks(State& state);

} // namespace caster::exe::pages::controllers_page
