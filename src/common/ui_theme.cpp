// src/common/ui_theme.cpp

#include "ui_theme.hpp"
#include "font_registry.hpp"

#include <algorithm>  // std::max
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace caster::common::ui_theme {

// ---------------------------------------------------------------------------
// Theme definitions
// ---------------------------------------------------------------------------
namespace {

// Helper: pack an ImFont* or fall back to nullptr safely.
ImFont* font_or_null(ImFont* p) { return p; }

constexpr Theme kThemeDefault{
    ThemeId::Default,
    "Red",
    /*bg_top*/       { 0.00f, 0.00f, 0.00f, 1.00f },
    /*bg_bottom*/    { 0.165f, 0.031f, 0.031f, 1.00f },  // #2a0808
    /*bg_elevated*/  { 0.078f, 0.078f, 0.078f, 1.00f },  // #141414
    /*bg_card*/      { 0.102f, 0.102f, 0.102f, 1.00f },  // #1a1a1a
    /*rule*/         { 1.00f, 1.00f, 1.00f, 0.05f },
    /*rule_mid*/     { 1.00f, 1.00f, 1.00f, 0.10f },
    /*rule_strong*/  { 1.00f, 1.00f, 1.00f, 0.18f },
    /*text*/         { 0.878f, 0.878f, 0.878f, 1.00f },  // #e0e0e0
    /*text_muted*/   { 0.533f, 0.533f, 0.533f, 1.00f },  // #888888
    /*text_dim*/     { 0.333f, 0.333f, 0.333f, 1.00f },  // #555555
    /*accent*/       { 0.80f, 0.133f, 0.133f, 1.00f },   // #cc2222
    /*accent_soft*/  { 0.60f, 0.067f, 0.067f, 1.00f },   // #991111
    /*accent_2*/     { 0.80f, 0.133f, 0.133f, 1.00f },   // same as accent
    /*accent_bg*/    { 0.80f, 0.133f, 0.133f, 0.12f },
    /*error*/        { 1.00f, 0.40f, 0.40f, 1.00f },
    /*success*/      { 0.40f, 0.90f, 0.40f, 1.00f },
    /*warn*/         { 1.00f, 0.60f, 0.20f, 1.00f },
    /*info*/         { 1.00f, 0.85f, 0.20f, 1.00f },
    /*input_radius*/   0.0f,
    /*btn_radius*/     0.0f,
    /*toggle_radius*/  0.0f,
    /*slider_style*/   SliderStyle::Thick,
    /*toggle_style*/   ToggleStyle::Square,
    /*input_style*/    InputStyle::Box,
    /*button_style*/   ButtonStyle::Sharp,
};

constexpr Theme kThemeModern{
    ThemeId::Modern,
    "Blue",
    /*bg_top*/       { 0.016f, 0.024f, 0.039f, 1.00f },  // #04060a
    /*bg_bottom*/    { 0.110f, 0.157f, 0.216f, 1.00f },  // #1c2837
    /*bg_elevated*/  { 0.102f, 0.125f, 0.157f, 1.00f },  // #1a2028
    /*bg_card*/      { 0.102f, 0.125f, 0.157f, 1.00f },
    /*rule*/         { 1.00f, 1.00f, 1.00f, 0.06f },
    /*rule_mid*/     { 1.00f, 1.00f, 1.00f, 0.12f },
    /*rule_strong*/  { 1.00f, 1.00f, 1.00f, 0.20f },
    /*text*/         { 1.00f, 1.00f, 1.00f, 1.00f },
    /*text_muted*/   { 0.545f, 0.584f, 0.639f, 1.00f },  // #8b95a3
    /*text_dim*/     { 0.353f, 0.392f, 0.439f, 1.00f },  // #5a6470
    /*accent*/       { 0.00f, 0.831f, 1.00f, 1.00f },    // #00d4ff
    /*accent_soft*/  { 0.00f, 0.60f, 0.80f, 1.00f },     // #0099cc
    /*accent_2*/     { 0.486f, 0.361f, 1.00f, 1.00f },   // #7c5cff
    /*accent_bg*/    { 0.00f, 0.831f, 1.00f, 0.12f },
    /*error*/        { 1.00f, 0.40f, 0.40f, 1.00f },
    /*success*/      { 0.40f, 0.90f, 0.40f, 1.00f },
    /*warn*/         { 1.00f, 0.60f, 0.20f, 1.00f },
    /*info*/         { 1.00f, 0.85f, 0.20f, 1.00f },
    /*input_radius*/   6.0f,
    /*btn_radius*/     6.0f,
    /*toggle_radius*/  999.0f,
    /*slider_style*/   SliderStyle::Modern,
    /*toggle_style*/   ToggleStyle::Pill,
    /*input_style*/    InputStyle::Rounded,
    /*button_style*/   ButtonStyle::Rounded,
};

constexpr Theme kThemeElegant{
    ThemeId::Elegant,
    "Elegant Summer",
    /*bg_top*/       { 0.016f, 0.012f, 0.008f, 1.00f },  // #040302
    /*bg_bottom*/    { 0.122f, 0.094f, 0.063f, 1.00f },  // #1f1810
    /*bg_elevated*/  { 0.086f, 0.075f, 0.059f, 1.00f },  // #16130f
    /*bg_card*/      { 0.086f, 0.075f, 0.059f, 1.00f },
    /*rule*/         { 0.910f, 0.863f, 0.784f, 0.07f },  // warm white at 7%
    /*rule_mid*/     { 0.910f, 0.863f, 0.784f, 0.13f },
    /*rule_strong*/  { 0.910f, 0.863f, 0.784f, 0.22f },
    /*text*/         { 0.910f, 0.863f, 0.769f, 1.00f },  // #e8dcc4
    /*text_muted*/   { 0.659f, 0.620f, 0.541f, 1.00f },  // #a89e8a
    /*text_dim*/     { 0.416f, 0.376f, 0.310f, 1.00f },  // #6a604f
    /*accent*/       { 0.722f, 0.502f, 0.290f, 1.00f },  // #b8804a
    /*accent_soft*/  { 0.541f, 0.369f, 0.204f, 1.00f },  // #8a5e34
    /*accent_2*/     { 0.722f, 0.502f, 0.290f, 1.00f },
    /*accent_bg*/    { 0.722f, 0.502f, 0.290f, 0.10f },
    /*error*/        { 1.00f, 0.40f, 0.40f, 1.00f },
    /*success*/      { 0.40f, 0.90f, 0.40f, 1.00f },
    /*warn*/         { 1.00f, 0.60f, 0.20f, 1.00f },
    /*info*/         { 1.00f, 0.85f, 0.20f, 1.00f },
    /*input_radius*/   0.0f,
    /*btn_radius*/     2.0f,
    /*toggle_radius*/  999.0f,
    /*slider_style*/   SliderStyle::Thin,
    /*toggle_style*/   ToggleStyle::Pill,
    /*input_style*/    InputStyle::Underline,
    /*button_style*/   ButtonStyle::Flat,
};

ThemeId g_active_theme_id = ThemeId::Elegant;
bool g_rounded_corners = false;  // default: sharp corners

} // namespace

// ---------------------------------------------------------------------------
// Public API: theme management
// ---------------------------------------------------------------------------
ThemeId theme_id_from_int(int v) {
    if (v < 0 || v > 2) return ThemeId::Elegant;
    return static_cast<ThemeId>(v);
}

const Theme& active_theme() {
    switch (g_active_theme_id) {
        case ThemeId::Default: return kThemeDefault;
        case ThemeId::Modern:  return kThemeModern;
        case ThemeId::Elegant: return kThemeElegant;
    }
    return kThemeElegant;
}

ThemeId active_theme_id() { return g_active_theme_id; }

void set_active_theme(ThemeId id) {
    if (id == g_active_theme_id) return;
    g_active_theme_id = id;
    // Only apply to ImGuiStyle if a context exists — set_active_theme() may
    // be called from main() BEFORE GuiWindow creates the ImGui context.
    // GuiWindow's constructor calls apply_theme_to_imgui_style() after
    // CreateContext + font loading, which picks up g_active_theme_id.
    if (ImGui::GetCurrentContext() != nullptr) {
        apply_theme_to_imgui_style();
    }
}

void set_rounded_corners(bool enabled) {
    if (enabled == g_rounded_corners) return;
    g_rounded_corners = enabled;
    if (ImGui::GetCurrentContext() != nullptr) {
        apply_theme_to_imgui_style();
    }
}

bool rounded_corners_enabled() { return g_rounded_corners; }

// ---------------------------------------------------------------------------
// Window dimension helpers — query ImGui's main viewport so the layout
// adapts when the user resizes the SDL window.
// ---------------------------------------------------------------------------
float window_width() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (vp && vp->Size.x > 0.0f) return vp->Size.x;
    return WINDOW_W;  // fallback for first frame
}
float window_height() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    if (vp && vp->Size.y > 0.0f) return vp->Size.y;
    return WINDOW_H;
}

