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
    std::snprintf(s.rollback_buf,  sizeof(s.rollback_buf),  "%d", cfg.default_rollback);

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

void maybe_save_rollback(cfg_ns::Config& cfg, State& s) {
    try {
        int v = std::stoi(s.rollback_buf);
        if (v >= 0 && v <= 20 && v != cfg.default_rollback) {
            cfg.default_rollback = v;
            cfg_ns::save(cfg);
            s.last_saved_field = "rollback";
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
        ImGui::SameLine(0, 8);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.9f, 0.4f, 1.0f));
        ImGui::TextUnformatted("Saved!");
        ImGui::PopStyleColor();
    }
}

} // namespace

void draw(cfg_ns::Config& cfg, State& s) {
    if (!s.initialized) {
        init_buffers(cfg, s);
    }

    const float card_w = -1.0f;  // fill available width

    // ---- PLAYER PROFILE ------------------------------------------------
    if (ut::beginCard("Profile", card_w, 0, /*auto_y=*/true)) {
        ut::cardTitle("PLAYER PROFILE");

        ImGui::TextDisabled("Display name (max 31 chars, shown to opponent)");
        ImGui::PushItemWidth(250);
        // InputText returns true when the text changes (on each keystroke).
        // We only save when the user presses Enter or the field loses focus.
        bool changed = ImGui::InputText("##name", s.name_buf, sizeof(s.name_buf),
                                         ImGuiInputTextFlags_EnterReturnsTrue);
        // Also detect on blur (deactivated).
        if (changed || (ImGui::IsItemDeactivatedAfterEdit())) {
            maybe_save_name(cfg, s);
        }
        ImGui::PopItemWidth();
        maybe_show_saved_feedback(s, "name");

        ut::endCard();
    }

    ImGui::Spacing();

    // ---- MATCH RULES ---------------------------------------------------
    if (ut::beginCard("Match", card_w, 0, /*auto_y=*/true)) {
        ut::cardTitle("MATCH RULES");

        ImGui::TextDisabled("Win count (best-of, e.g. 2 = first to 2 wins)");
        ImGui::PushItemWidth(80);
        bool wc_changed = ImGui::InputText("##wincount", s.wincount_buf,
                                            sizeof(s.wincount_buf),
                                            ImGuiInputTextFlags_CharsDecimal |
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        if (wc_changed || ImGui::IsItemDeactivatedAfterEdit()) {
            maybe_save_wincount(cfg, s);
        }
        ImGui::PopItemWidth();
        maybe_show_saved_feedback(s, "wincount");

        ImGui::Spacing();

        ImGui::TextDisabled("Default rollback frames (0..20)");
        ImGui::PushItemWidth(80);
        bool rb_changed = ImGui::InputText("##rollback", s.rollback_buf,
                                            sizeof(s.rollback_buf),
                                            ImGuiInputTextFlags_CharsDecimal |
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        if (rb_changed || ImGui::IsItemDeactivatedAfterEdit()) {
            maybe_save_rollback(cfg, s);
        }
        ImGui::PopItemWidth();
        maybe_show_saved_feedback(s, "rollback");

        ut::endCard();
    }

    ImGui::Spacing();

    // ---- NETWORK SETTINGS ----------------------------------------------
    if (ut::beginCard("Network", card_w, 0, /*auto_y=*/true)) {
        ut::cardTitle("NETWORK SETTINGS");

        ImGui::TextDisabled("Relay servers (one per line, format host:port).");
        ImGui::TextDisabled("Empty = use built-in defaults.");
        ImGui::PushItemWidth(-1);  // fill card width
        bool relay_changed = ImGui::InputTextMultiline("##relays", s.relay_buf,
                                                         sizeof(s.relay_buf),
                                                         ImVec2(0, 80),
                                                         ImGuiInputTextFlags_EnterReturnsTrue);
        if (relay_changed || ImGui::IsItemDeactivatedAfterEdit()) {
            maybe_save_relays(cfg, s);
        }
        ImGui::PopItemWidth();
        maybe_show_saved_feedback(s, "relays");

        ut::endCard();
    }

    ImGui::Spacing();

    // ---- OVERLAY SETTINGS -----------------------------------------------
    if (ut::beginCard("Overlay", card_w, 0, /*auto_y=*/true)) {
        ut::cardTitle("OVERLAY SETTINGS");

        // Playername overlay enabled toggle.
        bool pn_enabled = cfg.playername_enabled;
        if (ImGui::Checkbox("Show player names during netplay", &pn_enabled)) {
            cfg.playername_enabled = pn_enabled;
            cfg_ns::save(cfg);
            s.last_saved_field = "pn_enabled";
            s.saved_feedback_until_ms = now_ms() + 2000;
        }
        maybe_show_saved_feedback(s, "pn_enabled");

        ImGui::Spacing();

        // Playername position: Top / Bottom radio buttons.
        ImGui::TextDisabled("Player name position");
        bool pos_top = !cfg.playername_position_bottom;
        if (ImGui::RadioButton("Top", pos_top)) {
            cfg.playername_position_bottom = false;
            cfg_ns::save(cfg);
            s.last_saved_field = "pn_pos";
            s.saved_feedback_until_ms = now_ms() + 2000;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Bottom", !pos_top)) {
            cfg.playername_position_bottom = true;
            cfg_ns::save(cfg);
            s.last_saved_field = "pn_pos";
            s.saved_feedback_until_ms = now_ms() + 2000;
        }
        maybe_show_saved_feedback(s, "pn_pos");

        ut::endCard();
    }
}

} // namespace caster::exe::pages::config_page
