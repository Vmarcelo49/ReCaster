// src/exe/pages/header.cpp

#include "header.hpp"
#include "../../common/config.hpp"
#include "../../common/ui_theme.hpp"

#include <imgui.h>

namespace caster::exe::pages::header {

namespace {
constexpr int kWindowW = 1024;
}

void draw() {
    namespace ut = caster::common::ui_theme;

    // Header child window: 1024 wide, HEADER_H tall, at (0, 0).
    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_ChildBg,
        ImVec4(ut::COL_HEADER_BAR.x, ut::COL_HEADER_BAR.y,
               ut::COL_HEADER_BAR.z, ut::COL_HEADER_BAR.w));
    ImGui::BeginChild("##header", ImVec2(kWindowW, ut::HEADER_H),
                      ImGuiChildFlags_AlwaysUseWindowPadding);

    // "ZZ CASTER" logo at the left, vertically centered.
    const float logo_y = (ut::HEADER_H - ImGui::CalcTextSize("ZZ").y) / 2.0f;
    ut::drawLogo(20.0f, logo_y);

    // Version string at the right edge, vertically centered.
    const char* ver = caster::common::config::kVersionString;
    const ImVec2 ver_size = ImGui::CalcTextSize(ver);
    ImGui::SetCursorPos(ImVec2(kWindowW - ver_size.x - 20.0f,
                               (ut::HEADER_H - ver_size.y) / 2.0f));
    ImGui::TextDisabled("%s", ver);

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

} // namespace caster::exe::pages::header
