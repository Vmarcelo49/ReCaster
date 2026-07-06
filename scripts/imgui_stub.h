// Minimal ImGui stub to test-compile ui_theme.cpp without the real ImGui.
// Just declares the symbols used by ui_theme.cpp; not for execution.
#pragma once
#include <cstdint>
#include <cstdarg>

typedef int ImGuiCol;
typedef int ImGuiStyleVar;
typedef int ImGuiWindowFlags;
typedef int ImGuiChildFlags;
typedef std::uint32_t ImU32;
typedef unsigned int Uint32;

struct ImVec2 {
    float x, y;
    ImVec2() : x(0), y(0) {}
    ImVec2(float x_, float y_) : x(x_), y(y_) {}
};
struct ImVec4 {
    float x, y, z, w;
    ImVec4() : x(0), y(0), z(0), w(0) {}
    ImVec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
};
struct ImDrawList {
    void AddRectFilledMultiColor(ImVec2, ImVec2, ImU32, ImU32, ImU32, ImU32) {}
    void AddText(ImVec2, ImU32, const char*) {}
    void AddTextUnformatted(ImVec2, ImU32, const char*) {}
};
struct ImFont {
    int dummy;
};
struct ImFontAtlas {
    ImFont* AddFontFromMemoryTTF(void*, int, float) { return nullptr; }
};
struct ImGuiIO {
    int ConfigFlags;
    const char* IniFilename;
    ImFontAtlas* Fonts;
    ImFont* FontDefault;
    ImGuiIO() : ConfigFlags(0), IniFilename(nullptr), Fonts(nullptr), FontDefault(nullptr) {}
};
struct ImGuiStyle {
    ImVec2 WindowPadding; float WindowRounding; float WindowBorderSize;
    float ChildRounding; float ChildBorderSize; ImVec2 FramePadding;
    float FrameRounding; float FrameBorderSize; ImVec2 ItemSpacing;
    ImVec2 ItemInnerSpacing; float IndentSpacing; float ScrollbarRounding;
    float GrabRounding; float TabRounding;
    ImVec4 Colors[64];
};
struct ImGuiContext {};

#define ImGuiConfigFlags_NavEnableKeyboard 1

enum {
    ImGuiCol_WindowBg=0, ImGuiCol_ChildBg, ImGuiCol_Border, ImGuiCol_Text,
    ImGuiCol_TextDisabled, ImGuiCol_TextLink, ImGuiCol_FrameBg,
    ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_Button,
    ImGuiCol_ButtonHovered, ImGuiCol_ButtonActive, ImGuiCol_Header,
    ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive, ImGuiCol_CheckMark,
    ImGuiCol_Separator, ImGuiCol_SeparatorHovered, ImGuiCol_SeparatorActive,
    ImGuiCol_ScrollbarBg, ImGuiCol_ScrollbarGrab, ImGuiCol_ScrollbarGrabHovered,
    ImGuiCol_ScrollbarGrabActive, ImGuiCol_SliderGrab, ImGuiCol_SliderGrabActive,
    ImGuiCol_InputTextCursor, ImGuiCol_Tab, ImGuiCol_TabHovered,
    ImGuiCol_TabSelected, ImGuiCol_TabSelectedOverline,
};
enum {
    ImGuiStyleVar_WindowPadding=0, ImGuiStyleVar_WindowRounding,
    ImGuiStyleVar_WindowBorderSize, ImGuiStyleVar_ChildRounding,
    ImGuiStyleVar_ChildBorderSize, ImGuiStyleVar_FramePadding,
    ImGuiStyleVar_FrameRounding, ImGuiStyleVar_FrameBorderSize,
    ImGuiStyleVar_ItemSpacing, ImGuiStyleVar_ItemInnerSpacing,
    ImGuiStyleVar_IndentSpacing, ImGuiStyleVar_ScrollbarRounding,
    ImGuiStyleVar_GrabRounding, ImGuiStyleVar_TabRounding,
};
enum {
    ImGuiChildFlags_Border=1, ImGuiChildFlags_AlwaysUseWindowPadding=2,
    ImGuiChildFlags_AutoResizeY=4,
};
enum {
    ImGuiWindowFlags_NoTitleBar=1, ImGuiWindowFlags_NoResize=2,
    ImGuiWindowFlags_NoMove=4, ImGuiWindowFlags_NoCollapse=8,
    ImGuiWindowFlags_NoBringToFrontOnFocus=16, ImGuiWindowFlags_NoNavFocus=32,
    ImGuiWindowFlags_NoScrollbar=64, ImGuiWindowFlags_NoScrollWithMouse=128,
};

namespace ImGui {
    inline ImGuiContext* CreateContext() { static ImGuiContext c; return &c; }
    inline void DestroyContext(ImGuiContext*) {}
    inline void NewFrame() {}
    inline void Render() {}
    inline void* GetDrawData() { return nullptr; }
    inline void PushStyleColor(ImGuiCol, ImVec4) {}
    inline void PopStyleColor(int = 1) {}
    inline void PushStyleVar(ImGuiStyleVar, float) {}
    inline void PushStyleVar(ImGuiStyleVar, ImVec2) {}
    inline void PopStyleVar(int = 1) {}
    inline void StyleColorsDark() {}
    inline ImGuiStyle& GetStyle() { static ImGuiStyle s; return s; }
    inline ImGuiIO& GetIO() { static ImGuiIO io; return io; }
    inline ImDrawList* GetWindowDrawList() { static ImDrawList d; return &d; }
    inline ImVec2 GetWindowPos() { return ImVec2(0, 0); }
    inline float GetWindowWidth() { return 0; }
    inline float GetWindowHeight() { return 0; }
    inline ImU32 ColorConvertFloat4ToU32(ImVec4) { return 0; }
    inline bool BeginChild(const char*, ImVec2, ImGuiChildFlags = 0, ImGuiWindowFlags = 0) { return true; }
    inline void EndChild() {}
    inline void TextUnformatted(const char*) {}
    inline void Text(const char*, ...) {}
    inline void TextDisabled(const char*, ...) {}
    inline void TextWrapped(const char*, ...) {}
    inline void TextColored(ImVec4, const char*, ...) {}
    inline void Spacing() {}
    inline void Separator() {}
    inline void Dummy(ImVec2) {}
    inline bool Button(const char*, ImVec2) { return false; }
    inline ImVec2 CalcTextSize(const char*) { return ImVec2(0,0); }
    inline ImVec2 CalcTextSize(const char*, const char*) { return ImVec2(0,0); }
    inline void SetCursorPos(ImVec2) {}
    inline void SetNextWindowPos(ImVec2, int = 0) {}
    inline void SetNextWindowSize(ImVec2, int = 0) {}
    inline bool Begin(const char*, bool* = nullptr, ImGuiWindowFlags = 0) { return true; }
    inline void End() {}
    inline void BulletText(const char*, ...) {}
}