// ---------------------------------------------------------------------------
// Theme typography role mapping
// ---------------------------------------------------------------------------
ImFont* Theme::font_body() const {
    // All themes use Inter Regular for body text (and titles, since the
    // Fraunces display font was removed).
    return font_or_null(font_registry::body());
}
ImFont* Theme::font_body_sm() const {
    return font_or_null(font_registry::body_sm());
}
ImFont* Theme::font_body_lg() const {
    return font_or_null(font_registry::body_lg());
}
ImFont* Theme::font_mono() const {
    return font_or_null(font_registry::mono());
}

// ---------------------------------------------------------------------------
// apply_theme_to_imgui_style — push theme palette + geometry into the
// global ImGuiStyle so subsequent widgets pick it up.
// ---------------------------------------------------------------------------
namespace {

ImVec4 to_imvec4(const Color& c) { return ImVec4(c.x, c.y, c.z, c.w); }
ImU32  to_u32    (const Color& c) { return ImGui::ColorConvertFloat4ToU32(to_imvec4(c)); }

// Effective radii: when rounded_corners is on, use the theme's native
// radius but enforce a minimum of 6px so themes with native radius=0
// (Default/Elegant) still look rounded when the toggle is on.
// When off, force 0 (sharp) for all themes.
float eff_btn_radius() {
    if (!g_rounded_corners) return 0.0f;
    return std::max(active_theme().btn_radius, 6.0f);
}
float eff_input_radius() {
    if (!g_rounded_corners) return 0.0f;
    return std::max(active_theme().input_radius, 6.0f);
}

} // namespace

