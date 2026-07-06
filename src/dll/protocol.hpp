// src/dll/protocol.hpp
//
// Protocol dispatcher. Without cereal — uses the manual serialize/deserialize
// methods defined in messages.hpp. This replaces the entire CCCaster
// lib/Protocol.hpp + lib/Protocol.cpp (~560 LOC) with ~80 LOC.
//
// Wire format per message: [1-byte tag][message-specific payload]
// (No compression, no hash, no sequence numbers in this simplified version.
// ENet provides reliable ordered delivery so we don't need our own.)

#pragma once

#include "messages.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace caster::dll {

// A decoded message — holds one of the known message types.
// Use std::visit or std::get_if to access.
struct DecodedMessage {
    MsgType type = MsgType::BothInputs;  // dummy default

    // Storage for each possible message type.
    BothInputs         bothInputs;
    PlayerInputs       playerInputs;
    RngState           rngState;
    InitialGameState   initialGameState;
    NetplayConfigMsg   netplayConfig;
    ConfirmConfig      confirmConfig;
    SyncHash           syncHash;
    MenuIndex          menuIndex;
    ChangeConfig       changeConfig;
    TransitionIndex    transitionIndex;
    ClientMode         clientMode;

    // Decode a raw byte buffer into a DecodedMessage.
    // Returns false if the tag is unknown or the buffer is too short.
    static bool decode(const uint8_t* data, std::size_t len, DecodedMessage& out);

    // Encode helper: append a serialized message to a buffer.
    // (Most messages already have serialize() that returns a vector.)
};

} // namespace caster::dll
