// src/dll/dll_hacks.hpp
// Ported from CCCaster DllHacks.hpp. Lifecycle: initializePreLoad, initializePostLoad, deinitialize.

#pragma once

namespace caster::dll::dll_hacks {

extern void* windowHandle;

void initializePreLoad();
void initializePostLoad();
void deinitialize();

} // namespace caster::dll::dll_hacks
