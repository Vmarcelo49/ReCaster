// src/dll/hooks/frame_limiter.cpp
// Ported from CCCaster DllFrameRate.cpp. Uses QPC for frame pacing.

#include "frame_limiter.hpp"
#include "game/addresses.hpp"
#include "asm_patches.hpp"

#include <windows.h>

namespace caster::dll::frame_rate {

double desiredFps = 60.0;
double actualFps = 60.0;
static bool isEnabled = false;

void enable() {
    if (isEnabled) return;
    WRITE_ASM_HACK(asm_hacks::disableFpsLimit);
    WRITE_ASM_HACK(asm_hacks::disableFpsCounter);
    isEnabled = true;
    caster::common::logger::info("frame_rate: FPS control enabled");
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
    newCasterFrameLimiter();
}

void PresentFrameEnd(IDirect3DDevice9*) {
    limitFPS();
}

} // namespace caster::dll::frame_rate
