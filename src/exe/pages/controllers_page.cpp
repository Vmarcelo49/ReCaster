// src/exe/pages/controllers_page.cpp

#include "controllers_page.hpp"
#include "../../common/ui_theme.hpp"

#include <imgui.h>

namespace caster::exe::pages::controllers_page {

void draw() {
    namespace ut = caster::common::ui_theme;

    // Two cards side-by-side: Player 1 (left) and Player 2 (right).
    const float card_w = 460.0f;
    const float card_h = 480.0f;
    const float gap    = 16.0f;

    // ---- Player 1 -----------------------------------------------------
    if (ut::beginCard("P1", card_w, card_h, false)) {
        ut::cardTitle("PLAYER 1");
        ImGui::TextWrapped("Phase 6 will add the controller mapping UI:");
        ImGui::BulletText("Device combo (Keyboard + joysticks)");
        ImGui::BulletText("13 bind buttons: A/B/C/D/E/A+B/Start/FN1/FN2");
        ImGui::BulletText("Direction buttons: Up/Down/Left/Right");
        ImGui::BulletText("SOCD mode radio (4 modes)");
        ImGui::BulletText("Air Dash Macro checkbox");
        ImGui::BulletText("Analog deadzone slider");
        ImGui::BulletText("Default Bindings / Clear buttons");
        ImGui::Spacing();
        ImGui::TextDisabled("(disabled — Phase 6)");
        ut::endCard();
    }

    // ---- Player 2 -----------------------------------------------------
    ImGui::SameLine(0, gap);
    if (ut::beginCard("P2", card_w, card_h, false)) {
        ut::cardTitle("PLAYER 2");
        ImGui::TextWrapped("Same UI as Player 1, independent state.");
        ImGui::Spacing();
        ImGui::TextDisabled("(disabled — Phase 6)");
        ut::endCard();
    }
}

} // namespace caster::exe::pages::controllers_page
