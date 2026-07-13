// src/exe/pages/header.cpp

#include "header.hpp"
#include "../../common/ui_theme.hpp"

#include <imgui.h>

#include <cstddef>    // std::size

namespace caster::exe::pages::header {

namespace ut = caster::common::ui_theme;

void draw(MenuPage& current_page) {
    const ut::Theme& t = ut::active_theme();
    const float win_w = ut::window_width();

    // Position the header at the top-left, full width × HEADER_H tall.
    ImGui::SetCursorPos(ImVec2(0, 0));
    ut::pushStyleColor(ImGuiCol_ChildBg, t.bg_top);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36, 0));  // 36px horizontal padding
    ImGui::BeginChild("##header", ImVec2(win_w, ut::HEADER_H),
                      ImGuiChildFlags_AlwaysUseWindowPadding |
                      ImGuiChildFlags_Borders);  // bottom border via Border color

    // Layout: nav-tabs on the LEFT, brand block on the RIGHT.
    // We use a 2-column flex-like layout via SameLine + cursor math.

    const float content_h = ut::HEADER_H;

    // ---- Nav-tabs (left side) ---------------------------------------
    // Stack 3 nav-tabs horizontally. The active one gets the underline.
    {
        // Vertically center the tabs in the 72px header.
        const float tab_h = 36.0f;  // approx height of a tab button
        ImGui::SetCursorPosY((content_h - tab_h) * 0.5f);

        struct Tab { const char* label; MenuPage page; };
        const Tab tabs[] = {
            { "PLAY",       MenuPage::Play        },
            { "CONFIG",     MenuPage::GameConfig  },
            { "CONTROLLER", MenuPage::Controllers },
        };
        for (size_t i = 0; i < std::size(tabs); ++i) {
            if (i > 0) ImGui::SameLine(0, 4.0f);
            if (ut::navTab(tabs[i].label, current_page == tabs[i].page)) {
                current_page = tabs[i].page;
            }
        }
    }

    // ---- Brand block (right side) -----------------------------------
    // Right-align "RE CASTER" and vertically center it in the header.
    {
        ImFont* name_font = t.font_body() ? t.font_body() : ImGui::GetFont();

        const char* name = "RE CASTER";
        ImGui::PushFont(name_font);
        const ImVec2 name_size = ImGui::CalcTextSize(name);
        ImGui::PopFont();

        const float brand_w = name_size.x;
        const float brand_h = name_size.y;

        // Right-align relative to the actual window width.
        const float brand_x = win_w - 36.0f - brand_w;
        const float brand_y = (content_h - brand_h) * 0.5f;
        ut::drawBrand(brand_x, brand_y);
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(1);
    ut::popStyleColor(1);

    // Draw a 1px bottom border under the header (matches HTML border-bottom).
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 root_pos = ImGui::GetWindowPos();
        const float y = root_pos.y + ut::HEADER_H;
        const ImU32 border_col = ImGui::ColorConvertFloat4ToU32(
            ImVec4(t.rule.x, t.rule.y, t.rule.z, t.rule.w));
        dl->AddLine(ImVec2(root_pos.x, y),
                    ImVec2(root_pos.x + win_w, y),
                    border_col, 1.0f);
    }
}

} // namespace caster::exe::pages::header
