// src/exe/pages/play_page.cpp

#include "play_page.hpp"
#include "main_menu.hpp"
#include "../launcher/game_runner.hpp"
#include "../../common/config.hpp"
#include "../../common/logger.hpp"
#include "../../common/ui_theme.hpp"

#include <filesystem>
#include <imgui.h>

namespace fs = std::filesystem;

namespace caster::exe::pages::play_page {

namespace {

// Shared launch helper. Calls GameRunner::launch_offline and transitions
// to InGame on success, or to ErrorState on failure.
void do_launch(MainMenu* menu,
               const common::config::Config& cfg,
               bool training) {
    if (!menu) return;

    auto& runner = menu->game_runner();
    if (runner.is_running()) {
        menu->set_error("Game already running (PID " +
                        std::to_string(runner.pid()) + ")");
        return;
    }

    launcher::LaunchOfflineParams params;
    params.training = training;

    auto r = runner.launch_offline(cfg, params);
    if (r.success) {
        menu->transition_to(UiState::InGame);
    } else {
        common::logger::err("play_page: launch failed: {}", r.error_message);
        menu->set_error(r.error_message);
    }
}

} // namespace

void draw(const common::config::Config& cfg, MainMenu* menu) {
    namespace ut = caster::common::ui_theme;

    // Two cards side-by-side: NETPLAY (left) and OFFLINE (right).
    const float card_w = 460.0f;
    const float card_h = 480.0f;
    const float gap    = 16.0f;

    // ---- NETPLAY card -------------------------------------------------
    ImGui::SetCursorPosX(0);
    if (ut::beginCard("Netplay", card_w, card_h, false)) {
        ut::cardTitle("NETPLAY");

        ImGui::TextWrapped("Phase 7+8 will add the unified input field here:");
        ImGui::BulletText("Port number           -> Host on that port");
        ImGui::BulletText("IP:Port               -> Direct join");
        ImGui::BulletText("#RoomCode (4 letters) -> Relay join");
        ImGui::Spacing();

        ImGui::TextDisabled("Buttons (Phase 7+8):");
        ImGui::BulletText("Host     - start smart host (direct + relay)");
        ImGui::BulletText("Join     - direct or relay, depending on input");
        ImGui::BulletText("Spectate - direct only (relay: Phase 9)");
        ImGui::Spacing();

        ImGui::TextDisabled("Default port: %d",
                            caster::common::config::kDefaultPort);
        ImGui::TextDisabled("Display name: %s",
                            cfg.display_name.empty() ? "(not set)"
                                                     : cfg.display_name.c_str());

        ut::endCard();
    }

    // ---- OFFLINE card -------------------------------------------------
    ImGui::SameLine(0, gap);
    if (ut::beginCard("Offline", card_w, card_h, false)) {
        ut::cardTitle("OFFLINE");

        // Disable launch buttons if a game is already running.
        const bool busy = menu && menu->game_runner().is_running();
        ImGui::BeginDisabled(busy);
        if (ut::primaryButton("Training", 280, 44)) {
            do_launch(menu, cfg, /*training=*/true);
        }
        ImGui::EndDisabled();
        ImGui::Spacing();
        ImGui::TextDisabled("Launches MBAA.exe in training mode "
                            "(hook.dll injected).");

        ImGui::Spacing();
        ImGui::Spacing();

        ImGui::BeginDisabled(busy);
        if (ut::primaryButton("Versus Mode", 280, 44)) {
            do_launch(menu, cfg, /*training=*/false);
        }
        ImGui::EndDisabled();
        ImGui::Spacing();
        ImGui::TextDisabled("Launches MBAA.exe in versus mode "
                            "(hook.dll injected).");

        if (busy) {
            ImGui::Spacing();
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                               "Game already running (PID %u) — use Force Kill first.",
                               menu ? menu->game_runner().pid() : 0u);
        }

        ut::endCard();
    }
}

} // namespace caster::exe::pages::play_page
