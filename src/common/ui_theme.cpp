// src/common/ui_theme.cpp

#include "ui_theme.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace caster::common::ui_theme {

namespace {

// Internal style stack wrappers — only used by beginCard below.
void pushStyleVarFloat(ImGuiStyleVar idx, float v) {
    ImGui::PushStyleVar(idx, v);
}
void pushStyleVarVec2(ImGuiStyleVar idx, float x, float y) {
    ImGui::PushStyleVar(idx, ImVec2(x, y));
}
void popStyleVar(int count = 1) { ImGui::PopStyleVar(count); }

// Internal color conversion — used by drawGradientBackground and drawLogo.
std::uint32_t colorU32(Color col) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(col.x, col.y, col.z, col.w));
}

} // namespace

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

bool beginCard(const char* id, float w, float h, bool auto_resize_y,
               ImGuiWindowFlags flags) {
    pushStyleColor(ImGuiCol_ChildBg, COL_CARD);
    pushStyleColor(ImGuiCol_Border,  COL_CARD_BRD);
    pushStyleVarVec2(ImGuiStyleVar_WindowPadding, CARD_PAD, CARD_PAD);

    ImGuiChildFlags child_flags = ImGuiChildFlags_Borders |
                                  ImGuiChildFlags_AlwaysUseWindowPadding;
    if (auto_resize_y) child_flags |= ImGuiChildFlags_AutoResizeY;

    bool open = ImGui::BeginChild(id, ImVec2(w, h), child_flags, flags);

    // Pop before drawing card contents so styles don't bleed.
    popStyleVar(1);
    popStyleColor(2);

    return open;
}

void endCard() {
    ImGui::EndChild();
}

bool beginCenteredCard(const char* id, float w, float h,
                       bool auto_resize_y, ImGuiWindowFlags flags) {
    ImGui::SetCursorPos(ImVec2((WINDOW_W - w) / 2.0f, (WINDOW_H - h) / 2.0f));
    return beginCard(id, w, h, auto_resize_y, flags);
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
    const float sz = SIDEBAR_W - 2 * NAV_BUTTON_PAD;
    bool clicked = ImGui::Button(letter, ImVec2(sz, sz));
    popStyleColor(4);
    return clicked;
}

// Intentional no-op wrapper for call-site symmetry with secondaryButton.
bool primaryButton(const char* label, float w, float h) {
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
    constexpr float kLogoGap = 8.0f;
    dl->AddText(ImVec2(at_x, at_y), logo_col, logo_str);
    dl->AddText(ImVec2(at_x + logo_size.x + kLogoGap, at_y), caster_col, caster_str);
}

void drawErrorText(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    pushStyleColor(ImGuiCol_Text, COL_ERROR);
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
    popStyleColor(1);
}

void drawSuccessText(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    pushStyleColor(ImGuiCol_Text, COL_SUCCESS);
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
    popStyleColor(1);
}

void drawWarnText(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    pushStyleColor(ImGuiCol_Text, COL_WARN);
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
    popStyleColor(1);
}

void drawInfoText(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    pushStyleColor(ImGuiCol_Text, COL_INFO);
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
    popStyleColor(1);
}

} // namespace caster::common::ui_theme
