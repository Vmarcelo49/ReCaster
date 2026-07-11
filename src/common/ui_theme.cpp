// src/common/ui_theme.cpp

#include "ui_theme.hpp"

#include <cstdarg>
#include <cstdio>

namespace caster::common::ui_theme {

void applyModernTheme() {
    // Style vars (geometry) — 14 pushes
    pushStyleVarVec2(ImGuiStyleVar_WindowPadding,    0, 0);
    pushStyleVarFloat(ImGuiStyleVar_WindowRounding,  0.0f);
    pushStyleVarFloat(ImGuiStyleVar_WindowBorderSize,0.0f);
    pushStyleVarFloat(ImGuiStyleVar_ChildRounding,   CARD_ROUND);
    pushStyleVarFloat(ImGuiStyleVar_ChildBorderSize, 1.0f);
    pushStyleVarVec2(ImGuiStyleVar_FramePadding,     8, 5);
    pushStyleVarFloat(ImGuiStyleVar_FrameRounding,   6.0f);
    pushStyleVarFloat(ImGuiStyleVar_FrameBorderSize, 0.0f);
    pushStyleVarVec2(ImGuiStyleVar_ItemSpacing,      8, 8);
    pushStyleVarVec2(ImGuiStyleVar_ItemInnerSpacing, 6, 4);
    pushStyleVarFloat(ImGuiStyleVar_IndentSpacing,   16.0f);
    pushStyleVarFloat(ImGuiStyleVar_ScrollbarRounding, 8.0f);
    pushStyleVarFloat(ImGuiStyleVar_GrabRounding,    4.0f);
    pushStyleVarFloat(ImGuiStyleVar_TabRounding,     4.0f);

    // Style colors (palette) — 30 pushes
    pushStyleColor(ImGuiCol_WindowBg,             COL_TRANSPARENT);
    pushStyleColor(ImGuiCol_ChildBg,              COL_CARD);
    pushStyleColor(ImGuiCol_Border,               COL_CARD_BRD);
    pushStyleColor(ImGuiCol_Text,                 COL_TEXT);
    pushStyleColor(ImGuiCol_TextDisabled,         COL_MUTED);
    pushStyleColor(ImGuiCol_TextLink,             COL_RED);  // ImGui 1.90+ has TextLink; if older, falls back to a no-op push.
    pushStyleColor(ImGuiCol_FrameBg,              COL_FRAME);
    pushStyleColor(ImGuiCol_FrameBgHovered,       COL_FRAME_HOV);
    pushStyleColor(ImGuiCol_FrameBgActive,        COL_FRAME_ACT);
    pushStyleColor(ImGuiCol_Button,               COL_RED);
    pushStyleColor(ImGuiCol_ButtonHovered,        COL_RED_HOV);
    pushStyleColor(ImGuiCol_ButtonActive,         COL_RED_ACT);
    pushStyleColor(ImGuiCol_Header,               COL_NAV_ACTIVE);
    pushStyleColor(ImGuiCol_HeaderHovered,        COL_NAV_HOV);
    pushStyleColor(ImGuiCol_HeaderActive,         COL_NAV_ACTIVE);
    pushStyleColor(ImGuiCol_CheckMark,            COL_RED);
    pushStyleColor(ImGuiCol_Separator,            COL_CARD_BRD);
    pushStyleColor(ImGuiCol_SeparatorHovered,     COL_RED_DIM);
    pushStyleColor(ImGuiCol_SeparatorActive,      COL_RED);
    pushStyleColor(ImGuiCol_ScrollbarBg,          COL_TRANSPARENT);
    pushStyleColor(ImGuiCol_ScrollbarGrab,        COL_FRAME);
    pushStyleColor(ImGuiCol_ScrollbarGrabHovered, COL_FRAME_HOV);
    pushStyleColor(ImGuiCol_ScrollbarGrabActive,  COL_FRAME_ACT);
    pushStyleColor(ImGuiCol_SliderGrab,           COL_RED);
    pushStyleColor(ImGuiCol_SliderGrabActive,     COL_RED_HOV);
    pushStyleColor(ImGuiCol_NavCursor,         COL_RED);
    pushStyleColor(ImGuiCol_Tab,                  COL_FRAME);
    pushStyleColor(ImGuiCol_TabHovered,           COL_RED_DIM);
    pushStyleColor(ImGuiCol_TabSelected,          COL_RED);
    pushStyleColor(ImGuiCol_TabSelectedOverline,  COL_RED_HOV);
}

void popModernTheme() {
    popStyleColor(kPushedStyleColors);
    popStyleVar(kPushedStyleVars);
}

void drawGradientBackground() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetWindowPos();
    float w = ImGui::GetWindowWidth();
    float h = ImGui::GetWindowHeight();
    const ImU32 top = colorU32(COL_BG_DARK);
    const ImU32 bot = colorU32(COL_BG_MID);
    dl->AddRectFilledMultiColor(
        ImVec2(pos.x, pos.y),
        ImVec2(pos.x + w, pos.y + h),
        top, top, bot, bot);
}

