// src/dll/overlay/overlay_ui.cpp
//
// DX9 in-game overlay implementation. Ported from CCCaster:
//   - targets/DllOverlayUi.cpp     (lazy DX init + PresentFrameBegin facade)
//   - targets/DllOverlayUiText.cpp (state machine + 3-column text rendering)
//
// SKIPPED vs CCCaster:
//   - Trial mode (TrialManager::dtext, comboTrialText, comboName, etc.)
//     Trial is categoria D — fora de escopo v1.
//   - ImGui debug window (DllOverlayUiImGui.cpp). Can be added later.
//   - `Mode` enum (None/Trial/Mapping). Kept only the parts we use.
//   - `setTrial` / `setMapping` / `isTrial` / `isMapping` API.
//
// Kept faithful:
//   - State machine (Disabled -> Enabling -> Enabled -> Disabling) with
//     height animation via OVERLAY_CHANGE_DELTA.
//   - showMessage() with timeout in frames + background-window reset.
//   - 3-column text (left/center/right) drawn into a single full-width RECT.
//   - Translucent black background via a 4-vertex triangle strip + scale
//     transform.
//   - Selector rectangles (2, for future controller-mapping overlay).
//   - Lazy font + VB init on first Present call.
//
// Wiring:
//   - dll_main.cpp's PresentFrameBegin() calls overlay::presentFrameBegin().
//   - dll_main.cpp's InvalidateDeviceObjects() calls overlay::invalidateDeviceObjects().
//   - lifecycle.cpp calls overlay::init() after HookDirectX() succeeds, and
//     overlay::enable() to start in "ligado" state (per user choice).

#include "overlay_ui.hpp"
#include "primitives.hpp"
#include "game/addresses.hpp"
#include "util/algorithms.hpp"
#include "entry/lifecycle.hpp"          // for dll_hacks::windowHandle (foreground check)
#include "../common/logger.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace caster::dll::overlay {

// ----------------------------------------------------------------------------
// Constants (ported verbatim from DllOverlayUiText.cpp)
// ----------------------------------------------------------------------------

inline constexpr const char*  kOverlayFont          = "Tahoma";
inline constexpr int          kOverlayFontHeight    = 14;
inline constexpr int          kOverlayFontWidth     = 5;
inline constexpr int          kOverlayFontWeight    = 600;

inline constexpr D3DCOLOR kOverlayTextColor        = D3DCOLOR_XRGB(255, 255, 255);
inline constexpr D3DCOLOR kOverlayDebugColor       = D3DCOLOR_XRGB(255, 0,   0);
inline constexpr D3DCOLOR kOverlaySelectorLColor   = D3DCOLOR_XRGB(210, 0,   0);
inline constexpr D3DCOLOR kOverlaySelectorRColor   = D3DCOLOR_XRGB(30,  30,  255);
inline constexpr D3DCOLOR kOverlayBgColor          = D3DCOLOR_ARGB(220, 0,   0,   0);

inline constexpr int kOverlayTextBorder            = 10;
inline constexpr int kOverlaySelectorXBorder       = 5;
inline constexpr int kOverlaySelectorYBorder       = 1;

// Height-animation step. CCCaster defines this as a macro because it reads
// the file-scope `height`/`newHeight` at the call site; we keep it as a macro
// for the same reason (a lambda would capture by reference, adding indirection).
#define OVERLAY_CHANGE_DELTA() (4 + std::abs(g_height - g_newHeight) / 4)

// ----------------------------------------------------------------------------
// State (file-scope statics, matching CCCaster's static globals)
// ----------------------------------------------------------------------------

enum class State { Disabled, Disabling, Enabled, Enabling };

static State g_state = State::Disabled;

static int g_height = 0, g_oldHeight = 0, g_newHeight = 0;
static int g_initialTimeout = 0, g_messageTimeout = 0;

static std::array<std::string, 3> g_desiredText;   // what the caller wants (persists across state changes)
static std::array<std::string, 3> g_text;          // what's currently rendered (lags during state transitions)
static std::array<RECT, 2>        g_selector;
static std::array<bool, 2>        g_shouldDrawSelector { false, false };
static std::array<std::string, 2> g_selectorLine;