void apply_theme_to_imgui_style() {
    const Theme& t = active_theme();
    ImGuiStyle& style = ImGui::GetStyle();

    // When rounded_corners is off (the default), force all radii to 0 so
    // every theme looks sharp. When on, use the theme's native radii but
    // enforce a minimum of 6px so themes with native radius=0 (Default/
    // Elegant) still look rounded when the toggle is on.
    const float r       = g_rounded_corners ? std::max(t.btn_radius, 6.0f)     : 0.0f;
    const float ir      = g_rounded_corners ? std::max(t.input_radius, 6.0f)   : 0.0f;
    const float grab_r  = g_rounded_corners
                            ? (t.toggle_radius >= 999.0f ? 999.0f : std::max(t.btn_radius, 6.0f))
                            : 0.0f;

    // Geometry — driven by the active theme + rounded_corners override.
    style.WindowPadding    = ImVec2(0, 0);
    style.WindowRounding   = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildRounding    = r;
    style.ChildBorderSize  = 1.0f;
    style.FramePadding     = ImVec2(12, 8);
    style.FrameRounding    = ir;
    style.FrameBorderSize  = (t.input_style == InputStyle::Underline) ? 0.0f : 1.0f;
    style.ItemSpacing      = ImVec2(8, 8);
    style.ItemInnerSpacing = ImVec2(6, 4);
    style.IndentSpacing    = 16.0f;
    style.ScrollbarRounding = r;
    style.GrabRounding     = grab_r;
    style.TabRounding      = r;

    auto& cols = style.Colors;
    cols[ImGuiCol_WindowBg]             = to_imvec4(t.bg_top);  // we draw gradient on top
    cols[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);   // transparent by default
    cols[ImGuiCol_Border]               = to_imvec4(t.rule_strong);
    cols[ImGuiCol_Text]                 = to_imvec4(t.text);
    cols[ImGuiCol_TextDisabled]         = to_imvec4(t.text_muted);
    cols[ImGuiCol_TextLink]             = to_imvec4(t.accent);
    cols[ImGuiCol_FrameBg]              = to_imvec4(t.bg_elevated);
    cols[ImGuiCol_FrameBgHovered]       = to_imvec4(t.rule_mid);
    cols[ImGuiCol_FrameBgActive]        = to_imvec4(t.accent_bg);
    cols[ImGuiCol_Button]               = to_imvec4(t.bg_elevated);
    cols[ImGuiCol_ButtonHovered]        = to_imvec4(t.accent);
    cols[ImGuiCol_ButtonActive]         = to_imvec4(t.accent_soft);
    cols[ImGuiCol_Header]               = to_imvec4(t.accent_bg);
    cols[ImGuiCol_HeaderHovered]        = to_imvec4(t.rule_mid);
    cols[ImGuiCol_HeaderActive]         = to_imvec4(t.accent_bg);
    cols[ImGuiCol_CheckMark]            = to_imvec4(t.accent);
    cols[ImGuiCol_Separator]            = to_imvec4(t.rule);
    cols[ImGuiCol_SeparatorHovered]     = to_imvec4(t.rule_mid);
    cols[ImGuiCol_SeparatorActive]      = to_imvec4(t.accent);
    cols[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    cols[ImGuiCol_ScrollbarGrab]        = to_imvec4(t.rule_mid);
    cols[ImGuiCol_ScrollbarGrabHovered] = to_imvec4(t.rule_strong);
    cols[ImGuiCol_ScrollbarGrabActive]  = to_imvec4(t.accent);
    cols[ImGuiCol_SliderGrab]           = to_imvec4(t.accent);
    cols[ImGuiCol_SliderGrabActive]     = to_imvec4(t.accent_soft);
    cols[ImGuiCol_NavCursor]            = to_imvec4(t.accent);
    cols[ImGuiCol_Tab]                  = to_imvec4(t.bg_elevated);
    cols[ImGuiCol_TabHovered]           = to_imvec4(t.accent_bg);
    cols[ImGuiCol_TabSelected]          = to_imvec4(t.accent_bg);
    cols[ImGuiCol_TabSelectedOverline]  = to_imvec4(t.accent);
}

// ---------------------------------------------------------------------------
// drawGradientBackground
// ---------------------------------------------------------------------------
void drawGradientBackground() {
    const Theme& t = active_theme();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetWindowPos();
    float w = ImGui::GetWindowWidth();
    float h = ImGui::GetWindowHeight();
    const ImU32 top = to_u32(t.bg_top);
    const ImU32 bot = to_u32(t.bg_bottom);
    dl->AddRectFilledMultiColor(
        ImVec2(pos.x, pos.y),
        ImVec2(pos.x + w, pos.y + h),
        top, top, bot, bot);
}

// ---------------------------------------------------------------------------
// Card container (legacy compat)
// ---------------------------------------------------------------------------
bool beginCard(const char* id, float w, float h, bool auto_resize_y,
               ImGuiWindowFlags flags) {
    const Theme& t = active_theme();
    pushStyleColor(ImGuiCol_ChildBg, t.bg_card);
    pushStyleColor(ImGuiCol_Border,  t.rule_strong);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(CARD_PAD, CARD_PAD));

    ImGuiChildFlags child_flags = ImGuiChildFlags_Borders |
                                  ImGuiChildFlags_AlwaysUseWindowPadding;
    if (auto_resize_y) child_flags |= ImGuiChildFlags_AutoResizeY;

    bool open = ImGui::BeginChild(id, ImVec2(w, h), child_flags, flags);

    ImGui::PopStyleVar(1);
    ImGui::PopStyleColor(2);
    return open;
}