bool beginCard(const char* id, float w, float h, bool auto_resize_y) {
    return beginCardWithFlags(id, w, h, auto_resize_y, 0);
}

bool beginCardWithFlags(const char* id, float w, float h,
                        bool auto_resize_y, ImGuiWindowFlags flags) {
    pushStyleColor(ImGuiCol_ChildBg, COL_CARD);
    pushStyleColor(ImGuiCol_Border,  COL_CARD_BRD);
    pushStyleVarVec2(ImGuiStyleVar_WindowPadding, CARD_PAD, CARD_PAD);

    ImGuiChildFlags child_flags = ImGuiChildFlags_Borders |
                                  ImGuiChildFlags_AlwaysUseWindowPadding;
    if (auto_resize_y) child_flags |= ImGuiChildFlags_AutoResizeY;

    bool open = ImGui::BeginChild(id, ImVec2(w, h), child_flags, flags);

    // Pop the style stack NOW (matches zzcaster's `defer`) so the styles
    // don't bleed into the card's contents.
    popStyleVar(1);
    popStyleColor(2);

    return open;
}

void endCard() {
    ImGui::EndChild();
}

void cardTitle(const char* text) {
    pushStyleColor(ImGuiCol_Text, COL_MUTED);
    ImGui::TextUnformatted(text);
    popStyleColor(1);
    ImGui::Spacing();
}

bool navButton(const char* letter, bool active) {
    if (active) {
        pushStyleColor(ImGuiCol_Button,        COL_NAV_ACTIVE);
        pushStyleColor(ImGuiCol_ButtonHovered, COL_NAV_ACTIVE);
        pushStyleColor(ImGuiCol_ButtonActive,  COL_RED_DIM);
        pushStyleColor(ImGuiCol_Text,          COL_RED);
    } else {
        pushStyleColor(ImGuiCol_Button,        COL_NAV);
        pushStyleColor(ImGuiCol_ButtonHovered, COL_NAV_HOV);
        pushStyleColor(ImGuiCol_ButtonActive,  COL_NAV_HOV);
        pushStyleColor(ImGuiCol_Text,          COL_TEXT);
    }
    const float sz = SIDEBAR_W - 14.0f;
    bool clicked = ImGui::Button(letter, ImVec2(sz, sz));
    popStyleColor(4);
    return clicked;
}

bool primaryButton(const char* label, float w, float h) {
    // The default themed button is already red (COL_RED); no style override
    // needed. This wrapper exists so calls read consistently in pages.
    return ImGui::Button(label, ImVec2(w, h));
}

bool secondaryButton(const char* label, float w, float h) {
    pushStyleColor(ImGuiCol_Button,        COL_FRAME);
    pushStyleColor(ImGuiCol_ButtonHovered, COL_FRAME_HOV);
    pushStyleColor(ImGuiCol_ButtonActive,  COL_FRAME_ACT);
    pushStyleColor(ImGuiCol_Text,          COL_TEXT);
    bool clicked = ImGui::Button(label, ImVec2(w, h));
    popStyleColor(4);
    return clicked;
}

void drawLogo(float at_x, float at_y) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 logo_col    = colorU32(COL_TEXT);
    const ImU32 caster_col  = colorU32(COL_RED);
    const char* logo_str    = "RE";
    const char* caster_str  = "CASTER";
    ImVec2 logo_size = ImGui::CalcTextSize(logo_str);
    dl->AddText(ImVec2(at_x, at_y), logo_col, logo_str);
    dl->AddText(ImVec2(at_x + logo_size.x + 8.0f, at_y), caster_col, caster_str);
}

void textColored(Color col, const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ImGui::TextColored(ImVec4(col.x, col.y, col.z, col.w), "%s", buf);
}

void textWrapped(Color col, const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    pushStyleColor(ImGuiCol_Text, col);
    ImGui::TextWrapped("%s", buf);
    popStyleColor(1);
}

} // namespace caster::common::ui_theme
