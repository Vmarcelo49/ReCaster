// src/dll/messages.cpp
//
// Manual binary serialization for all netplay message types.
// All integers are little-endian. No padding. No cereal.

#include "messages.hpp"

#include <cstring>

namespace caster::dll {

// ---- BothInputs ----------------------------------------------------------

std::vector<uint8_t> BothInputs::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(wire_size());
    out.push_back(static_cast<uint8_t>(MsgType::BothInputs));
    write_u64_le(out, indexedFrame.value);
    for (int p = 0; p < 2; ++p)
        for (int i = 0; i < NUM_INPUTS; ++i)
            write_u16_le(out, inputs[p][i]);
    return out;
}

BothInputs BothInputs::deserialize(const uint8_t* data, std::size_t len) {
    BothInputs msg;
    if (len < wire_size()) return msg;
    ++data; // skip tag
    msg.indexedFrame.value = read_u64_le(data);
    data += 8;
    for (int p = 0; p < 2; ++p)
        for (int i = 0; i < NUM_INPUTS; ++i) {
            msg.inputs[p][i] = read_u16_le(data);
            data += 2;
        }
    return msg;
}

// ---- PlayerInputs --------------------------------------------------------

std::vector<uint8_t> PlayerInputs::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(wire_size());
    out.push_back(static_cast<uint8_t>(MsgType::PlayerInputs));
    write_u64_le(out, indexedFrame.value);
    for (int i = 0; i < NUM_INPUTS; ++i)
        write_u16_le(out, inputs[i]);
    return out;
}

PlayerInputs PlayerInputs::deserialize(const uint8_t* data, std::size_t len) {
    PlayerInputs msg;
    if (len < wire_size()) return msg;
    ++data;
    msg.indexedFrame.value = read_u64_le(data);
    data += 8;
    for (int i = 0; i < NUM_INPUTS; ++i) {
        msg.inputs[i] = read_u16_le(data);
        data += 2;
    }
    return msg;
}

// ---- RngState ------------------------------------------------------------

std::vector<uint8_t> RngState::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(wire_size());
    out.push_back(static_cast<uint8_t>(MsgType::RngState));
    write_u32_le(out, index);
    write_u32_le(out, rngState0);
    write_u32_le(out, rngState1);
    write_u32_le(out, rngState2);
    out.insert(out.end(), rngState3.begin(), rngState3.end());
    return out;
}

RngState RngState::deserialize(const uint8_t* data, std::size_t len) {
    RngState msg;
    if (len < wire_size()) return msg;
    ++data;
    msg.index    = read_u32_le(data); data += 4;
    msg.rngState0 = read_u32_le(data); data += 4;
    msg.rngState1 = read_u32_le(data); data += 4;
    msg.rngState2 = read_u32_le(data); data += 4;
    std::memcpy(msg.rngState3.data(), data, CC_RNG_STATE3_SIZE);
    return msg;
}

// ---- InitialGameState ----------------------------------------------------

std::vector<uint8_t> InitialGameState::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(wire_size());
    out.push_back(static_cast<uint8_t>(MsgType::InitialGameState));
    write_u64_le(out, indexedFrame.value);
    write_u32_le(out, stage);
    out.push_back(netplayState);
    out.push_back(isTraining);
    out.push_back(chara[0]);
    out.push_back(chara[1]);
    out.push_back(moon[0]);
    out.push_back(moon[1]);
    out.push_back(color[0]);
    out.push_back(color[1]);
    return out;
}

InitialGameState InitialGameState::deserialize(const uint8_t* data, std::size_t len) {
    InitialGameState msg;
    if (len < wire_size()) return msg;
    ++data;
    msg.indexedFrame.value = read_u64_le(data); data += 8;
    msg.stage       = read_u32_le(data); data += 4;
    msg.netplayState = *data++;
    msg.isTraining   = *data++;
    msg.chara[0] = *data++;
    msg.chara[1] = *data++;
    msg.moon[0]  = *data++;
    msg.moon[1]  = *data++;
    msg.color[0] = *data++;
    msg.color[1] = *data++;
    return msg;
}

// ---- NetplayConfigMsg ----------------------------------------------------

std::vector<uint8_t> NetplayConfigMsg::serialize() const {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(MsgType::NetplayConfig));
    mode.serialize(out);
    out.push_back(delay);
    out.push_back(rollback);
    out.push_back(rollbackDelay);
    out.push_back(winCount);
    out.push_back(hostPlayer);
    write_u16_le(out, broadcastPort);
    write_string_lp(out, names[0]);
    write_string_lp(out, names[1]);
    write_string_lp(out, sessionId);
    return out;
}

NetplayConfigMsg NetplayConfigMsg::deserialize(const uint8_t* data, std::size_t len) {
    NetplayConfigMsg msg;
    // Need: 1 (tag) + 2 (ClientMode) + 7 (delay, rollback, rollbackDelay, winCount, hostPlayer, broadcastPort u16)
    if (len < 1 + ClientMode::wire_size() + 7) return msg;
    ++data; --len;
    msg.mode = ClientMode::deserialize(data, len);
    data += ClientMode::wire_size();
    len -= ClientMode::wire_size();
    if (len < 7) return msg;
    msg.delay         = *data++;
    msg.rollback      = *data++;
    msg.rollbackDelay = *data++;
    msg.winCount      = *data++;
    msg.hostPlayer    = *data++;
    msg.broadcastPort = read_u16_le(data); data += 2; len -= 7;
    msg.names[0]  = read_string_lp(data, len);
    msg.names[1]  = read_string_lp(data, len);
    msg.sessionId = read_string_lp(data, len);
    return msg;
}

// ---- SyncHash ------------------------------------------------------------

