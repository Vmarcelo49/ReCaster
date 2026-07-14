// src/dll/spec/spectate_client.hpp
//
// Phase C / Fase 3 — Client-side spectator support.
//
// When the local client is a spectator (cfg.is_spectator() == true), the
// DLL receives SpectateConfig + InitialGameState + RngState from the host
// on connect, then a stream of BothInputs messages every few frames. The
// SpectateClient processes these and drives the NetplayManager to replay
// the match in real-time.
//
// Threading model:
//   - All methods are called from the GAME THREAD (via drainNetplayInbox
//     in frameStep). The NetworkThread routes SpectateConfig /
//     InitialGameState / BothInputs / RngState / MenuIndex to the
//     existing inbox queues, and drainNetplayInbox dispatches them to
//     the SpectateClient.
//   - No internal threading, no mutexes — single-threaded game-thread
//     access only.
//
// Spectator protocol (client-side perspective):
//   1. Connect to host via ENet (handled by NetworkThread::start)
//   2. Receive SpectateConfig → configure NetplayManager (delay, rollback,
//      hostPlayer, names, isTraining)
//   3. Receive InitialGameState → force FSM to AutoCharaSelect with the
//      given indexedFrame + chara/moon/color/stage
//   4. Receive RngState → setRngState on NetplayManager
//   5. Receive BothInputs → setBothInputs on NetplayManager (writes both
//      players' inputs at once)
//   6. Receive MenuIndex → setRetryMenuIndex on NetplayManager
//
// The spectator never sends anything — it only receives. The host's
// SpectatorManager handles the round-robin broadcast.

#pragma once

#include "../protocol/messages.hpp"
#include "../game/addresses.hpp"

#include <cstdint>
#include <optional>

namespace caster::dll {

class NetplayManager;

namespace spec {

class SpectateClient {
public:
    // netMan is the global NetplayManager. Not owned.
    explicit SpectateClient(NetplayManager* netManPtr);
    // Destructor must be defined in the .cpp because unique_ptr<SpectateClient>
    // in dll_main.cpp needs the type complete to invoke the destructor.
    // The = default here would make it inline, requiring the full
    // SpectateClient definition everywhere unique_ptr<SpectateClient> is
    // destroyed. Defining it out-of-line in the .cpp solves this.
    ~SpectateClient();

    SpectateClient(const SpectateClient&)            = delete;
    SpectateClient& operator=(const SpectateClient&) = delete;

    // ---- Message handlers (called from drainNetplayInbox) ----

    // Called when a SpectateConfig message arrives. Configures the
    // NetplayManager with the host's match settings. Only the first
    // call has effect — subsequent calls are ignored (the host sends
    // it once on connect).
    void onSpectateConfig(const SpectateConfig& sc);

    // Called when an InitialGameState message arrives. Forces the FSM
    // into AutoCharaSelect state (spectators skip CharaSelect) and
    // writes the chara/moon/color/stage into game memory so the
    // Loading transition picks them up.
    void onInitialGameState(const InitialGameState& igs);

    // Called when an RngState message arrives. Forwards to
    // NetplayManager::setRngState.
    void onRngState(const RngState& rs);

    // Called when a MenuIndex message arrives. Forwards to
    // NetplayManager::setRetryMenuIndex.
    void onMenuIndex(const MenuIndex& mi);

    // Called when a BothInputs message arrives. Forwards to
    // NetplayManager::setBothInputs — this is the per-frame input
    // batch from the host that the spectator replays.
    void onBothInputs(const BothInputs& bi);

    // ---- State ----

    bool configReceived() const { return configReceived_; }
    bool initialReceived() const { return initialReceived_; }

    // The spectator's "current position" in the input stream — the
    // indexedFrame of the last BothInputs we successfully applied.
    // Used by the overlay/debug display to show how far behind the
    // spectator is.
    IndexedFrame currentPosition() const { return currentPosition_; }

private:
    NetplayManager* _netManPtr = nullptr;
    bool configReceived_ = false;
    bool initialReceived_ = false;
    IndexedFrame currentPosition_ = {{0, 0}};
};

} // namespace spec
} // namespace caster::dll