void endCard() { ImGui::EndChild(); }

bool beginCenteredCard(const char* id, float w, float h,
                       bool auto_resize_y, ImGuiWindowFlags flags) {
    // Center relative to the actual window dimensions (resizable).
    const float ww = window_width();
    const float wh = window_height();
    ImGui::SetCursorPos(ImVec2((ww - w) / 2.0f, (wh - h) / 2.0f));
    return beginCard(id, w, h, auto_resize_y, flags);
}

void cardTitle(const char* text) {
    const Theme& t = active_theme();
    pushStyleColor(ImGuiCol_Text, t.text_muted);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor(1);
    ImGui::Spacing();
}

// ---------------------------------------------------------------------------
// navTab — uppercase text button with underline indicator
// ---------------------------------------------------------------------------
bool navTab(const char* label, bool active) {
    const Theme& t = active_theme();

    if (t.font_body_sm()) ImGui::PushFont(t.font_body_sm());

    // Measure text. Add horizontal padding (18px each side, matching HTML).
    const float pad_x = 18.0f;
    const float pad_y = 10.0f;
    const ImVec2 text_size = ImGui::CalcTextSize(label);
    const float w = text_size.x + pad_x * 2;
    const float h = text_size.y + pad_y * 2;

    // Push transparent button (we draw the underline ourselves).
    pushStyleColor(ImGuiCol_Button,        COL_TRANSPARENT);
    pushStyleColor(ImGuiCol_ButtonHovered, COL_TRANSPARENT);
    pushStyleColor(ImGuiCol_ButtonActive,  COL_TRANSPARENT);
    pushStyleColor(ImGuiCol_Text, active ? t.text : t.text_muted);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(pad_x, pad_y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

    bool clicked = ImGui::Button(label, ImVec2(w, h));

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);

    // Draw the underline indicator. For active: full width, accent color.
    // For hover: animated grow (we approximate by drawing it whenever the
    // mouse is over the button).
    ImVec2 cursor = ImGui::GetItemRectMin();
    ImVec2 size   = ImGui::GetItemRectSize();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float underline_y = cursor.y + size.y - 4.0f;
    const float underline_x0 = cursor.x + pad_x;
    const float underline_x1 = cursor.x + size.x - pad_x;

    if (active) {
        dl->AddLine(ImVec2(underline_x0, underline_y),
                    ImVec2(underline_x1, underline_y),
                    to_u32(t.accent), 1.0f);
    } else if (ImGui::IsItemHovered()) {
        // Partial underline on hover (50% width, centered)
        const float mid = (underline_x0 + underline_x1) * 0.5f;
        const float half = (underline_x1 - underline_x0) * 0.25f;
        dl->AddLine(ImVec2(mid - half, underline_y),
                    ImVec2(mid + half, underline_y),
                    to_u32(t.accent), 1.0f);
    }

    if (t.font_body_sm()) ImGui::PopFont();
    return clicked;
}

