// src/exe/pages/config_page.cpp

#include "config_page.hpp"
#include "../../common/config.hpp"
#include "../../common/ui_theme.hpp"

#include <imgui.h>

namespace caster::exe::pages::config_page {

void draw(const caster::common::config::Config& cfg) {
    namespace ut = caster::common::ui_theme;

    // Three cards stacked vertically: PROFILE / MATCH RULES / NETWORK.
    const float card_w = -1.0f;  // fill available width
    const float card_h = 0.0f;   // auto-resize Y

    // ---- PLAYER PROFILE -----------------------------------------------
    if (ut::beginCard("Profile", card_w, card_h, /*auto_resize_y=*/true)) {
        ut::cardTitle("PLAYER PROFILE");
        ImGui::TextDisabled("Display name: %s",
                            cfg.display_name.empty() ? "(not set)"
                                                     : cfg.display_name.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("Phase 7 will add an InputText + Apply button "
                           "here that updates config.display_name and "
                           "persists to caster/config.ini.");
        ut::endCard();
    }

    ImGui::Spacing();

    // ---- MATCH RULES --------------------------------------------------
    if (ut::beginCard("Match", card_w, card_h, /*auto_resize_y=*/true)) {
        ut::cardTitle("MATCH RULES");
        ImGui::BulletText("Win count: %d", cfg.versus_win_count);
        ImGui::BulletText("Default rollback: %d frames", cfg.default_rollback);
        ImGui::BulletText("Max real delay: %d frames", cfg.max_real_delay);
        ImGui::BulletText("High CPU priority: %s",
                          cfg.high_cpu_priority ? "yes" : "no");
        ImGui::Spacing();
        ImGui::TextWrapped("Phase 7 will add InputText + Apply for win count "
                           "and rollback.");
        ut::endCard();
    }

    ImGui::Spacing();

    // ---- NETWORK SETTINGS ---------------------------------------------
    if (ut::beginCard("Network", card_w, card_h, /*auto_resize_y=*/true)) {
        ut::cardTitle("NETWORK SETTINGS");
        if (cfg.relay_servers.empty()) {
            ImGui::TextDisabled("Relay servers: (using built-in defaults)");
        } else {
            ImGui::TextDisabled("Relay servers (%zu):",
                                cfg.relay_servers.size());
            for (const auto& r : cfg.relay_servers) {
                ImGui::BulletText("%s", r.c_str());
            }
        }
        ImGui::Spacing();
        ImGui::TextWrapped("Phase 7 will add an editable multi-line textbox "
                           "for the relay list.");
        ut::endCard();
    }
}

} // namespace caster::exe::pages::config_page
