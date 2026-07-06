// src/dll/net_listener.hpp
//
// Placeholder ENet host that binds to 0.0.0.0:7500 (UDP, as per ENet).
// It accepts connections, immediately disconnects them, and serves only to
// prove the networking stack is alive inside the injected DLL.
//
// Threading contract:
//   - start_net_listener() spawns its own worker thread; returns immediately.
//   - stop_net_listener() signals the thread and joins it. Safe to call
//     from DllMain's DLL_PROCESS_DETACH handler.
//   - Status getters (running / connection count) are safe to call from any
//     other thread; they take a mutex internally.

#pragma once

#include <atomic>
#include <cstdint>

namespace caster::dll {

// Spawn the ENet listener thread. The `running` flag is borrowed (not owned)
// and is checked each loop iteration; setting it to false is the canonical
// way to request stop.
void start_net_listener(std::atomic<bool>& running);

// Signal-and-join the listener thread. Idempotent.
void stop_net_listener();

// True if the ENet host is bound and accepting events.
bool net_listener_running();

// Count of peer connections accepted (and immediately closed) since start.
// Useful as a "is the listener actually doing anything" indicator for the
// ImGui status panel.
uint64_t net_listener_accepted_count();

} // namespace caster::dll