// ---------------------------------------------------------------------------
// segmentedControl — N mutually-exclusive buttons
// ---------------------------------------------------------------------------
bool segmentedControl(const char* const* labels, int count, int* active_index) {
    if (count <= 0 || !active_index) return false;
    const Theme& t = active_theme();
    bool changed = false;

    if (t.font_body_sm()) ImGui::PushFont(t.font_body_sm());

    const float gap = 8.0f;
    const float total_gap = gap * (count - 1);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float btn_w = (avail - total_gap) / count;
    const float btn_h = 38.0f;

    for (int i = 0; i < count; ++i) {
        if (i > 0) ImGui::SameLine(0, gap);

        const bool is_active = (i == *active_index);

        // Style depends on active state.
        pushStyleColor(ImGuiCol_Text, is_active ? t.bg_top : t.text_muted);
        pushStyleColor(ImGuiCol_Button,        is_active ? t.accent : t.bg_elevated);
        pushStyleColor(ImGuiCol_ButtonHovered, is_active ? t.accent_soft : t.rule_mid);
        pushStyleColor(ImGuiCol_ButtonActive,  is_active ? t.accent_soft : t.rule_strong);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, eff_btn_radius());
        pushStyleColor(ImGuiCol_Border, t.rule_strong);

        if (ImGui::Button(labels[i], ImVec2(btn_w, btn_h))) {
            if (*active_index != i) {
                *active_index = i;
                changed = true;
            }
        }

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);
    }

    if (t.font_body_sm()) ImGui::PopFont();
    return changed;
}

