// src/dll/hooks/frame_limiter.hpp
// Ported from CCCaster DllFrameRate.hpp. Frame rate limiter for netplay sync.

#pragma once

#include <cstdint>

struct IDirect3DDevice9;

namespace caster::dll::frame_rate {

extern double desiredFps;
extern double actualFps;

void enable();
void limitFPS();
void PresentFrameEnd(IDirect3DDevice9* device);

} // namespace caster::dll::frame_rate
