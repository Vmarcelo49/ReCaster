// src/dll/netplay_connector.hpp
//
// Synchronous netplay transport for hook.dll. Polled once per frame from
// frameStep() — no extra threads, consistent with the DLL's "everything runs
// on the game's main thread via callback()" design (and with CCCaster, which
// also polls ENet synchronously inside its frame loop).
//
// This replaces the old threaded net_listener (port 7500 liveness probe),
// which conflicted with the design and was never referenced.
//
// Scope (current slice): connect → receive BothInputs → write P2 → send P1.
// Deferred: rollback, input delay, desync detection, RNG sync, spectate.

#pragma once

#include "../common/ipc/config_buffer.hpp"

#include <cstdint>

namespace caster::dll::netplay {

// One-time initialization from the IPC config. Idempotent.
// If cfg.is_netplay() is false this is a no-op (offline training/versus).
//   Host: binds local_udp_port, waits for the client's ENet CONNECT (driven
//         by poll() each frame until the peer connects).
//   Client: binds local_udp_port, issues enet_host_connect(peer_addr:peer_port).
void start(const caster::common::ipc::config_buffer::Config& cfg);

// Non-blocking poll. Drains pending ENet events (decodes BothInputs from the
// peer and stores them in the internal InputsContainer). Safe to call every
// frame; a no-op when netplay was never started.
void poll();

// Send the local player's input for `frame` to the peer as a BothInputs
// packet (local slot filled, remote slot zeroed). The direction/buttons use
// the game's split format (numpad + CC_BUTTON_* bitmask); they are packed
// into the wire's combined uint16_t via combine_input.
void send_local_input(uint16_t direction, uint16_t buttons, uint32_t frame);

// Write the latest available remote input for `frame` to the game's player
// `remote_player` (1 or 2) via process_manager::writeGameInput. If no remote
// input has arrived yet for this frame, the last known input is reused.
// Returns true if a remote input was applied.
bool apply_remote_input(uint8_t remote_player, uint32_t frame);

// True once the ENet peer has connected (handshake completed). Before
// connection, send_local_input drops and apply_remote_input writes neutral.
bool connected();

// Teardown — disconnects the peer and destroys the ENet host. Called from
// DLL_PROCESS_DETACH.
void shutdown();

} // namespace caster::dll::netplay