// ---------------------------------------------------------------------------
// toggle — iOS-style pill toggle drawn via ImDrawList
// ---------------------------------------------------------------------------
bool toggle(bool* value, const char* id) {
    if (!value) return false;
    const Theme& t = active_theme();
    bool changed = false;

    const float w = 38.0f;
    const float h = 20.0f;
    const float radius = (t.toggle_radius >= 999.0f) ? (h * 0.5f) : 2.0f;

    // Invisible button for input hit-testing. Use the caller-provided id
    // so multiple toggles in the same window don't conflict.
    ImGui::InvisibleButton(id, ImVec2(w, h));
    if (ImGui::IsItemClicked()) {
        *value = !*value;
        changed = true;
    }
    const bool hovered = ImGui::IsItemHovered();

    // Draw the track.
    ImVec2 p0 = ImGui::GetItemRectMin();
    ImVec2 p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const Color track_bg     = *value ? t.accent : COL_TRANSPARENT;
    const Color track_border = *value ? t.accent : t.rule_strong;

    // Track background (filled when on, transparent when off).
    if (*value) {
        dl->AddRectFilled(p0, p1, to_u32(track_bg), radius);
    }
    // Track border (always).
    dl->AddRect(p0, p1, to_u32(track_border), radius, 0, 1.0f);

    // Knob.
    const float knob_size = 12.0f;
    const float knob_pad  = (h - knob_size) * 0.5f;
    const float knob_x = *value ? (p1.x - knob_pad - knob_size)
                                : (p0.x + knob_pad);
    const float knob_y = p0.y + knob_pad;
    const Color knob_color = *value ? t.bg_top : t.text_dim;
    dl->AddCircleFilled(ImVec2(knob_x + knob_size * 0.5f,
                               knob_y + knob_size * 0.5f),
                        knob_size * 0.5f, to_u32(knob_color));

    // Modern glow when on.
    if (*value && t.id == ThemeId::Modern) {
        // Subtle outer ring.
        dl->AddRect(p0, p1, to_u32(t.accent_bg), radius, 0, 3.0f);
    }

    return changed;
}

