// src/dll/protocol/decoder.cpp

#include "decoder.hpp"

namespace caster::dll {

bool DecodedMessage::decode(const uint8_t* data, std::size_t len,
                             DecodedMessage& out) {
    if (len < 1) return false;

    MsgType tag = static_cast<MsgType>(data[0]);
    out.type = tag;

    switch (tag) {
        case MsgType::BothInputs:
            if (len < BothInputs::wire_size()) return false;
            out.bothInputs = BothInputs::deserialize(data, len);
            return true;

        case MsgType::PlayerInputs:
            if (len < PlayerInputs::wire_size()) return false;
            out.playerInputs = PlayerInputs::deserialize(data, len);
            return true;

        case MsgType::RngState:
            if (len < RngState::wire_size()) return false;
            out.rngState = RngState::deserialize(data, len);
            return true;

        case MsgType::InitialGameState:
            if (len < InitialGameState::wire_size()) return false;
            out.initialGameState = InitialGameState::deserialize(data, len);
            return true;

        case MsgType::NetplayConfig:
            out.netplayConfig = NetplayConfigMsg::deserialize(data, len);
            return true;

        case MsgType::ConfirmConfig:
            out.confirmConfig = ConfirmConfig::deserialize(data, len);
            return true;

        case MsgType::SyncHash:
            if (len < SyncHash::wire_size()) return false;
            out.syncHash = SyncHash::deserialize(data, len);
            return true;

        case MsgType::MenuIndex:
            if (len < MenuIndex::wire_size()) return false;
            out.menuIndex = MenuIndex::deserialize(data, len);
            return true;

        case MsgType::ChangeConfig:
            if (len < ChangeConfig::wire_size()) return false;
            out.changeConfig = ChangeConfig::deserialize(data, len);
            return true;

        case MsgType::TransitionIndex:
            if (len < TransitionIndex::wire_size()) return false;
            out.transitionIndex = TransitionIndex::deserialize(data, len);
            return true;

        case MsgType::ClientMode: {
            if (len < 1 + ClientMode::wire_size()) return false;
            out.clientMode = ClientMode::deserialize(data + 1, len - 1);
            return true;
        }

        case MsgType::SpectateConfig:
            if (len < SpectateConfig::min_wire_size()) return false;
            out.spectateConfig = SpectateConfig::deserialize(data, len);
            return true;

        default:
            return false;
    }
}

} // namespace caster::dll
