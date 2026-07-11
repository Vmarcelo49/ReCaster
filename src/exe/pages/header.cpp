// src/exe/pages/header.cpp

#include "header.hpp"
#include "../../common/config.hpp"
#include "../../common/ui_theme.hpp"

#include <imgui.h>

namespace caster::exe::pages::header {

void draw() {
    namespace ut = caster::common::ui_theme;

    ImGui::SetCursorPos(ImVec2(0, 0));
    ut::pushStyleColor(ImGuiCol_ChildBg, ut::COL_HEADER_BAR);
    ImGui::BeginChild("##header", ImVec2(ut::WINDOW_W, ut::HEADER_H),
                      ImGuiChildFlags_AlwaysUseWindowPadding);

    // "RE CASTER" logo at the left, vertically centered.
    const float logo_y = (ut::HEADER_H - ImGui::CalcTextSize("RE").y) / 2.0f;
    ut::drawLogo(20.0f, logo_y);

    // Version string at the right edge, vertically centered.
    const char* ver = caster::common::config::kVersionString;
    const ImVec2 ver_size = ImGui::CalcTextSize(ver);
    ImGui::SetCursorPos(ImVec2(ut::WINDOW_W - ver_size.x - 20.0f,
                               (ut::HEADER_H - ver_size.y) / 2.0f));
    ImGui::TextDisabled("%s", ver);

    ImGui::EndChild();
    ut::popStyleColor();
}

} // namespace caster::exe::pages::header