// ---------------------------------------------------------------------------
// themedSliderFloat — custom-drawn slider matching the HTML reference
// ---------------------------------------------------------------------------
bool themedSliderFloat(const char* label, float* v, float v_min, float v_max,
                       const char* format) {
    if (!v) return false;
    const Theme& t = active_theme();
    bool changed = false;

    // We use ImGui::SliderFloat under the hood for input handling, but
    // pre-push colors so the grab + track match the theme.
    const float track_h = (t.slider_style == SliderStyle::Thin) ? 1.0f : 4.0f;

    pushStyleColor(ImGuiCol_FrameBg,        t.rule_strong);
    pushStyleColor(ImGuiCol_FrameBgHovered, t.rule_strong);
    pushStyleColor(ImGuiCol_FrameBgActive,  t.rule_strong);
    pushStyleColor(ImGuiCol_SliderGrab,     t.accent);
    pushStyleColor(ImGuiCol_SliderGrabActive, t.accent_soft);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, (t.slider_style == SliderStyle::Thin) ? 0.0f : track_h * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, (8.0f - track_h) * 0.5f + 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

    if (t.font_mono()) ImGui::PushFont(t.font_mono());
    changed = ImGui::SliderFloat(label, v, v_min, v_max, format);
    if (t.font_mono()) ImGui::PopFont();

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
    return changed;
}

// ---------------------------------------------------------------------------
// keybindButton — mono-font button for controller bindings
// ---------------------------------------------------------------------------
bool keybindButton(const char* label, bool listening, float width) {
    const Theme& t = active_theme();
    const float h = 26.0f;

    if (t.font_mono()) ImGui::PushFont(t.font_mono());

    if (listening) {
        pushStyleColor(ImGuiCol_Button,        t.accent);
        pushStyleColor(ImGuiCol_ButtonHovered, t.accent);
        pushStyleColor(ImGuiCol_ButtonActive,  t.accent);
        pushStyleColor(ImGuiCol_Text,          t.bg_top);
        pushStyleColor(ImGuiCol_Border,        t.accent);
    } else {
        pushStyleColor(ImGuiCol_Button,        COL_TRANSPARENT);
        pushStyleColor(ImGuiCol_ButtonHovered, COL_TRANSPARENT);
        pushStyleColor(ImGuiCol_ButtonActive,  COL_TRANSPARENT);
        pushStyleColor(ImGuiCol_Text,          t.text);
        pushStyleColor(ImGuiCol_Border,        t.rule_strong);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, eff_btn_radius());
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 4));

    bool clicked = ImGui::Button(label, ImVec2(width, h));

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);

    if (t.font_mono()) ImGui::PopFont();
    return clicked;
}

// ---------------------------------------------------------------------------
// actionButton — themed button with three variants
// ---------------------------------------------------------------------------
bool actionButton(const char* label, float w, float h, ButtonVariant variant) {
    const Theme& t = active_theme();

    if (t.font_body_sm()) ImGui::PushFont(t.font_body_sm());

    switch (variant) {
        case ButtonVariant::Primary: {
            // Filled with accent, text is bg_top color.
            pushStyleColor(ImGuiCol_Text,          t.bg_top);
            pushStyleColor(ImGuiCol_Button,        t.accent);
            pushStyleColor(ImGuiCol_ButtonHovered, t.accent_soft);
            pushStyleColor(ImGuiCol_ButtonActive,  t.accent_soft);
            pushStyleColor(ImGuiCol_Border,        t.accent);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, eff_btn_radius());
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            bool clicked = ImGui::Button(label, ImVec2(w, h));
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(5);
            if (t.font_body_sm()) ImGui::PopFont();
            return clicked;
        }
        case ButtonVariant::Ghost: {
            // Transparent bg, text becomes accent on hover.
            pushStyleColor(ImGuiCol_Text,          t.text_muted);
            pushStyleColor(ImGuiCol_Button,        COL_TRANSPARENT);
            pushStyleColor(ImGuiCol_ButtonHovered, COL_TRANSPARENT);
            pushStyleColor(ImGuiCol_ButtonActive,  COL_TRANSPARENT);
            pushStyleColor(ImGuiCol_Border,        COL_TRANSPARENT);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, eff_btn_radius());
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            bool clicked = ImGui::Button(label, ImVec2(w, h));
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(5);
            if (t.font_body_sm()) ImGui::PopFont();
            return clicked;
        }
        case ButtonVariant::Default:
        default: {
            // Bordered box, hover fills with accent.
            pushStyleColor(ImGuiCol_Text,          t.text);
            pushStyleColor(ImGuiCol_Button,        t.bg_elevated);
            pushStyleColor(ImGuiCol_ButtonHovered, t.accent);
            pushStyleColor(ImGuiCol_ButtonActive,  t.accent_soft);
            pushStyleColor(ImGuiCol_Border,        t.rule_strong);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, eff_btn_radius());
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            bool clicked = ImGui::Button(label, ImVec2(w, h));
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(5);
            if (t.font_body_sm()) ImGui::PopFont();
            return clicked;
        }
    }
}

