// src/exe/pages/controllers_page.cpp

#include "controllers_page.hpp"
#include "controller_helpers.hpp"
#include "../../common/controller/mapping.hpp"
#include "../../common/logger.hpp"
#include "../../common/ui_theme.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_joystick.h>

#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace caster::exe::pages::controllers_page {

namespace {

namespace cm = caster::common::controller;
namespace ut = caster::common::ui_theme;

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

// Re-sync device_sel from mapping.device_index (e.g. after load_mapping).
void sync_device_sel(State& state) {
    state.p1_device_sel = state.p1.device_index + 1;
    state.p2_device_sel = state.p2.device_index + 1;
}

// Open the joystick for the current device_sel (without closing first).
// Used after load_mapping to open the right joystick.
void open_joystick_for_sel(int device_sel, SDL_Joystick*& joy) {
    if (joy) {
        SDL_JoystickClose(joy);
        joy = nullptr;
    }
    if (device_sel > 0) {
        joy = SDL_JoystickOpen(device_sel - 1);
    }
}

} // namespace

void load_mapping(State& state) {
    if (state.loaded) return;

    // Start from defaults so missing keys don't leave us with empty mappings.
    state.p1 = cm::ControllerMapping::default_xbox();
    state.p2 = cm::ControllerMapping::default_xbox();

    if (state.mapping_path.empty()) {
        state.loaded = true;
        return;
    }

    if (cm::load_mapping(state.mapping_path, state.p1, state.p2)) {
        // Loaded successfully — sync device_sel from the loaded device_index.
        sync_device_sel(state);
        open_joystick_for_sel(state.p1_device_sel, state.p1_joy);
        open_joystick_for_sel(state.p2_device_sel, state.p2_joy);
    }
    state.loaded = true;
}

void close_joysticks(State& state) {
    if (state.p1_joy) {
        SDL_JoystickClose(state.p1_joy);
        state.p1_joy = nullptr;
    }
    if (state.p2_joy) {
        SDL_JoystickClose(state.p2_joy);
        state.p2_joy = nullptr;
    }
}

void draw(State& state) {
    namespace ut = caster::common::ui_theme;

    // Load on first frame.
    if (!state.loaded) {
        load_mapping(state);
    }

    const std::int64_t now = now_ms();

    // Title row.
    ut::cardTitle("CONTROLLERS");
    ImGui::Spacing();

    bool changed = false;

    // Two cards side-by-side (list view only).
    const float card_w = 920.0f;
    const float gap    = 16.0f;

    if (ut::beginCard("P1_list", (card_w - gap) / 2, 0, /*auto_y=*/true)) {
        if (controller_helpers::draw_list_panel(
                "Player 1", state.p1, state.p1_bind_target, state.p1_joy,
                state.p1_device_sel, state.p1_cooldown_until_ms, now)) {
            changed = true;
        }
        ut::endCard();
    }
    ImGui::SameLine(0, gap);
    if (ut::beginCard("P2_list", (card_w - gap) / 2, 0, /*auto_y=*/true)) {
        if (controller_helpers::draw_list_panel(
                "Player 2", state.p2, state.p2_bind_target, state.p2_joy,
                state.p2_device_sel, state.p2_cooldown_until_ms, now)) {
            changed = true;
        }
        ut::endCard();
    }

    // Auto-save on any change.
    if (changed && !state.mapping_path.empty()) {
        cm::save_mapping(state.mapping_path, state.p1, state.p2);
    }
}

} // namespace caster::exe::pages::controllers_page
