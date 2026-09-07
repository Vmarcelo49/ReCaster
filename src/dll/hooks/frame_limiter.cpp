// src/dll/hooks/frame_limiter.cpp
// Ported from CCCaster DllFrameRate.cpp. Uses QPC for frame pacing.
//
// DXVK integration: when the launcher detects Vulkan + deploys DXVK
// (see src/exe/launcher/dxvk.{hpp,cpp}), it sets DXVK_FRAME_RATE=60 in
// the child process environment. DXVK's driver-level limiter is far
// more precise than our QPC-based limiter, so we stand down when DXVK
// is really active — but we verify that positively (env var set AND the
// loaded d3d9.dll comes from the game dir), because users commonly set
// DXVK_FRAME_RATE globally per DXVK guides and the mere presence of the
// var would otherwise leave native-D3D9 machines uncapped.
//
// The check is done once at enable() — we don't re-read the env var
// every frame. If the env var changes mid-session (it shouldn't — only
// the launcher sets it before CreateProcess), we won't pick it up.

#include "frame_limiter.hpp"
#include "game/addresses.hpp"
#include "asm_patches.hpp"

#include "../../common/logger.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdlib>
#include <cstring>

namespace caster::dll::frame_rate {

double desiredFps = 60.0;
double actualFps = 60.0;
static bool isEnabled = false;

// Cached result of "is DXVK active?". Read once at enable() time.
// DXVK is considered active if DXVK_FRAME_RATE is set (the launcher
// always sets it to "60" when deploying DXVK — see dxvk.cpp set_env_vars).
static bool g_dxvk_active = false;

// Returns true if DXVK is really handling frame pacing: the
// DXVK_FRAME_RATE env var is set (the launcher always sets it to "60"
// when deploying DXVK) AND the d3d9.dll loaded in this process comes
// from the game dir (where we deploy DXVK). The env var alone is not
// trustworthy — users commonly set DXVK_FRAME_RATE globally per DXVK
// setup guides, and then it would wrongly stand our limiter down on a
// native-D3D9 machine with no DXVK around, leaving the game uncapped.
static bool check_dxvk_active() {
    const char* v = std::getenv("DXVK_FRAME_RATE");
    if (v == nullptr || v[0] == '\0') return false;

    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    if (!d3d9) return false;  // D3D9 not loaded — DXVK can't be active
    char dll_path[MAX_PATH] = {0};
    if (!GetModuleFileNameA(d3d9, dll_path, sizeof(dll_path))) return false;

    // Game dir = dir of the host exe (MBAA.exe).
    char exe_path[MAX_PATH] = {0};
    if (!GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path))) return false;
    const char* exe_sep = strrchr(exe_path, '\\');
    const char* dll_sep = strrchr(dll_path, '\\');
    if (!exe_sep || !dll_sep) return false;
    // Same directory (case-insensitive)? Then the loaded d3d9 is a
    // game-dir override (DXVK or equivalent), not System32 native.
    size_t exe_dir_len = static_cast<size_t>(exe_sep - exe_path);
    size_t dll_dir_len = static_cast<size_t>(dll_sep - dll_path);
    if (exe_dir_len != dll_dir_len) return false;
    return _strnicmp(exe_path, dll_path, exe_dir_len) == 0;
}

void enable() {
    if (isEnabled) return;

    // Check if DXVK is active before applying our own limiter. If it
    // is, we skip the ASM hacks that disable the game's native limiter
    // (DXVK overrides it anyway via DXVK_FRAME_RATE=60), and limitFPS()
    // becomes a no-op.
    g_dxvk_active = check_dxvk_active();
    if (g_dxvk_active) {
        common::logger::info("frame_rate: DXVK detected (DXVK_FRAME_RATE env "
                             "var set) — our limiter is a no-op, DXVK handles "
                             "frame pacing at the driver level");
        isEnabled = true;  // mark as enabled so limitFPS() knows we're "on"
                            // (but it will early-return on the dxvk check)
        return;
    }

    WRITE_ASM_HACK(asm_hacks::disableFpsLimit);
    WRITE_ASM_HACK(asm_hacks::disableFpsCounter);
    isEnabled = true;
    caster::common::logger::info("frame_rate: FPS control enabled (native D3D9 path, "
                                 "DXVK not detected)");
}

static void newCasterFrameLimiter() {
    static LARGE_INTEGER baseFreq, prevFrameTime;
    static bool isFirstRun = true;

    if (isFirstRun) {
        isFirstRun = false;
        QueryPerformanceFrequency(&baseFreq);
        prevFrameTime.QuadPart = 0;
    }

    LARGE_INTEGER freq;
    freq.QuadPart = baseFreq.QuadPart / (LONGLONG)desiredFps;

    LARGE_INTEGER currTime;
    while (true) {
        QueryPerformanceCounter(&currTime);
        if (currTime.QuadPart - prevFrameTime.QuadPart > freq.QuadPart)
            break;
    }

    uint32_t temp = (uint32_t)(baseFreq.QuadPart / (currTime.QuadPart - prevFrameTime.QuadPart - 1));
    *(uint32_t*)CC_FPS_COUNTER_ADDR = temp;
    prevFrameTime.QuadPart = currTime.QuadPart;
}

void limitFPS() {
    if (!isEnabled || *(uint32_t*)CC_SKIP_FRAMES_ADDR)
        return;
    // DXVK is doing the frame limiting — our QPC-based limiter would
    // just add unnecessary overhead + cause double-limiting hitching.
    if (g_dxvk_active)
        return;
    newCasterFrameLimiter();
}

void PresentFrameEnd(IDirect3DDevice9*) {
    limitFPS();
}

} // namespace caster::dll::frame_rate
