// src/dll/overlay/primitives.hpp
//
// D3D9 drawing helpers. Ported from CCCaster targets/DllOverlayPrimitives.hpp.
// These are thin wrappers over IDirect3DDevice9 + ID3DXFont + ID3DXLine used
// by the overlay text renderer. No behaviour changes vs CCCaster — only
// formatting / include style adapted to ReCaster conventions.
//
// All functions are `static inline` (header-only) to keep them trivially
// inlinable from the single TU that uses them (overlay_ui.cpp).

#pragma once

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>

#include <array>
#include <cmath>
#include <string>

namespace caster::dll::overlay::primitives {

// Common colors (opaque).
inline constexpr D3DCOLOR kColorBlack = D3DCOLOR_XRGB(0,   0,   0);
inline constexpr D3DCOLOR kColorWhite = D3DCOLOR_XRGB(255, 255, 255);
inline constexpr D3DCOLOR kColorRed   = D3DCOLOR_XRGB(255, 0,   0);
inline constexpr D3DCOLOR kColorGreen = D3DCOLOR_XRGB(0,   255, 0);
inline constexpr D3DCOLOR kColorBlue  = D3DCOLOR_XRGB(0,   0,   255);

// Fill a solid rectangle. Uses Clear() with D3DCLEAR_TARGET — fast but
// ignores current blend state. Matches CCCaster's DrawRectangle().
inline void drawRectangle(IDirect3DDevice9* device, int x1, int y1, int x2, int y2, D3DCOLOR color) {
    const D3DRECT rect = { x1, y1, x2, y2 };
    device->Clear(1, &rect, D3DCLEAR_TARGET, color, 0, 0);
}

// Draw a hollow box (4 rectangles of width `w`).
inline void drawBox(IDirect3DDevice9* device, int x1, int y1, int x2, int y2, int w, D3DCOLOR color) {
    drawRectangle(device, x1, y1, x1 + w, y2, color);
    drawRectangle(device, x1, y1, x2, y1 + w, color);
    drawRectangle(device, x2 - w, y1, x2, y2, color);
    drawRectangle(device, x1, y2 - w, x2, y2, color);
}

// Draw a circle outline as an N-segment polyline. N is the vertex count
// (template param so the verts array is stack-allocated).
template <size_t N>
inline void drawCircle(IDirect3DDevice9* device, float x, float y, float r, D3DCOLOR color) {
    std::array<D3DXVECTOR2, N + 1> verts;
    for (size_t i = 0; i < verts.size(); ++i) {
        const float a = float(2.0f * 3.14159265358979323846 * i) / float(verts.size() - 1);
        verts[i].x = x + r * std::cos(a);
        verts[i].y = y + r * std::sin(a);
    }
    ID3DXLine* line = nullptr;
    D3DXCreateLine(device, &line);
    line->Begin();
    line->Draw(&verts[0], verts.size(), color);
    line->End();
    line->Release();
}

// Draw ASCII text into `rect` with the given format flags (DT_LEFT / DT_CENTER /
// DT_WORDBREAK / etc.). Returns the height in pixels (per ID3DXFont::DrawText).
// No-op if font is null.
inline int drawText(ID3DXFont* font, const std::string& text, RECT& rect, int flags, D3DCOLOR color) {
    if (!font) return 0;
    return font->DrawTextA(
        nullptr,                // ID3DXSprite (use default)
        text.c_str(),           // text (null-terminated; we pass -1 for length below)
        -1,                     // count: -1 = null-terminated
        &rect,
        flags | DT_NOCLIP,
        color);
}

// Wide-string variant (used by the future trial-mode combo text; kept for parity).
inline int drawTextW(ID3DXFont* font, const std::wstring& text, RECT& rect, int flags, D3DCOLOR color) {
    if (!font) return 0;
    return font->DrawTextW(
        nullptr,
        text.c_str(),
        -1,
        &rect,
        flags | DT_NOCLIP,
        color);
}

// Measure text into `rect` without drawing (DT_CALCRECT). The `color` arg
// is accepted for parity with CCCaster but ignored.
inline int textCalcRect(ID3DXFont* font, const std::string& text, RECT& rect, int flags, D3DCOLOR /*color*/) {
    if (!font) return 0;
    return font->DrawTextA(nullptr, text.c_str(), -1, &rect, DT_CALCRECT | flags, 0);
}

inline int textCalcRectW(ID3DXFont* font, const std::wstring& text, RECT& rect, int flags, D3DCOLOR /*color*/) {
    if (!font) return 0;
    return font->DrawTextW(nullptr, text.c_str(), -1, &rect, DT_CALCRECT | flags, 0);
}

} // namespace caster::dll::overlay::primitives