static ID3DXFont*                 g_font = nullptr;
static IDirect3DVertexBuffer9*    g_background = nullptr;

// Lazy-init flag — set by init(), consumed by presentFrameBegin().
static bool g_shouldInitDirectX = false;
static bool g_initializedDirectX = false;

// Vertex format for the background quad. XYZ + diffuse color, untransformed
// (we use a view matrix to scale/translate it into place).
struct Vertex {
    FLOAT  x, y, z;
    DWORD  color;
    static constexpr DWORD Format = D3DFVF_XYZ | D3DFVF_DIFFUSE;
};

// ----------------------------------------------------------------------------
// API: init / enable / disable / toggle / state queries
// ----------------------------------------------------------------------------

void init() {
    g_shouldInitDirectX = true;
}

void enable() {
    if (g_state != State::Enabled)
        g_state = State::Enabling;
}

void disable() {
    if (g_state != State::Disabled)
        g_state = State::Disabling;
}

void toggle() {
    if (isEnabled()) disable();
    else             enable();
}

bool isEnabled()  { return (g_state != State::Disabled) && (g_messageTimeout <= 0); }
bool isDisabled() { return g_state == State::Disabled; }
bool isToggling() { return g_state == State::Enabling || g_state == State::Disabling; }

std::array<std::string, 3> getText() { return g_desiredText; }

int getHeight()    { return g_height; }
int getNewHeight() { return g_newHeight; }

std::array<bool, 2> getShouldDrawSelector() { return g_shouldDrawSelector; }

// ----------------------------------------------------------------------------
// Text update / height animation
// ----------------------------------------------------------------------------

static int getTextHeight(const std::array<std::string, 3>& newText) {
    int h = 0;
    for (const std::string& s : newText) {
        const int lineCount = 1 + int(std::count(s.begin(), s.end(), '\n'));
        h = std::max(h, kOverlayFontHeight * lineCount);
    }
    return h;
}

void updateText() {
    updateText(g_desiredText);
}

void updateText(const std::array<std::string, 3>& newText) {
    // Always store what the caller wants — persists across state transitions
    // so that updateText() (no args) re-applies it. The state machine below
    // decides when g_text (what's rendered) catches up.
    g_desiredText = newText;

    switch (g_state) {
        case State::Disabled:
        default:
            g_height = g_oldHeight = g_newHeight = 0;
            g_text = { "", "", "" };
            return;

        case State::Disabling:
            g_newHeight = 0;
            if (g_height != g_newHeight) break;
            g_state = State::Disabled;
            g_oldHeight = 0;
            g_text = { "", "", "" };
            break;

        case State::Enabled:
            g_newHeight = getTextHeight(g_desiredText);
            if (g_newHeight > g_height) break;        // grow: wait for animation
            if (g_newHeight == g_height) g_oldHeight = g_height;
            g_text = g_desiredText;
            break;

        case State::Enabling:
            g_newHeight = getTextHeight(g_desiredText);
            if (g_height != g_newHeight) break;       // animate to target
            g_state = State::Enabled;
            g_oldHeight = g_height;
            g_text = g_desiredText;
            break;
    }

    if (g_height == g_newHeight) return;

    const int delta = OVERLAY_CHANGE_DELTA();
    if (g_newHeight > g_height)
        g_height = clamped(g_height + delta, g_height, g_newHeight);
    else
        g_height = clamped(g_height - delta, g_newHeight, g_height);
}

// ----------------------------------------------------------------------------
// Selector (kept for parity; unused for now)
// ----------------------------------------------------------------------------

