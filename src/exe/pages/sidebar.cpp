// src/exe/pages/sidebar.cpp

#include "sidebar.hpp"
#include "../../common/ui_theme.hpp"

#include <imgui.h>

namespace caster::exe::pages::sidebar {

void draw(MenuPage& current_page, bool& quit_clicked) {
    namespace ut = caster::common::ui_theme;

    quit_clicked = false;

    ImGui::SetCursorPos(ImVec2(0, ut::HEADER_H));
    ut::pushStyleColor(ImGuiCol_ChildBg, ut::COL_SIDEBAR);
    const float sidebar_h = ut::WINDOW_H - ut::HEADER_H;
    ImGui::BeginChild("##sidebar", ImVec2(ut::SIDEBAR_W, sidebar_h),
                      ImGuiChildFlags_AlwaysUseWindowPadding);

    // Top group: 3 nav buttons stacked vertically.
    ImGui::SetCursorPosX(ut::NAV_BUTTON_PAD);
    ImGui::SetCursorPosY(ut::CONTENT_PAD);
    if (ut::navButton("P", current_page == MenuPage::Play)) {
        current_page = MenuPage::Play;
    }
    ImGui::SetCursorPosX(ut::NAV_BUTTON_PAD);
    if (ut::navButton("C", current_page == MenuPage::GameConfig)) {
        current_page = MenuPage::GameConfig;
    }
    ImGui::SetCursorPosX(ut::NAV_BUTTON_PAD);
    if (ut::navButton("M", current_page == MenuPage::Controllers)) {
        current_page = MenuPage::Controllers;
    }

    // Bottom group: Quit button pinned to the bottom of the sidebar.
    const float button_size = ut::SIDEBAR_W - 2 * ut::NAV_BUTTON_PAD;
    const float quit_y = sidebar_h - button_size - ut::CONTENT_PAD;
    ImGui::SetCursorPosY(quit_y);
    ImGui::SetCursorPosX(ut::NAV_BUTTON_PAD);
    if (ut::navButton("Q", false)) {
        quit_clicked = true;
    }

    ImGui::EndChild();
    ut::popStyleColor();
}

} // namespace caster::exe::pages::sidebar
