// src/dll/netplay_connector.hpp
//
// Synchronous ENet transport for hook.dll. Polled once per frame from
// frameStep() — no extra threads, consistent with the DLL's "everything
// runs on the game's main thread via callback()" design.
//
// This is the wire-protocol layer: it sends/receives serialized message
// structs (PlayerInputs, TransitionIndex, RngState, MenuIndex, etc.)
// and exposes a small send/recv API that the NetplayManager-aware
// frameStep in dll_main.cpp uses.
//
// The NetplayManager (netplay_manager.{hpp,cpp}) is the brain that
// decides WHAT to send and how to interpret what's received — this
// module is just the pipe.

#pragma once

#include "protocol/messages.hpp"

#include "../common/ipc/config_buffer.hpp"

#include <cstdint>
#include <optional>

namespace caster::dll::netplay {

// ---- Lifecycle ----

// One-time initialization from the IPC config. Idempotent.
// If cfg.is_netplay() is false this is a no-op (offline training/versus).
//   Host: binds local_udp_port, waits for the client's ENet CONNECT (driven
//         by poll() each frame until the peer connects).
//   Client: binds local_udp_port, issues enet_host_connect(peer_addr:peer_port).
void start(const caster::common::ipc::config_buffer::Config& cfg);

// Non-blocking poll. Drains pending ENet events and dispatches received
// messages to the per-type inbox queues. Safe to call every frame; a
// no-op when netplay was never started.
void poll();

// Teardown — disconnects the peer and destroys the ENet host. Called
// from DLL_PROCESS_DETACH.
void shutdown();

// ---- Connection state ----

// True once the ENet peer has connected (handshake completed). Before
// connection, all send_* calls drop silently and recv_* return nullopt.
bool connected();
bool isHost();  // mirror of cfg.is_host() at start time

// ---- Outbox (frameStep → peer) ----

// Send a PlayerInputs message containing the local player's recent
// input history. Called every frame in netplay mode after the local
// player's input is set on the NetplayManager.
void sendPlayerInputs(const PlayerInputs& pi);

// Send a TransitionIndex announcing that our NetplayState just changed.
// The peer uses this to advance its remoteIndex (its view of where we
// are in the FSM).
void sendTransitionIndex(uint32_t index);

// Send our local retry menu selection. The peer uses this to decide
// when both sides have selected and the auto-navigation can begin.
void sendMenuIndex(const MenuIndex& mi);

// Send a RngState snapshot. Only the host sends these (to the client,
// at the start of each round) so the client's RNG matches the host's.
void sendRngState(const RngState& rs);

// Send a SyncHash snapshot. Both sides send these periodically (every
// 5*60 frames or 150 frames, whichever comes first) and compare them
// to detect desyncs. A mismatch triggers a delayedStop("Desync!").
void sendSyncHash(const SyncHash& sh);

// ---- Inbox (peer → frameStep) ----

// Drain the remote-player PlayerInputs inbox. Each call returns one
// message in FIFO order; nullopt when empty. The caller (frameStep)
// forwards these to netMan.setInputs(remotePlayer, ...).
std::optional<PlayerInputs> recvPlayerInputs();

// Drain the TransitionIndex inbox. The caller forwards these to
// netMan.setRemoteIndex(...).
std::optional<uint32_t> recvTransitionIndex();

// Drain the MenuIndex inbox. The caller forwards these to
// netMan.setRemoteRetryMenuIndex(...).
std::optional<MenuIndex> recvMenuIndex();

// Drain the RngState inbox. The caller forwards these to
// netMan.setRngState(...).
std::optional<RngState> recvRngState();

// Drain the SyncHash inbox. The caller stores these for comparison
// against locally-generated SyncHashes (desync detection).
std::optional<SyncHash> recvSyncHash();

} // namespace caster::dll::netplay
