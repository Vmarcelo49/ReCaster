// src/exe/pages/controller_helpers.cpp

#include "controller_helpers.hpp"
#include "../../common/controller/binder.hpp"
#include "../../common/controller/mapping.hpp"
#include "../../common/logger.hpp"
#include "../../common/ui_theme.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_joystick.h>

#include <imgui.h>

#include <algorithm>  // std::min
#include <chrono>
#include <cstddef>  // std::size
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
    const bool listening = (bind_target == target);
    if (listening) {
        std::snprintf(btn_label, sizeof(btn_label), "...");
    } else {
        std::snprintf(btn_label, sizeof(btn_label), "%s",
                      current.label().c_str());
    }
    return ut::keybindButton(btn_label, listening, kBindButtonW);
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

    // Note: the caller (controllers_page) already draws the player label.
    // We don't draw `name` here to avoid duplicate labels.

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

    // Single-column list: all 13 bindings stacked vertically, label on the
    // left and keybind button on the right. Tight vertical spacing (2px
    // between rows) so the 13 bindings don't waste vertical space.
    float list_total = ImGui::GetContentRegionAvail().x;
    const float item_w = ImGui::CalcItemWidth();
    if (item_w > 0.0f && item_w < list_total) {
        list_total = item_w;
    }

    // Tighten item spacing for the binding rows only.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 2.0f));
    for (size_t i = 0; i < std::size(rows); ++i) {
        const auto& row = rows[i];
        ImGui::PushID(static_cast<int>(i));

        // Label on the left.
        ImGui::AlignTextToFramePadding();
        ut::pushStyleColor(ImGuiCol_Text, ut::active_theme().text_muted);
        if (ut::active_theme().font_body_sm()) ImGui::PushFont(ut::active_theme().font_body_sm());
        ImGui::TextUnformatted(row.label);
        if (ut::active_theme().font_body_sm()) ImGui::PopFont();
        ut::popStyleColor();

        // Keybind button on the right of this row.
        ImGui::SameLine(list_total - kBindButtonW);
        if (draw_bind_button(row.label, row.target,
                             *mapping.binding(row.target), bind_target,
                             cooldown_until_ms, now_ms)) {
            bind_target = row.target;
            cooldown_until_ms = now_ms + 250;
        }
        ImGui::PopID();
    }
    ImGui::PopStyleVar();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- Compact settings block --------------------------------------
    // Each setting on its own row: SOCD, Air Dash Macro, Deadzone.
    // Format: Label (left) : Widget (right), tight 2px vertical spacing.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 2.0f));

    const float settings_w = ImGui::GetContentRegionAvail().x;

    // Helper lambda for drawing a label on the left.
    auto draw_label = [](const char* text) {
        ImGui::AlignTextToFramePadding();
        ut::pushStyleColor(ImGuiCol_Text, ut::active_theme().text_muted);
        if (ut::active_theme().font_body_sm())
            ImGui::PushFont(ut::active_theme().font_body_sm());
        ImGui::TextUnformatted(text);
        if (ut::active_theme().font_body_sm())
            ImGui::PopFont();
        ut::popStyleColor();
    };

    // Row 1: SOCD dropdown.
    {
        const float combo_w = 140.0f;
        draw_label("SOCD");
        ImGui::SameLine(settings_w - combo_w);
        ImGui::PushItemWidth(combo_w);
        int socd = static_cast<int>(mapping.socd_mode);
        if (socd == 0) socd = 3;
        const int old_socd = socd;
        const char* socd_preview = (socd == 1) ? "L+R neutralize"
                                   : (socd == 2) ? "U+D neutralize"
                                   : "Both neutralize";
        if (ImGui::BeginCombo("##socd", socd_preview)) {
            if (ImGui::Selectable("L+R neutralize", socd == 1)) socd = 1;
            if (ImGui::Selectable("U+D neutralize", socd == 2)) socd = 2;
            if (ImGui::Selectable("Both neutralize", socd == 3)) socd = 3;
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        if (socd != old_socd) {
            mapping.socd_mode = static_cast<cm::SocdMode>(socd);
            changed = true;
        }
    }

    // Row 2: Air Dash Macro toggle.
    {
        draw_label("AIR DASH MACRO");
        ImGui::SameLine(settings_w - 38.0f);
        bool macro = mapping.air_dash_macro;
        if (ut::toggle(&macro, "##toggle_macro")) {
            mapping.air_dash_macro = macro;
            changed = true;
        }
    }

    // Air Dash Macro timing slider (only when macro is on).
    if (mapping.air_dash_macro) {
        const float slider_w = 140.0f;
        draw_label("JUMP FRAMES");
        ImGui::SameLine(settings_w - slider_w);
        ImGui::PushItemWidth(slider_w);
        int jump_f = mapping.air_dash_jump_frames;
        if (ImGui::SliderInt("##jump_frames", &jump_f, 1, 15, "%d")) {
            mapping.air_dash_jump_frames = static_cast<std::uint8_t>(jump_f);
            changed = true;
        }
        ImGui::PopItemWidth();
    }

    // Row 3: Analog deadzone slider (float 0..1, themed).
    {
        const float slider_w = 140.0f;
        draw_label("DEADZONE");
        ImGui::SameLine(settings_w - slider_w);
        ImGui::PushItemWidth(slider_w);
        float dz = static_cast<float>(mapping.deadzone) / 32767.0f;
        const float old_dz = dz;
        ut::themedSliderFloat("##deadzone", &dz, 0.0f, 1.0f, "%.2f");
        if (dz != old_dz) {
            mapping.deadzone = static_cast<std::uint32_t>(dz * 32767.0f);
            changed = true;
        }
        ImGui::PopItemWidth();
    }

    ImGui::PopStyleVar();  // ItemSpacing

    ImGui::Spacing();

    // ---- Action buttons (standardized width/height) -----
    {
        const float btn_w = 130.0f;
        const float btn_h = 32.0f;
        if (ut::actionButton("DEFAULT BINDINGS", btn_w, btn_h,
                              ut::ButtonVariant::Default)) {
            const int saved_device = mapping.device_index;
            mapping = cm::ControllerMapping::default_xbox();
            mapping.device_index = saved_device;
            changed = true;
        }
        ImGui::SameLine(0, 8);
        if (ut::actionButton("CLEAR", btn_w, btn_h,
                             ut::ButtonVariant::Default)) {
            mapping = mapping.cleared_bindings();
            changed = true;
        }
    }

    ImGui::PopID();
    return changed;
}

} // namespace caster::exe::pages::controller_helpers
