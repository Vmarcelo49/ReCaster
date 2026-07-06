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

// Initialize input buffers from the loaded Config. Called once on first
// draw (or when the buffers haven't been initialized yet).
void init_buffers(const cfg_ns::Config& cfg, State& s) {
    std::snprintf(s.name_buf,      sizeof(s.name_buf),      "%s", cfg.display_name.c_str());
    std::snprintf(s.wincount_buf,  sizeof(s.wincount_buf),  "%d", cfg.versus_win_count);
    std::snprintf(s.rollback_buf,  sizeof(s.rollback_buf),  "%d", cfg.default_rollback);

    // Join relay_servers with newlines for the multi-line textbox.
    std::string relays;
    for (size_t i = 0; i < cfg.relay_servers.size(); ++i) {
        if (i) relays += '\n';
        relays += cfg.relay_servers[i];
    }
    std::snprintf(s.relay_buf, sizeof(s.relay_buf), "%s", relays.c_str());

    s.initialized = true;
}

// Show a green "Saved!" next to the most recently saved field for 2s.
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
        ImGui::InputText("##name", s.name_buf, sizeof(s.name_buf));
        ImGui::PopItemWidth();
        ImGui::SameLine(0, 8);
        if (ut::primaryButton("Apply", 80, 0)) {
            // Truncate to kMaxNameLen chars.
            size_t name_len = strnlen(s.name_buf, sizeof(s.name_buf));
            std::string name(s.name_buf, name_len);
            if (name.size() > cfg_ns::kMaxNameLen) {
                name.resize(cfg_ns::kMaxNameLen);
            }
            cfg.display_name = name;
            cfg_ns::save(cfg);
            s.last_saved_field = "name";
            s.saved_feedback_until_ms = now_ms() + 2000;
            caster::common::logger::info("config: display_name='{}'", name);
        }
        maybe_show_saved_feedback(s, "name");

        ut::endCard();
    }

    ImGui::Spacing();

    // ---- MATCH RULES ---------------------------------------------------
    if (ut::beginCard("Match", card_w, 0, /*auto_y=*/true)) {
        ut::cardTitle("MATCH RULES");

        ImGui::TextDisabled("Win count (best-of, e.g. 2 = first to 2 wins)");
        ImGui::PushItemWidth(80);
        ImGui::InputText("##wincount", s.wincount_buf, sizeof(s.wincount_buf),
                         ImGuiInputTextFlags_CharsDecimal);
        ImGui::PopItemWidth();
        ImGui::SameLine(0, 8);
        if (ut::primaryButton("Apply", 80, 0)) {
            try {
                int v = std::stoi(s.wincount_buf);
                if (v >= 1 && v <= 99) {
                    cfg.versus_win_count = v;
                    cfg_ns::save(cfg);
                    s.last_saved_field = "wincount";
                    s.saved_feedback_until_ms = now_ms() + 2000;
                    caster::common::logger::info("config: versus_win_count={}", v);
                }
            } catch (...) {
                // ignore parse errors
            }
        }
        maybe_show_saved_feedback(s, "wincount");

        ImGui::Spacing();

        ImGui::TextDisabled("Default rollback frames (0..20)");
        ImGui::PushItemWidth(80);
        ImGui::InputText("##rollback", s.rollback_buf, sizeof(s.rollback_buf),
                         ImGuiInputTextFlags_CharsDecimal);
        ImGui::PopItemWidth();
        ImGui::SameLine(0, 8);
        if (ut::primaryButton("Apply", 80, 0)) {
            try {
                int v = std::stoi(s.rollback_buf);
                if (v >= 0 && v <= 20) {
                    cfg.default_rollback = v;
                    cfg_ns::save(cfg);
                    s.last_saved_field = "rollback";
                    s.saved_feedback_until_ms = now_ms() + 2000;
                    caster::common::logger::info("config: default_rollback={}", v);
                }
            } catch (...) {
                // ignore parse errors
            }
        }
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
        ImGui::InputTextMultiline("##relays", s.relay_buf, sizeof(s.relay_buf),
                                  ImVec2(0, 80));
        ImGui::PopItemWidth();
        if (ut::primaryButton("Apply", 80, 0)) {
            // Split relay_buf by newlines into cfg.relay_servers.
            cfg.relay_servers.clear();
            std::string current;
            for (char c : std::string(s.relay_buf)) {
                if (c == '\n' || c == '\r') {
                    if (!current.empty()) {
                        cfg.relay_servers.push_back(current);
                        current.clear();
                    }
                } else {
                    current += c;
                }
            }
            if (!current.empty()) {
                cfg.relay_servers.push_back(current);
            }
            cfg_ns::save(cfg);
            s.last_saved_field = "relays";
            s.saved_feedback_until_ms = now_ms() + 2000;
            caster::common::logger::info("config: relay_servers ({} entries)",
                                         cfg.relay_servers.size());
        }
        maybe_show_saved_feedback(s, "relays");

        ut::endCard();
    }
}

} // namespace caster::exe::pages::config_page