void updateSelector(uint8_t index, int position, const std::string& line) {
    if (index > 1) return;
    g_selectorLine[index] = line;

    if (position == 0 || line.empty()) {
        g_shouldDrawSelector[index] = false;
        return;
    }

    RECT rect;
    rect.top = rect.left = 0;
    rect.right = 1;
    rect.bottom = kOverlayFontHeight;
    primitives::textCalcRect(g_font, line, rect, DT_CALCRECT, primitives::kColorBlack);

    rect.top    += kOverlayTextBorder + position * kOverlayFontHeight - kOverlaySelectorYBorder + 1;
    rect.bottom += kOverlayTextBorder + position * kOverlayFontHeight + kOverlaySelectorYBorder;

    if (index == 0) {
        rect.left  += kOverlayTextBorder - kOverlaySelectorXBorder;
        rect.right += kOverlayTextBorder + kOverlaySelectorXBorder;
    } else {
        const uint32_t screenW = *asU32(CC_SCREEN_WIDTH_ADDR);
        rect.left  = screenW - rect.right - kOverlayTextBorder - kOverlaySelectorXBorder;
        rect.right = screenW - kOverlayTextBorder + kOverlaySelectorXBorder;
    }

    g_selector[index] = rect;
    g_shouldDrawSelector[index] = true;
}

// ----------------------------------------------------------------------------
// Messages (temporary centered text)
// ----------------------------------------------------------------------------

void showMessage(const std::string& newText, int timeout) {
    // Timeout is in ms; convert to frames (~17ms each at 60fps).
    g_initialTimeout = g_messageTimeout = (timeout / 17);
    // Set both desired + rendered text so the message appears immediately
    // (bypassing the Enabling animation's text-lag).
    g_desiredText = { "", newText, "" };
    g_text = { "", newText, "" };
    g_shouldDrawSelector = { false, false };
    enable();
}

void updateMessage() {
    updateText(g_desiredText);

    if (g_messageTimeout == 1) {
        if (g_state == State::Disabled) g_messageTimeout = 0;
        return;
    }
    if (g_messageTimeout <= 2) {
        disable();
        g_messageTimeout = 1;
        return;
    }

    // Reset the countdown while the game window is in the background —
    // matches CCCaster so that messages aren't missed during alt-tab.
    const HWND fg = GetForegroundWindow();
    const HWND gameWnd = reinterpret_cast<HWND>(dll_hacks::windowHandle);
    if (gameWnd && fg != gameWnd)
        g_messageTimeout = g_initialTimeout;
    else
        --g_messageTimeout;
}

bool isShowingMessage() { return g_messageTimeout > 0; }

// ----------------------------------------------------------------------------
// D3D resource lifecycle
// ----------------------------------------------------------------------------

static void initOverlayText(IDirect3DDevice9* device) {
    D3DXCreateFontA(
        device,
        kOverlayFontHeight,
        kOverlayFontWidth,
        kOverlayFontWeight,
        1,                                  // mipmap levels
        FALSE,                              // italic
        ANSI_CHARSET,
        OUT_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        kOverlayFont,
        &g_font);

    // 4-vertex triangle strip covering the whole screen in clip space
    // (-1..1). The view transform scales it down to the bar height.
    static const Vertex verts[4] = {
        { -1, -1, 0, kOverlayBgColor },
        {  1, -1, 0, kOverlayBgColor },
        { -1,  1, 0, kOverlayBgColor },
        {  1,  1, 0, kOverlayBgColor },
    };

    device->CreateVertexBuffer(
        4 * sizeof(Vertex),
        0,
        Vertex::Format,
        D3DPOOL_MANAGED,
        &g_background,
        nullptr);

    void* ptr = nullptr;
    g_background->Lock(0, 0, &ptr, 0);
    std::memcpy(ptr, verts, sizeof(verts));
    g_background->Unlock();
}

static void invalidateOverlayText() {
    if (g_font) {
        g_font->OnLostDevice();
        g_font->Release();
        g_font = nullptr;
    }
    if (g_background) {
        g_background->Release();
        g_background = nullptr;
    }
}

static void initializeDirectX(IDirect3DDevice9* device) {
    if (!g_shouldInitDirectX) return;
    g_initializedDirectX = true;
    initOverlayText(device);
    caster::common::logger::info("overlay: DirectX resources initialized (font + background VB)");
}