// ---------------------------------------------------------------------------
// fieldLabel — small uppercase muted label
// ---------------------------------------------------------------------------
void fieldLabel(const char* text) {
    const Theme& t = active_theme();
    if (t.font_body_sm()) ImGui::PushFont(t.font_body_sm());
    pushStyleColor(ImGuiCol_Text, t.text_dim);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor(1);
    if (t.font_body_sm()) ImGui::PopFont();
    ImGui::Spacing();
}

// ---------------------------------------------------------------------------
// verticalSeparator
// ---------------------------------------------------------------------------
void verticalSeparator(float x, float y0, float y1) {
    const Theme& t = active_theme();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), to_u32(t.rule), 1.0f);
}

// ---------------------------------------------------------------------------
// drawBrand — "RE CASTER" name only (kicker removed per user request)
// ---------------------------------------------------------------------------
void drawBrand(float at_x, float at_y) {
    const Theme& t = active_theme();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Name: body font, text color.
    if (t.font_body()) ImGui::PushFont(t.font_body());
    dl->AddText(ImVec2(at_x, at_y),
                to_u32(t.text), "RE CASTER");
    if (t.font_body()) ImGui::PopFont();
}

// ---------------------------------------------------------------------------
// Legacy nav button (sidebar) — kept for compat, no-op in new layout
// ---------------------------------------------------------------------------
bool navButton(const char* letter, bool active) {
    const Theme& t = active_theme();
    if (active) {
        pushStyleColor(ImGuiCol_Button,        COL_NAV_ACTIVE);
        pushStyleColor(ImGuiCol_ButtonHovered, COL_NAV_ACTIVE);
        pushStyleColor(ImGuiCol_ButtonActive,  COL_RED_DIM);
        pushStyleColor(ImGuiCol_Text,          t.accent);
    } else {
        pushStyleColor(ImGuiCol_Button,        COL_NAV);
        pushStyleColor(ImGuiCol_ButtonHovered, COL_NAV_HOV);
        pushStyleColor(ImGuiCol_ButtonActive,  COL_NAV_HOV);
        pushStyleColor(ImGuiCol_Text,          t.text);
    }
    const float sz = 42.0f;  // legacy size, unused in new layout
    bool clicked = ImGui::Button(letter, ImVec2(sz, sz));
    ImGui::PopStyleColor(4);
    return clicked;
}

// Legacy logo — redirects to drawBrand.
void drawLogo(float at_x, float at_y) {
    drawBrand(at_x, at_y);
}

// ---------------------------------------------------------------------------
// Colored text helpers
// ---------------------------------------------------------------------------
void drawErrorText(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    const Theme& t = active_theme();
    pushStyleColor(ImGuiCol_Text, t.error);
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor(1);
}

void drawSuccessText(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    const Theme& t = active_theme();
    pushStyleColor(ImGuiCol_Text, t.success);
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor(1);
}

void drawWarnText(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    const Theme& t = active_theme();
    pushStyleColor(ImGuiCol_Text, t.warn);
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor(1);
}

void drawInfoText(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    const Theme& t = active_theme();
    pushStyleColor(ImGuiCol_Text, t.info);
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(buf);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor(1);
}

} // namespace caster::common::ui_theme
