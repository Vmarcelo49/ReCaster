// src/exe/pages/waiting_for_peer.hpp
//
// Full-screen view shown during the netplay handshake. Calls session.step()
// each frame and renders status, room code, ping stats, delay, and the
// Start Match / Cancel buttons.

#pragma once

#include "../session/session.hpp"

#include <string>

namespace caster::exe::pages::waiting_for_peer {

// Draw the waiting-for-peer view. `session` is driven (step() called) here.
// Returns true if the user clicked Cancel (caller should abort); false
// otherwise. The caller should also check session.state() == Launching to
// detect handshake completion.
struct DrawResult {
    bool       cancelled      = false;
    bool       launching      = false;  // session transitioned to Launching
    std::string error_message;          // populated if session failed
};

DrawResult draw(caster::exe::session::NetplaySession& session);

} // namespace caster::exe::pages::waiting_for_peer