void invalidateDeviceObjects() {
    if (!g_initializedDirectX) return;
    g_initializedDirectX = false;
    invalidateOverlayText();
}

// ----------------------------------------------------------------------------
// Rendering (called from D3DHook's PresentFrameBegin)
// ----------------------------------------------------------------------------

static void renderOverlayText(IDirect3DDevice9* device, const D3DVIEWPORT9& viewport) {
    if (g_state == State::Disabled) return;

    // ---- Measure centered message width (if any) for the background scale ----
    float messageWidth = 0.0f;
    if (isShowingMessage()) {
        RECT rect;
        rect.top = rect.left = 0;
        rect.right = 1;
        rect.bottom = kOverlayFontHeight;
        primitives::textCalcRect(g_font, g_text[1], rect, DT_CALCRECT, primitives::kColorBlack);
        messageWidth = float(rect.right) + 2.0f * kOverlayTextBorder;
    }

    // ---- Scale + translate the background quad to cover the bar ----
    const float scaleX = isShowingMessage() ? (messageWidth / float(viewport.Width)) : 1.0f;
    const float scaleY = float(g_height + 2 * kOverlayTextBorder) / float(viewport.Height);

    D3DXMATRIX scaleM, translateM;
    D3DXMatrixScaling(&scaleM, scaleX, scaleY, 1.0f);
    D3DXMatrixTranslation(&translateM, 0.0f, 1.0f - scaleY, 0.0f);

    device->SetTexture(0, nullptr);
    D3DXMATRIX viewM = scaleM * translateM;
    device->SetTransform(D3DTS_VIEW, &viewM);
    device->SetStreamSource(0, g_background, 0, sizeof(Vertex));
    device->SetFVF(Vertex::Format);
    device->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);

    // Only draw text once fully enabled (or while a message is showing).
    if (g_state != State::Enabled) return;

    const bool anyText = !g_text[0].empty() || !g_text[1].empty() || !g_text[2].empty();
    if (!anyText) return;

    const int centerX = int(viewport.Width) / 2;

    RECT rect;
    rect.left   = centerX - int(viewport.Width / 2) + kOverlayTextBorder;
    rect.right  = centerX + int(viewport.Width / 2) - kOverlayTextBorder;
    rect.top    = kOverlayTextBorder;
    rect.bottom = rect.top + g_height + kOverlayTextBorder;

    // Selectors (only when not mid-animation).
    if (g_newHeight == g_height) {
        if (g_shouldDrawSelector[0])
            primitives::drawRectangle(device,
                g_selector[0].left, g_selector[0].top,
                g_selector[0].right, g_selector[0].bottom,
                kOverlaySelectorLColor);
        if (g_shouldDrawSelector[1])
            primitives::drawRectangle(device,
                g_selector[1].left, g_selector[1].top,
                g_selector[1].right, g_selector[1].bottom,
                kOverlaySelectorRColor);
    }

    if (!g_text[0].empty())
        primitives::drawText(g_font, g_text[0], rect, DT_WORDBREAK | DT_LEFT,   kOverlayTextColor);
    if (!g_text[1].empty())
        primitives::drawText(g_font, g_text[1], rect, DT_WORDBREAK | DT_CENTER, kOverlayTextColor);
    if (!g_text[2].empty())
        primitives::drawText(g_font, g_text[2], rect, DT_WORDBREAK | DT_RIGHT,  kOverlayTextColor);
}

void presentFrameBegin(IDirect3DDevice9* device) {
    if (!g_initializedDirectX)
        initializeDirectX(device);

    D3DVIEWPORT9 viewport;
    device->GetViewport(&viewport);

    // Only draw in the main viewport. The game may have auxiliary viewports
    // (e.g. for texture previews) with different widths — skip those.
    const uint32_t screenW = *asU32(CC_SCREEN_WIDTH_ADDR);
    if (viewport.Width != screenW) return;

    renderOverlayText(device, viewport);
}

} // namespace caster::dll::overlay
