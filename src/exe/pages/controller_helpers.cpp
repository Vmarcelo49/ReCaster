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

constexpr float kBindButtonW = 90.0f;

void select_device(int device_sel, SDL_Joystick*& joy) {
    if (joy) {
        SDL_JoystickClose(joy);
        joy = nullptr;
    }
    if (device_sel > 0) {
        joy = SDL_JoystickOpen(device_sel - 1);
        if (!joy) {
            caster::common::logger::warn("controller: SDL_JoystickOpen({}) failed: {}",
                                         device_sel - 1, SDL_GetError());
        }
    }
}

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

bool maybe_poll_bind(cm::ControllerMapping& mapping,
                     cm::BindingTarget& bind_target,
                     SDL_Joystick* joy,
                     int device_sel,
                     std::int64_t& cooldown_until_ms,
                     std::int64_t now_ms) {
    if (bind_target == cm::BindingTarget::None) return false;
    if (cooldown_until_ms != 0 && now_ms < cooldown_until_ms) return false;
    if (cooldown_until_ms != 0 && now_ms >= cooldown_until_ms) {
        cooldown_until_ms = 0;
    }
    const int device_idx = device_sel - 1;
    cm::InputBinding b = cm::poll_for_bind_input(joy, device_idx);
    if (b.type != cm::InputType::None) {
        if (cm::InputBinding* slot = mapping.binding(bind_target)) {
            *slot = b;
        }
        bind_target = cm::BindingTarget::None;
        return true;  // binding changed — caller should save
    }
    return false;
}

bool draw_bind_button(const char* label,
                      cm::BindingTarget target,
                      const cm::InputBinding& current,
                      cm::BindingTarget& bind_target,
                      std::int64_t& cooldown_until_ms,
                      std::int64_t now_ms) {
    (void)cooldown_until_ms;
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

// Helper to handle a bind button click.
// MEMBER is the ControllerMapping member (e.g. a, b, up, left)
// TARGET is the BindingTarget enum value (e.g. cm::BindingTarget::A)
#define HANDLE_BIND(LABEL, MEMBER, TARGET, BIND_TARGET, COOLDOWN, NOW) \
    if (draw_bind_button(LABEL, TARGET, mapping.MEMBER, BIND_TARGET, COOLDOWN, NOW)) { \
        BIND_TARGET = TARGET; \
        COOLDOWN = NOW + 250; \
    }

} // namespace

std::vector<DeviceEntry> build_device_list() {
    std::vector<DeviceEntry> devices;
    devices.reserve(16);
    devices.push_back({"Keyboard", -1});
    const int n_joy = SDL_NumJoysticks();
    const int max_joy = std::min(n_joy, 15);
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

// ---- List view — already simple, no inner cards --------------------------

bool draw_list_panel(const char* name,
                     cm::ControllerMapping& mapping,
                     cm::BindingTarget& bind_target,
                     SDL_Joystick*& joy,
                     int& device_sel,
                     std::int64_t& cooldown_until_ms,
                     std::int64_t now_ms) {
    ImGui::PushID(("list_" + std::string(name)).c_str());

    bool changed = false;

    ImGui::TextUnformatted(name);
    ImGui::Spacing();

    auto devices = build_device_list();
    if (draw_device_combo("##device", device_sel, devices)) {
        select_device(device_sel, joy);
        mapping = mapping.cleared_bindings();
        mapping.device_index = device_sel - 1;
        changed = true;
    }

    // Poll for bind input (called AFTER device combo so device_sel is current).
    if (maybe_poll_bind(mapping, bind_target, joy, device_sel,
                        cooldown_until_ms, now_ms)) {
        changed = true;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

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

    // SOCD
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

    const bool old_macro = mapping.air_dash_macro;
    ImGui::Checkbox("Air Dash Macro (9AB/7AB)", &mapping.air_dash_macro);
    if (mapping.air_dash_macro != old_macro) changed = true;
    ImGui::Spacing();

    // Air Dash Macro timing controls — only shown when the macro is on.
    // The macro emits jump_dir for `jump_frames` frames then 6|AB for 1
    // frame. If 9AB is held, it retriggers immediately (no lockout).
    if (mapping.air_dash_macro) {
        ImGui::Indent(16.0f);
        ImGui::PushItemWidth(120.0f);

        // Jump frames: 1..15 (must be at least 1)
        int jump_f = mapping.air_dash_jump_frames;
        if (ImGui::SliderInt("Jump frames (9/7)", &jump_f, 1, 15, "%d")) {
            mapping.air_dash_jump_frames = static_cast<std::uint8_t>(jump_f);
            changed = true;
        }

        ImGui::PopItemWidth();
        ImGui::Unindent(16.0f);
        ImGui::Spacing();
    }

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