bool SyncHash::operator==(const SyncHash& other) const {
    if (indexedFrame.value != other.indexedFrame.value) return false;
    if (hash != other.hash) return false;
    if (roundTimer != other.roundTimer) return false;
    if (realTimer != other.realTimer) return false;
    if (cameraX != other.cameraX) return false;
    if (cameraY != other.cameraY) return false;

    // Compare chara hash — but ignore seqState when seq == 0 (neutral sequence).
    // This matches the CCCaster behavior to avoid false desyncs during transitions.
    for (int i = 0; i < 2; ++i) {
        const auto& a = chara[i];
        const auto& b = other.chara[i];
        if (a.seq        != b.seq)        return false;
        if (a.health     != b.health)     return false;
        if (a.redHealth  != b.redHealth)  return false;
        if (a.meter      != b.meter)      return false;
        if (a.heat       != b.heat)       return false;
        if (a.guardBar   != b.guardBar)   return false;
        if (a.guardQuality != b.guardQuality) return false;
        if (a.x          != b.x)          return false;
        if (a.y          != b.y)          return false;
        if (a.chara      != b.chara)      return false;
        if (a.moon       != b.moon)       return false;
        // Only compare seqState when seq != 0 (non-neutral sequence).
        if (a.seq != 0 && a.seqState != b.seqState) return false;
    }
    return true;
}

std::vector<uint8_t> SyncHash::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(wire_size());
    out.push_back(static_cast<uint8_t>(MsgType::SyncHash));
    write_u64_le(out, indexedFrame.value);
    out.insert(out.end(), hash.begin(), hash.end());
    write_u32_le(out, roundTimer);
    write_u32_le(out, realTimer);
    write_i32_le(out, cameraX);
    write_i32_le(out, cameraY);
    for (int i = 0; i < 2; ++i) {
        const auto& c = chara[i];
        write_u32_le(out, c.seq);
        write_u32_le(out, c.seqState);
        write_u32_le(out, c.health);
        write_u32_le(out, c.redHealth);
        write_u32_le(out, c.meter);
        write_u32_le(out, c.heat);
        write_f32_le(out, c.guardBar);
        write_f32_le(out, c.guardQuality);
        write_i32_le(out, c.x);
        write_i32_le(out, c.y);
        write_u16_le(out, c.chara);
        write_u16_le(out, c.moon);
    }
    return out;
}

SyncHash SyncHash::deserialize(const uint8_t* data, std::size_t len) {
    SyncHash msg;
    if (len < wire_size()) return msg;
    ++data;
    msg.indexedFrame.value = read_u64_le(data); data += 8;
    std::memcpy(msg.hash.data(), data, 16); data += 16;
    msg.roundTimer = read_u32_le(data); data += 4;
    msg.realTimer  = read_u32_le(data); data += 4;
    msg.cameraX    = read_i32_le(data); data += 4;
    msg.cameraY    = read_i32_le(data); data += 4;
    for (int i = 0; i < 2; ++i) {
        auto& c = msg.chara[i];
        c.seq        = read_u32_le(data); data += 4;
        c.seqState   = read_u32_le(data); data += 4;
        c.health     = read_u32_le(data); data += 4;
        c.redHealth  = read_u32_le(data); data += 4;
        c.meter      = read_u32_le(data); data += 4;
        c.heat       = read_u32_le(data); data += 4;
        c.guardBar   = read_f32_le(data); data += 4;
        c.guardQuality = read_f32_le(data); data += 4;
        c.x          = read_i32_le(data); data += 4;
        c.y          = read_i32_le(data); data += 4;
        c.chara      = read_u16_le(data); data += 2;
        c.moon       = read_u16_le(data); data += 2;
    }
    return msg;
}

// ---- MenuIndex -----------------------------------------------------------

std::vector<uint8_t> MenuIndex::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(wire_size());
    out.push_back(static_cast<uint8_t>(MsgType::MenuIndex));
    write_u32_le(out, index);
    out.push_back(static_cast<uint8_t>(menuIndex));
    return out;
}

MenuIndex MenuIndex::deserialize(const uint8_t* data, std::size_t len) {
    MenuIndex msg;
    if (len < wire_size()) return msg;
    ++data;
    msg.index     = read_u32_le(data); data += 4;
    msg.menuIndex = static_cast<int8_t>(*data);
    return msg;
}

// ---- ChangeConfig --------------------------------------------------------

std::vector<uint8_t> ChangeConfig::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(wire_size());
    out.push_back(static_cast<uint8_t>(MsgType::ChangeConfig));
    out.push_back(static_cast<uint8_t>(value));
    write_u64_le(out, indexedFrame.value);
    out.push_back(delay);
    out.push_back(rollbackDelay);
    out.push_back(rollback);
    return out;
}

ChangeConfig ChangeConfig::deserialize(const uint8_t* data, std::size_t len) {
    ChangeConfig msg;
    if (len < wire_size()) return msg;
    ++data;
    msg.value          = static_cast<Type>(*data++);
    msg.indexedFrame.value = read_u64_le(data); data += 8;
    msg.delay          = *data++;
    msg.rollbackDelay  = *data++;
    msg.rollback       = *data++;
    return msg;
}

// ---- TransitionIndex -----------------------------------------------------

std::vector<uint8_t> TransitionIndex::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(wire_size());
    out.push_back(static_cast<uint8_t>(MsgType::TransitionIndex));
    write_u32_le(out, index);
    return out;
}

TransitionIndex TransitionIndex::deserialize(const uint8_t* data, std::size_t len) {
    TransitionIndex msg;
    if (len < wire_size()) return msg;
    ++data;
    msg.index = read_u32_le(data);
    return msg;
}

} // namespace caster::dll
