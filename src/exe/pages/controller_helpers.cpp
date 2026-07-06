// src/exe/pages/controller_helpers.cpp

#include "controller_helpers.hpp"
#include "../../common/controller/binder.hpp"
#include "../../common/controller/mapping.hpp"
#include "../../common/logger.hpp"
#include "../../common/ui_theme.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_joystick.h>

#include <imgui.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace caster::exe::pages::controller_helpers {

namespace {

namespace cm = caster::common::controller;
namespace ut = caster::common::ui_theme;

// Fixed widths matching zzcaster.
constexpr float kBindButtonW     = 90.0f;
constexpr float kDirectionsCardW = 250.0f;
constexpr float kButtonsCardW    = 310.0f;
constexpr float kOptionsCardW    = 340.0f;
constexpr float kCardH           = 165.0f;

// Open the SDL joystick for `device_sel` and store it in `joy`.
// Closes the previously-open joystick first. device_sel == 0 means
// Keyboard → joy stays nullptr.
void select_device(int device_sel, SDL_Joystick*& joy) {
    if (joy) {
        SDL_JoystickClose(joy);
        joy = nullptr;
    }
    if (device_sel > 0) {
        // device_sel 1..N maps to SDL joystick index 0..N-1.
        const int sdl_index = device_sel - 1;
        joy = SDL_JoystickOpen(sdl_index);
        if (!joy) {
            caster::common::logger::warn("controller: SDL_JoystickOpen({}) failed: {}",
                                         sdl_index, SDL_GetError());
        }
    }
}

// Draw the device combo. Returns true if `device_sel` changed.
bool draw_device_combo(const char* id, int& device_sel,
                       const std::vector<DeviceEntry>& devices) {
    const char* preview = "";
    if (device_sel >= 0 && device_sel < static_cast<int>(devices.size())) {
        preview = devices[device_sel].display_name.c_str();
    }
    bool changed = false;
    if (ImGui::BeginCombo(id, preview)) {
        for (int i = 0; i < static_cast<int>(devices.size()); ++i) {
            const bool is_selected = (device_sel == i);
            if (ImGui::Selectable(devices[i].display_name.c_str(), is_selected)) {
                device_sel = i;
                changed = true;
            }
            if (is_selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// Poll for bind input (called once per frame per player when bind_target
// != None and cooldown has expired). If an input is detected, applies it
// to the mapping and clears bind_target.
void maybe_poll_bind(cm::ControllerMapping& mapping,
                     cm::BindingTarget& bind_target,
                     SDL_Joystick* joy,
                     int device_sel,
                     std::int64_t& cooldown_until_ms,
                     std::int64_t now_ms) {
    if (bind_target == cm::BindingTarget::None) return;
    if (cooldown_until_ms != 0 && now_ms < cooldown_until_ms) return;

    // Cooldown just expired — reset and start polling next frame.
    if (cooldown_until_ms != 0 && now_ms >= cooldown_until_ms) {
        cooldown_until_ms = 0;
    }

    const int device_idx = device_sel - 1;  // -1 = Keyboard, 0+ = joystick N
    cm::InputBinding b = cm::poll_for_bind_input(joy, device_idx);
    if (b.type != cm::InputType::None) {
        if (cm::InputBinding* slot = mapping.binding(bind_target)) {
            *slot = b;
        }
        bind_target = cm::BindingTarget::None;
    }
}

// Draw one bind button. Returns true if clicked (caller sets bind mode).
bool draw_bind_button(const char* label,
                      cm::BindingTarget target,
                      const cm::InputBinding& current,
                      cm::BindingTarget& bind_target,
                      std::int64_t& cooldown_until_ms,
                      std::int64_t now_ms) {
    (void)cooldown_until_ms;  // used by caller in maybe_poll_bind
    (void)now_ms;

    char btn_label[64];
    if (bind_target == target) {
        std::snprintf(btn_label, sizeof(btn_label), "%s: Press...", label);
    } else {
        std::snprintf(btn_label, sizeof(btn_label), "%s: %s",
                      label, current.label().c_str());
    }
    return ImGui::Button(btn_label, ImVec2(kBindButtonW, 0));
}

// Render a row of 3 bind buttons (used by the Buttons card).
void draw_button_row(const char* l1, cm::BindingTarget t1,
                     const char* l2, cm::BindingTarget t2,
                     const char* l3, cm::BindingTarget t3,
                     const cm::ControllerMapping& m,
                     cm::BindingTarget& bind_target,
                     std::int64_t& cooldown_until_ms,
                     std::int64_t now_ms) {
    if (draw_bind_button(l1, t1, *m.binding(t1), bind_target,
                         cooldown_until_ms, now_ms)) {
        bind_target = t1;
        cooldown_until_ms = now_ms + 250;
    }
    ImGui::SameLine(0, 8);
    if (draw_bind_button(l2, t2, *m.binding(t2), bind_target,
                         cooldown_until_ms, now_ms)) {
        bind_target = t2;
        cooldown_until_ms = now_ms + 250;
    }
    ImGui::SameLine(0, 8);
    if (draw_bind_button(l3, t3, *m.binding(t3), bind_target,
                         cooldown_until_ms, now_ms)) {
        bind_target = t3;
        cooldown_until_ms = now_ms + 250;
    }
}

} // namespace

std::vector<DeviceEntry> build_device_list() {
    std::vector<DeviceEntry> devices;
    devices.reserve(16);

    // Always Keyboard first.
    devices.push_back({"Keyboard", -1});

    const int n_joy = SDL_NumJoysticks();
    const int max_joy = std::min(n_joy, 15);  // 15 joysticks + 1 keyboard = 16 total
    for (int j = 0; j < max_joy; ++j) {
        const char* name = SDL_JoystickNameForIndex(j);
        std::string display;
        if (name) {
            display = name;
            if (display.size() > 58) display.resize(58);
            display += "##";
            display += std::to_string(j);
        } else {
            display = "Unknown Joystick##" + std::to_string(j);
        }
        devices.push_back({display, j});
    }
    return devices;
}

bool bind_button(const char* label,
                 cm::BindingTarget target,
                 const cm::InputBinding& current,
                 cm::BindingTarget& bind_target,
                 std::int64_t& cooldown_until_ms,
                 std::int64_t now_ms) {
    return draw_bind_button(label, target, current, bind_target,
                            cooldown_until_ms, now_ms);
}

bool draw_player_panel(const char* name,
                       cm::ControllerMapping& mapping,
                       cm::BindingTarget& bind_target,
                       SDL_Joystick*& joy,
                       int& device_sel,
                       std::int64_t& cooldown_until_ms,
                       std::int64_t now_ms) {
    ImGui::PushID(name);

    // Poll for bind BEFORE drawing (so a detected input applies this frame).
    maybe_poll_bind(mapping, bind_target, joy, device_sel,
                    cooldown_until_ms, now_ms);

    bool changed = false;

    // ---- Header: player name + device combo ---------------------------
    ImGui::TextUnformatted(name);
    ImGui::SameLine(0, 16);

    auto devices = build_device_list();
    if (draw_device_combo("##device", device_sel, devices)) {
        select_device(device_sel, joy);
        mapping.device_index = device_sel - 1;
        changed = true;
    }

    // ---- Top row: FN1 / Start / FN2 (indented to center) --------------
    ImGui::Indent(200.0f);
    if (draw_bind_button("FN1", cm::BindingTarget::FN1,
                         mapping.fn1, bind_target,
                         cooldown_until_ms, now_ms)) {
        bind_target = cm::BindingTarget::FN1;
        cooldown_until_ms = now_ms + 250;
    }
    ImGui::SameLine(0, 8);
    if (draw_bind_button("Start", cm::BindingTarget::Start,
                         mapping.start, bind_target,
                         cooldown_until_ms, now_ms)) {
        bind_target = cm::BindingTarget::Start;
        cooldown_until_ms = now_ms + 250;
    }
    ImGui::SameLine(0, 8);
    if (draw_bind_button("FN2", cm::BindingTarget::FN2,
                         mapping.fn2, bind_target,
                         cooldown_until_ms, now_ms)) {
        bind_target = cm::BindingTarget::FN2;
        cooldown_until_ms = now_ms + 250;
    }
    ImGui::Unindent(200.0f);

    ImGui::Spacing();

    // ---- Card 1: Directions (D-pad layout) ----------------------------
    if (ut::beginCard("Directions", kDirectionsCardW, kCardH, false)) {
        ImGui::Indent(72.0f);
        if (draw_bind_button("Up", cm::BindingTarget::Up,
                             mapping.up, bind_target,
                             cooldown_until_ms, now_ms)) {
            bind_target = cm::BindingTarget::Up;
            cooldown_until_ms = now_ms + 250;
        }
        ImGui::Unindent(72.0f);

        if (draw_bind_button("Left", cm::BindingTarget::Left,
                             mapping.left, bind_target,
                             cooldown_until_ms, now_ms)) {
            bind_target = cm::BindingTarget::Left;
            cooldown_until_ms = now_ms + 250;
        }
        ImGui::SameLine(0, 8);
        ImGui::Dummy(ImVec2(kBindButtonW, 0));
        ImGui::SameLine(0, 8);
        if (draw_bind_button("Right", cm::BindingTarget::Right,
                             mapping.right, bind_target,
                             cooldown_until_ms, now_ms)) {
            bind_target = cm::BindingTarget::Right;
            cooldown_until_ms = now_ms + 250;
        }

        ImGui::Indent(72.0f);
        if (draw_bind_button("Down", cm::BindingTarget::Down,
                             mapping.down, bind_target,
                             cooldown_until_ms, now_ms)) {
            bind_target = cm::BindingTarget::Down;
            cooldown_until_ms = now_ms + 250;
        }
        ImGui::Unindent(72.0f);

        ut::endCard();
    }

    // ---- Card 2: Buttons (grid 2×3) -----------------------------------
    ImGui::SameLine(0, 8);
    if (ut::beginCard("Buttons", kButtonsCardW, kCardH, false)) {
        draw_button_row("A", cm::BindingTarget::A,
                        "B", cm::BindingTarget::B,
                        "C", cm::BindingTarget::C,
                        mapping, bind_target, cooldown_until_ms, now_ms);
        ImGui::Spacing();
        draw_button_row("D", cm::BindingTarget::D,
                        "E", cm::BindingTarget::E,
                        "A+B", cm::BindingTarget::AB,
                        mapping, bind_target, cooldown_until_ms, now_ms);
        ut::endCard();
    }

    // ---- Card 3: Options (SOCD + macro + deadzone + actions) ----------
    ImGui::SameLine(0, 8);
    if (ut::beginCard("Options", kOptionsCardW, kCardH, false)) {
        // SOCD radios.
        ImGui::TextUnformatted("SOCD:");
        ImGui::SameLine(0, 6);
        int socd = static_cast<int>(mapping.socd_mode);
        if (socd == 0) socd = 1;
        const int old_socd = socd;
        ImGui::SameLine(0, 6);
        if (ImGui::RadioButton("L+R", socd == 1)) socd = 1;
        ImGui::SameLine(0, 6);
        if (ImGui::RadioButton("U+D", socd == 2)) socd = 2;
        ImGui::SameLine(0, 6);
        if (ImGui::RadioButton("Both", socd == 3)) socd = 3;
        if (socd != old_socd) {
            mapping.socd_mode = static_cast<cm::SocdMode>(socd);
            changed = true;
        }

        // Macro + deadzone slider.
        const bool old_macro = mapping.air_dash_macro;
        ImGui::Checkbox("AD Macro (9AB)", &mapping.air_dash_macro);
        if (mapping.air_dash_macro != old_macro) changed = true;

        ImGui::SameLine(0, 12);
        ImGui::PushItemWidth(65.0f);
        float dz = static_cast<float>(mapping.deadzone) / 32767.0f;
        const float old_dz = dz;
        ImGui::SliderFloat("Deadzone", &dz, 0.0f, 1.0f, "%.2f");
        if (dz != old_dz) {
            mapping.deadzone = static_cast<std::uint32_t>(dz * 32767.0f);
            changed = true;
        }
        ImGui::PopItemWidth();

        // Default Bindings / Clear.
        if (ImGui::Button("Default Bindings", ImVec2(120, 0))) {
            const int saved_device = mapping.device_index;
            mapping = cm::ControllerMapping::default_xbox();
            mapping.device_index = saved_device;
            changed = true;
        }
        ImGui::SameLine(0, 8);
        if (ImGui::Button("Clear", ImVec2(50, 0))) {
            mapping = mapping.cleared_bindings();
            changed = true;
        }

        ut::endCard();
    }

    ImGui::PopID();
    return changed;
}

bool draw_list_panel(const char* name,
                     cm::ControllerMapping& mapping,
                     cm::BindingTarget& bind_target,
                     SDL_Joystick*& joy,
                     int& device_sel,
                     std::int64_t& cooldown_until_ms,
                     std::int64_t now_ms) {
    ImGui::PushID(("list_" + std::string(name)).c_str());

    maybe_poll_bind(mapping, bind_target, joy, device_sel,
                    cooldown_until_ms, now_ms);

    bool changed = false;

    // ---- Header: name + device combo ----------------------------------
    ImGui::TextUnformatted(name);
    ImGui::Spacing();

    auto devices = build_device_list();
    if (draw_device_combo("##device", device_sel, devices)) {
        select_device(device_sel, joy);
        mapping.device_index = device_sel - 1;
        changed = true;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- 13 rows ------------------------------------------------------
    struct Row {
        const char* label;
        cm::BindingTarget target;
    };
    const Row rows[] = {
        {"Up",    cm::BindingTarget::Up},
        {"Down",  cm::BindingTarget::Down},
        {"Left",  cm::BindingTarget::Left},
        {"Right", cm::BindingTarget::Right},
        {"A",     cm::BindingTarget::A},
        {"B",     cm::BindingTarget::B},
        {"C",     cm::BindingTarget::C},
        {"D",     cm::BindingTarget::D},
        {"E",     cm::BindingTarget::E},
        {"A+B",   cm::BindingTarget::AB},
        {"Start", cm::BindingTarget::Start},
        {"FN1",   cm::BindingTarget::FN1},
        {"FN2",   cm::BindingTarget::FN2},
    };

    for (const auto& row : rows) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(row.label);
        ImGui::SameLine(90, 8);
        if (draw_bind_button(row.label, row.target,
                             *mapping.binding(row.target), bind_target,
                             cooldown_until_ms, now_ms)) {
            bind_target = row.target;
            cooldown_until_ms = now_ms + 250;
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- SOCD radios (different labels than grid view) ----------------
    ImGui::TextUnformatted("SOCD:");
    ImGui::SameLine(0, 8);
    int socd = static_cast<int>(mapping.socd_mode);
    if (socd == 0) socd = 1;
    const int old_socd = socd;
    ImGui::SameLine(0, 8);
    if (ImGui::RadioButton("L+R neg", socd == 1)) socd = 1;
    ImGui::SameLine(0, 8);
    if (ImGui::RadioButton("U+D neg", socd == 2)) socd = 2;
    ImGui::SameLine(0, 8);
    if (ImGui::RadioButton("Both neg", socd == 3)) socd = 3;
    if (socd != old_socd) {
        mapping.socd_mode = static_cast<cm::SocdMode>(socd);
        changed = true;
    }
    ImGui::Spacing();

    // ---- Air Dash Macro -----------------------------------------------
    const bool old_macro = mapping.air_dash_macro;
    ImGui::Checkbox("Air Dash Macro (9AB/7AB)", &mapping.air_dash_macro);
    if (mapping.air_dash_macro != old_macro) changed = true;
    ImGui::Spacing();

    // ---- Analog Deadzone slider ---------------------------------------
    ImGui::PushItemWidth(120.0f);
    float dz = static_cast<float>(mapping.deadzone) / 32767.0f;
    const float old_dz = dz;
    ImGui::SliderFloat("Analog Deadzone", &dz, 0.0f, 1.0f, "%.2f");
    if (dz != old_dz) {
        mapping.deadzone = static_cast<std::uint32_t>(dz * 32767.0f);
        changed = true;
    }
    ImGui::PopItemWidth();
    ImGui::Spacing();

    // ---- Default Bindings / Clear -------------------------------------
    if (ImGui::Button("Default Bindings", ImVec2(130, 0))) {
        const int saved_device = mapping.device_index;
        mapping = cm::ControllerMapping::default_xbox();
        mapping.device_index = saved_device;
        changed = true;
    }
    ImGui::SameLine(0, 8);
    if (ImGui::Button("Clear", ImVec2(60, 0))) {
        mapping = mapping.cleared_bindings();
        changed = true;
    }

    ImGui::PopID();
    return changed;
}

} // namespace caster::exe::pages::controller_helpers
