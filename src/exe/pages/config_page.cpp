// src/exe/pages/config_page.cpp

#include "config_page.hpp"
#include "../../common/config.hpp"
#include "../../common/logger.hpp"
#include "../../common/ui_theme.hpp"

#include <imgui.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace caster::exe::pages::config_page {

namespace {

namespace ut = caster::common::ui_theme;
namespace cfg_ns = caster::common::config;

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

void init_buffers(const cfg_ns::Config& cfg, State& s) {
    std::snprintf(s.name_buf,      sizeof(s.name_buf),      "%s", cfg.display_name.c_str());
    std::snprintf(s.wincount_buf,  sizeof(s.wincount_buf),  "%d", cfg.versus_win_count);

    std::string relays;
    for (size_t i = 0; i < cfg.relay_servers.size(); ++i) {
        if (i) relays += '\n';
        relays += cfg.relay_servers[i];
    }
    std::snprintf(s.relay_buf, sizeof(s.relay_buf), "%s", relays.c_str());

    s.initialized = true;
}

// Check if a buffer changed from the config value and auto-save if so.
void maybe_save_name(cfg_ns::Config& cfg, State& s) {
    std::string name(s.name_buf, strnlen(s.name_buf, sizeof(s.name_buf)));
    if (name.size() > cfg_ns::kMaxNameLen) name.resize(cfg_ns::kMaxNameLen);
    if (name != cfg.display_name) {
        cfg.display_name = name;
        cfg_ns::save(cfg);
        s.last_saved_field = "name";
        s.saved_feedback_until_ms = now_ms() + 2000;
    }
}

void maybe_save_wincount(cfg_ns::Config& cfg, State& s) {
    try {
        int v = std::stoi(s.wincount_buf);
        if (v >= 1 && v <= 99 && v != cfg.versus_win_count) {
            cfg.versus_win_count = v;
            cfg_ns::save(cfg);
            s.last_saved_field = "wincount";
            s.saved_feedback_until_ms = now_ms() + 2000;
        }
    } catch (...) {}
}

void maybe_save_relays(cfg_ns::Config& cfg, State& s) {
    // Parse relay_buf into a vector and compare.
    std::vector<std::string> new_relays;
    std::string current;
    for (char c : std::string(s.relay_buf)) {
        if (c == '\n' || c == '\r') {
            if (!current.empty()) {
                new_relays.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) new_relays.push_back(current);

    if (new_relays != cfg.relay_servers) {
        cfg.relay_servers = std::move(new_relays);
        cfg_ns::save(cfg);
        s.last_saved_field = "relays";
        s.saved_feedback_until_ms = now_ms() + 2000;
    }
}

void maybe_show_saved_feedback(State& s, const char* field) {
    const std::int64_t now = now_ms();
    if (s.last_saved_field == field && now < s.saved_feedback_until_ms) {
        const ut::Theme& t = ut::active_theme();
        ImGui::SameLine(0, 8);
        ut::pushStyleColor(ImGuiCol_Text, t.success);
        ImGui::TextUnformatted("Saved!");
        ut::popStyleColor();
    }
}

// Save the theme selection. The active theme is updated globally via
// ut::set_active_theme(), which propagates to ImGuiStyle immediately.
void maybe_save_theme(cfg_ns::Config& cfg, State& s, int new_theme) {
    if (new_theme != cfg.theme) {
        cfg.theme = new_theme;
        cfg_ns::save(cfg);
        ut::set_active_theme(ut::theme_id_from_int(new_theme));
        s.last_saved_field = "theme";
        s.saved_feedback_until_ms = now_ms() + 2000;
    }
}

// Save the rounded-corners toggle. Propagates to ImGuiStyle immediately
// via ut::set_rounded_corners().
void maybe_save_rounded_corners(cfg_ns::Config& cfg, State& s, bool enabled) {
    if (enabled != cfg.rounded_corners) {
        cfg.rounded_corners = enabled;
        cfg_ns::save(cfg);
        ut::set_rounded_corners(enabled);
        s.last_saved_field = "rounded";
        s.saved_feedback_until_ms = now_ms() + 2000;
    }
}

} // namespace

void draw(cfg_ns::Config& cfg, State& s) {
    const ut::Theme& t = ut::active_theme();

    if (!s.initialized) {
        init_buffers(cfg, s);
    }

    // ====================================================================
    // CONFIG PAGE LAYOUT — matches HTML .config-form
    //
    //   ┌─────────────────────────────────────┐
    //   │ THEME                               │
    //   │ [ RED ][ BLUE ][ ELEGANT SUMMER ]   │  ← segmented control
    //   │                                     │
    //   │ DISPLAY NAME                        │
    //   │ ┌─────────────────────────────────┐ │
    //   │ │ ...                             │ │
    //   │ └─────────────────────────────────┘ │
    //   │                                     │
    //   │ ──────────────────────────────────  │
    //   │ Show player names during netplay [●]│  ← inline toggle row
    //   │ ──────────────────────────────────  │
    //   │                                     │
    //   │ WIN COUNT                           │
    //   │ [2]                                  │
    //   │                                     │
    //   │ DEFAULT ROLLBACK                    │
    //   │ [4]                                  │
    //   │                                     │
    //   │ RELAY SERVERS                       │
    //   │ ┌─────────────────────────────────┐ │
    //   │ │ ...                             │ │
    //   │ └─────────────────────────────────┘ │
    //   └─────────────────────────────────────┘
    // ====================================================================

    // Center the form in the available content area with max-width 460px.
    const float form_w = 460.0f;
    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float left_pad = (avail_w > form_w) ? (avail_w - form_w) * 0.5f : 0.0f;

    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + left_pad);
    ImGui::PushItemWidth(form_w);

    // ── THEME selector ──────────────────────────────────────────────────
    ut::fieldLabel("THEME");

    {
        // Segmented control with 3 options. We need to mirror the active
        // theme int into a local variable that segmentedControl can write.
        const char* labels[] = { "RED", "BLUE", "ELEGANT SUMMER" };
        int active = cfg.theme;
        if (ut::segmentedControl(labels, 3, &active)) {
            maybe_save_theme(cfg, s, active);
        }
    }
    maybe_show_saved_feedback(s, "theme");

    ImGui::Spacing();
    ImGui::Spacing();

    // ── INLINE TOGGLE: Rounded corners ─────────────────────────────────
    // Same row style as the playername toggle below: label left, toggle
    // right, with rules above and below.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 rmin = ImGui::GetCursorScreenPos();
        ImVec2 rmax = ImVec2(rmin.x + form_w, rmin.y);
        dl->AddLine(rmin, rmax,
                    ImGui::ColorConvertFloat4ToU32(
                        ImVec4(t.rule.x, t.rule.y, t.rule.z, t.rule.w)),
                    1.0f);

        ImGui::Spacing();

        ut::pushStyleColor(ImGuiCol_Text, t.text);
        ImGui::TextUnformatted("Rounded corners");
        ut::popStyleColor();
        ImGui::SameLine(form_w - 38.0f);  // align toggle to right edge

        bool rounded = cfg.rounded_corners;
        if (ut::toggle(&rounded, "##toggle_rounded")) {
            maybe_save_rounded_corners(cfg, s, rounded);
        }
        maybe_show_saved_feedback(s, "rounded");

        ImGui::Spacing();

        ImVec2 rmin2 = ImGui::GetCursorScreenPos();
        ImVec2 rmax2 = ImVec2(rmin2.x + form_w, rmin2.y);
        dl->AddLine(rmin2, rmax2,
                    ImGui::ColorConvertFloat4ToU32(
                        ImVec4(t.rule.x, t.rule.y, t.rule.z, t.rule.w)),
                    1.0f);
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // ── DISPLAY NAME ────────────────────────────────────────────────────
    ut::fieldLabel("DISPLAY NAME");

    {
        bool changed = ImGui::InputText("##name", s.name_buf, sizeof(s.name_buf),
                                         ImGuiInputTextFlags_EnterReturnsTrue);
        if (changed || ImGui::IsItemDeactivatedAfterEdit()) {
            maybe_save_name(cfg, s);
        }
        maybe_show_saved_feedback(s, "name");
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // ── INLINE TOGGLE: Show player names during netplay ────────────────
    // Form row with label on the left, toggle on the right, with rules
    // above and below.
    {
        // Top rule.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 rmin = ImGui::GetCursorScreenPos();
        ImVec2 rmax = ImVec2(rmin.x + form_w, rmin.y);
        dl->AddLine(rmin, rmax,
                    ImGui::ColorConvertFloat4ToU32(
                        ImVec4(t.rule.x, t.rule.y, t.rule.z, t.rule.w)),
                    1.0f);

        ImGui::Spacing();

        // Inline row: label left, toggle right.
        ut::pushStyleColor(ImGuiCol_Text, t.text);
        ImGui::TextUnformatted("Show player names during netplay");
        ut::popStyleColor();
        ImGui::SameLine(form_w - 38.0f);  // align toggle to right edge

        bool pn_enabled = cfg.playername_enabled;
        if (ut::toggle(&pn_enabled, "##toggle_playername")) {
            cfg.playername_enabled = pn_enabled;
            cfg_ns::save(cfg);
            s.last_saved_field = "pn_enabled";
            s.saved_feedback_until_ms = now_ms() + 2000;
        }
        maybe_show_saved_feedback(s, "pn_enabled");

        ImGui::Spacing();

        // Bottom rule.
        ImVec2 rmin2 = ImGui::GetCursorScreenPos();
        ImVec2 rmax2 = ImVec2(rmin2.x + form_w, rmin2.y);
        dl->AddLine(rmin2, rmax2,
                    ImGui::ColorConvertFloat4ToU32(
                        ImVec4(t.rule.x, t.rule.y, t.rule.z, t.rule.w)),
                    1.0f);
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // ── WIN COUNT ───────────────────────────────────────────────────────
    ut::fieldLabel("WIN COUNT (BEST OF)");

    ImGui::PushItemWidth(80);
    {
        bool wc_changed = ImGui::InputText("##wincount", s.wincount_buf,
                                            sizeof(s.wincount_buf),
                                            ImGuiInputTextFlags_CharsDecimal |
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        if (wc_changed || ImGui::IsItemDeactivatedAfterEdit()) {
            maybe_save_wincount(cfg, s);
        }
    }
    ImGui::PopItemWidth();
    maybe_show_saved_feedback(s, "wincount");

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Spacing();

    // ── RELAY SERVERS ───────────────────────────────────────────────────
    ut::fieldLabel("RELAY SERVERS (ONE PER LINE)");

    {
        ImGui::PushItemWidth(form_w);
        bool relay_changed = ImGui::InputTextMultiline("##relays", s.relay_buf,
                                                         sizeof(s.relay_buf),
                                                         ImVec2(form_w, 80),
                                                         ImGuiInputTextFlags_EnterReturnsTrue);
        if (relay_changed || ImGui::IsItemDeactivatedAfterEdit()) {
            maybe_save_relays(cfg, s);
        }
        ImGui::PopItemWidth();
    }
    maybe_show_saved_feedback(s, "relays");

    ImGui::PopItemWidth();
}

} // namespace caster::exe::pages::config_page
