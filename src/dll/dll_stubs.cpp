// src/dll/dll_stubs.cpp
//
// Stubs for symbols that are referenced by the DLL infrastructure but
// whose full implementation belongs to Phase F (DllNetplayManager + DllMain
// refactoring). These will be replaced when the netplay engine is ported.
//
// Without these stubs, hook.dll fails to link.

#include "constants.hpp"
#include "frame_rate.hpp"
#include "../common/logger.hpp"

#include <d3d9.h>
#include <string>

// ---- callback() — the per-frame hook entry point ----
// asm_hacks.hpp declares `extern "C" void callback()` inside namespace
// caster::dll::asm_hacks, but extern "C" strips the namespace mangling.
// The ASM patches reference the unmangled symbol `_callback`.
// This stub will be replaced by the full DllMain::callback() in Phase F.
extern "C" void callback() {
    // Intentionally empty — will be the heart of the frame-step loop.
}

// ---- stopDllMain() — called on fatal error / Alt+F4 ----
// dll_hacks.cpp references caster::dll::dll_hacks::stopDllMain.
namespace caster::dll::dll_hacks {
void stopDllMain(const std::string& error) {
    caster::common::logger::info("stopDllMain called: '{}'", error);
    // Full implementation will: stop the netplay engine, revert hacks,
    // and exit the DLL cleanly. For now just log.
}
} // namespace caster::dll::dll_hacks

// ---- D3DHook callbacks — declared in D3DHook.h (global namespace) ----
// PresentFrameEnd is also declared in frame_rate.hpp inside namespace
// caster::dll::frame_rate, but D3DHook.cc calls it from global scope.
// We provide global-scope implementations here.

void PresentFrameBegin(IDirect3DDevice9*) {
    // Called before IDirect3DDevice9::Present. Will be used for overlay
    // rendering when that feature is implemented.
}

void EndScene(IDirect3DDevice9*) {
    // Called at IDirect3DDevice9::EndScene. Will be used for ImGui overlay
    // rendering when that feature is implemented.
}

void InvalidateDeviceObjects() {
    // Called when the D3D device is lost (alt-tab, resolution change).
    // Will be used to release overlay resources (fonts, vertex buffers).
}

// PresentFrameEnd — delegates to frame_rate::limitFPS()
void PresentFrameEnd(IDirect3DDevice9* device) {
    caster::dll::frame_rate::PresentFrameEnd(device);
}
