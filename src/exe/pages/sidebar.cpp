// src/exe/pages/sidebar.cpp

#include "sidebar.hpp"
#include "../../common/ui_theme.hpp"

#include <imgui.h>

namespace caster::exe::pages::sidebar {

namespace {
constexpr int kWindowH = 768;
}

void draw(MenuPage& current_page, bool& quit_clicked) {
    namespace ut = caster::common::ui_theme;

    quit_clicked = false;

    // Sidebar child window: SIDEBAR_W wide, extends from HEADER_H to bottom.
    ImGui::SetCursorPos(ImVec2(0, ut::HEADER_H));
    ut::pushStyleColor(ImGuiCol_ChildBg, ut::COL_SIDEBAR);
    const float sidebar_h = kWindowH - ut::HEADER_H;
    ImGui::BeginChild("##sidebar", ImVec2(ut::SIDEBAR_W, sidebar_h),
                      ImGuiChildFlags_AlwaysUseWindowPadding);

    // Top group: 3 nav buttons stacked vertically.
    constexpr float button_pad = 7.0f;  // matches (SIDEBAR_W - button_size) / 2
    ImGui::SetCursorPosX(button_pad);
    ImGui::SetCursorPosY(8.0f);
    if (ut::navButton("P", current_page == MenuPage::Play)) {
        current_page = MenuPage::Play;
    }
    ImGui::SetCursorPosX(button_pad);
    if (ut::navButton("C", current_page == MenuPage::GameConfig)) {
        current_page = MenuPage::GameConfig;
    }
    ImGui::SetCursorPosX(button_pad);
    if (ut::navButton("M", current_page == MenuPage::Controllers)) {
        current_page = MenuPage::Controllers;
    }

    // Bottom group: Quit button pinned to the bottom of the sidebar.
    // We use a dummy spacer to push it down — ImGui doesn't have a
    // "bottom-anchor" layout primitive.
    const float button_size = ut::SIDEBAR_W - 14.0f;
    const float quit_y = sidebar_h - button_size - 8.0f;
    ImGui::SetCursorPosY(quit_y);
    ImGui::SetCursorPosX(button_pad);
    if (ut::navButton("Q", false)) {
        quit_clicked = true;
    }

    ImGui::EndChild();
    ut::popStyleColor();
}

} // namespace caster::exe::pages::sidebar
