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
void open_joystick_for_sel(int device_sel, SDL_Joystick*& joy) {
    if (joy) {
        SDL_JoystickClose(joy);
        joy = nullptr;
    }
    if (device_sel > 0) {
        joy = SDL_JoystickOpen(device_sel - 1);
    }
}

// Draw a compact player label — accent-colored uppercase text.
void draw_player_label(const char* label) {
    const ut::Theme& t = ut::active_theme();
    if (t.font_body_sm()) ImGui::PushFont(t.font_body_sm());
    ut::pushStyleColor(ImGuiCol_Text, t.accent);
    ImGui::TextUnformatted(label);
    ut::popStyleColor();
    if (t.font_body_sm()) ImGui::PopFont();
    ImGui::Spacing();
}

} // namespace

void load_mapping(State& state) {
    if (state.loaded) return;

    state.p1 = cm::ControllerMapping::default_xbox();
    state.p2 = cm::ControllerMapping::default_xbox();

    if (state.mapping_path.empty()) {
        state.loaded = true;
        return;
    }

    if (cm::load_mapping(state.mapping_path, state.p1, state.p2)) {
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

// Helper: draw one player's column as a child window with fixed height.
// This guarantees the two columns sit side-by-side and each scrolls
// independently if its content is too tall.
static bool draw_player_column(const char* child_id,
                               const char* label,
                               const char* panel_name,
                               cm::ControllerMapping& mapping,
                               cm::BindingTarget& bind_target,
                               SDL_Joystick*& joy,
                               int& device_sel,
                               std::int64_t& cooldown_until_ms,
                               std::int64_t now,
                               float col_w,
                               float col_h) {
    bool changed = false;

    ImGui::BeginChild(child_id, ImVec2(col_w, col_h));
    ImGui::PushID(child_id);

    draw_player_label(label);

    // Don't PushItemWidth — let the controller_helpers use the natural
    // content region width inside this child window. Pushing col_w would
    // make widgets overflow because col_w includes the child's padding.
    if (controller_helpers::draw_list_panel(
            panel_name, mapping, bind_target, joy,
            device_sel, cooldown_until_ms, now)) {
        changed = true;
    }

    ImGui::PopID();
    ImGui::EndChild();

    return changed;
}

void draw(State& state) {
    namespace ut = caster::common::ui_theme;
    const ut::Theme& t = ut::active_theme();

    // Load on first frame.
    if (!state.loaded) {
        load_mapping(state);
    }

    const std::int64_t now = now_ms();
    bool changed = false;

    // ====================================================================
    // LAYOUT — split the content area vertically first, then draw each
    // player in its own half.
    //
    //   ┌──────────────────────┬──────────────────────┐
    //   │ PLAYER 1             │ PLAYER 2             │
    //   │  (child window,      │  (child window,      │
    //   │   fixed height,      │   fixed height,      │
    //   │   scrolls if needed) │   scrolls if needed) │
    //   │                      │                      │
    //   └──────────────────────┴──────────────────────┘
    //
    // Each column gets exactly half the available width (minus the gap),
    // and the FULL available height. This guarantees both players are
    // always visible side-by-side.
    // ====================================================================

    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float avail_h = ImGui::GetContentRegionAvail().y;
    const float col_gap = 24.0f;
    const float col_w = (avail_w - col_gap) * 0.5f;
    const float col_h = avail_h;

    // Record the top-left corner of the content area BEFORE drawing any
    // children. We need this to compute the separator X position — after
    // EndChild, GetCursorScreenPos() returns a position below the child,
    // not at its right edge.
    const ImVec2 content_top_left = ImGui::GetCursorScreenPos();

    // ---- LEFT HALF: Player 1 -----------------------------------------
    changed |= draw_player_column(
        "##p1_col", "PLAYER 1", "Player 1",
        state.p1, state.p1_bind_target, state.p1_joy,
        state.p1_device_sel, state.p1_cooldown_until_ms, now,
        col_w, col_h);

    // ---- Vertical separator (in the gap between the two columns) -----
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float sep_x = content_top_left.x + col_w + col_gap * 0.5f;
        const float y0 = content_top_left.y;
        const float y1 = content_top_left.y + col_h;
        dl->AddLine(ImVec2(sep_x, y0),
                    ImVec2(sep_x, y1),
                    ImGui::ColorConvertFloat4ToU32(
                        ImVec4(t.rule.x, t.rule.y, t.rule.z, t.rule.w)),
                    1.0f);
    }

    // ---- RIGHT HALF: Player 2 ----------------------------------------
    ImGui::SameLine(0, col_gap);
    changed |= draw_player_column(
        "##p2_col", "PLAYER 2", "Player 2",
        state.p2, state.p2_bind_target, state.p2_joy,
        state.p2_device_sel, state.p2_cooldown_until_ms, now,
        col_w, col_h);

    // Auto-save on any change.
    if (changed && !state.mapping_path.empty()) {
        cm::save_mapping(state.mapping_path, state.p1, state.p2);
    }
}

} // namespace caster::exe::pages::controllers_page
