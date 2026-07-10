// src/dll/messages.hpp
//
// Netplay protocol message types. Ported from CCCaster's netplay/Messages.hpp,
// adapted for C++23:
// - No cereal — each message has manual binary serialize()/deserialize()
// - No trial fields (trialAudioCue, trialFlashColor)
// - No SpectateConfig (spectate not in v1)
// - No tournament mode flag
// - Uses our IndexedFrame from constants.hpp
//
// Wire format: all integers little-endian, no padding. Each message is
// prefixed with a 1-byte tag (MsgType) when sent over the wire.

#pragma once

#include "constants.hpp"
#include "character_select.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace caster::dll {

// ---- Message type tags (wire protocol) -----------------------------------

enum class MsgType : uint8_t {
    BothInputs       = 1,
    PlayerInputs     = 2,
    RngState         = 3,
    InitialGameState = 4,
    NetplayConfig    = 5,
    ConfirmConfig    = 6,
    SyncHash         = 7,
    MenuIndex        = 8,
    ChangeConfig     = 9,
    TransitionIndex  = 10,
    ClientMode       = 11,
    VersionConfig    = 12,
    PingStats        = 13,
    InitialConfig    = 14,
    ErrorMessage     = 15,
};

// ---- ClientMode (mode + flags packed in 2 bytes) -------------------------

struct ClientMode {
    enum class Mode : uint8_t {
        Unknown          = 0,
        Host             = 1,
        Client           = 2,
        SpectateNetplay  = 3,
        SpectateBroadcast = 4,
        Broadcast        = 5,
        Offline          = 6,
    };

    Mode    value = Mode::Unknown;
    uint8_t flags = 0;

    // Flag bits
    static constexpr uint8_t Training     = 0x01;
    static constexpr uint8_t GameStarted  = 0x02;
    static constexpr uint8_t UdpTunnel    = 0x04;
    static constexpr uint8_t IsWine       = 0x08;
    static constexpr uint8_t VersusCPU    = 0x10;
    static constexpr uint8_t Replay       = 0x20;

    ClientMode() = default;
    ClientMode(Mode v, uint8_t f) : value(v), flags(f) {}

    bool isHost() const      { return value == Mode::Host; }
    bool isClient() const    { return value == Mode::Client; }
    bool isOffline() const   { return value == Mode::Offline; }
    bool isOnline() const    { return !isOffline(); }
    bool isNetplay() const   { return value == Mode::Host || value == Mode::Client; }
    bool isTraining() const  { return (flags & Training) != 0; }
    bool isVersus() const    { return !isTraining(); }
    bool isVersusCPU() const { return (flags & VersusCPU) != 0 && !isTraining(); }
    bool isSinglePlayer() const { return isNetplay() || isVersusCPU(); }

    void clear() { value = Mode::Unknown; flags = 0; }

    // 2-byte serialize: [Mode u8][flags u8]
    void serialize(std::vector<uint8_t>& out) const {
        out.push_back(static_cast<uint8_t>(value));
        out.push_back(flags);
    }
    static ClientMode deserialize(const uint8_t* data, std::size_t len) {
        ClientMode m;
        if (len >= 2) {
            m.value = static_cast<Mode>(data[0]);
            m.flags = data[1];
        }
        return m;
    }
    static constexpr std::size_t wire_size() { return 2; }
};

// ---- BaseInputs (shared by PlayerInputs and BothInputs) ------------------

struct BaseInputs {
    IndexedFrame indexedFrame = {{0, 0}};

    uint32_t getIndex() const { return indexedFrame.parts.index; }
    uint32_t getFrame() const { return indexedFrame.parts.frame; }

    uint32_t getStartFrame() const {
        return (indexedFrame.parts.frame + 1 < NUM_INPUTS)
            ? 0
            : indexedFrame.parts.frame + 1 - NUM_INPUTS;
    }
    uint32_t getEndFrame() const { return indexedFrame.parts.frame + 1; }
    std::size_t size() const { return getEndFrame() - getStartFrame(); }
};

// ---- BothInputs (the main per-frame message) -----------------------------
// Wire: [tag=1][u64 indexedFrame][u16×NUM_INPUTS × 2 players]
// Size: 1 + 8 + 2*30*2 = 129 bytes

struct BothInputs : public BaseInputs {
    std::array<std::array<uint16_t, NUM_INPUTS>, 2> inputs{};

    BothInputs() = default;
    explicit BothInputs(IndexedFrame f) { indexedFrame = f; }

    std::vector<uint8_t> serialize() const;
    static BothInputs deserialize(const uint8_t* data, std::size_t len);
    static constexpr std::size_t wire_size() {
        return 1 + 8 + sizeof(inputs);
    }
};

// ---- PlayerInputs (single-player inputs, used for spectator) -------------

struct PlayerInputs : public BaseInputs {
    std::array<uint16_t, NUM_INPUTS> inputs{};

    PlayerInputs() = default;
    explicit PlayerInputs(IndexedFrame f) { indexedFrame = f; }

    std::vector<uint8_t> serialize() const;
    static PlayerInputs deserialize(const uint8_t* data, std::size_t len);
    static constexpr std::size_t wire_size() {
        return 1 + 8 + sizeof(inputs);
    }
};

// ---- RngState (4 RNG fields, 232 bytes total) ----------------------------
// Wire: [tag=3][u32 index][u32 rng0][u32 rng1][u32 rng2][220 bytes rng3]
// Size: 1 + 4 + 4 + 4 + 4 + 220 = 237 bytes

struct RngState {
    uint32_t index = 0;
    uint32_t rngState0 = 0, rngState1 = 0, rngState2 = 0;
    std::array<char, CC_RNG_STATE3_SIZE> rngState3{};

    RngState() = default;
    explicit RngState(uint32_t idx) : index(idx) {}

    std::vector<uint8_t> serialize() const;
    static RngState deserialize(const uint8_t* data, std::size_t len);
    static constexpr std::size_t wire_size() {
        return 1 + 4 + 4 + 4 + 4 + CC_RNG_STATE3_SIZE;
    }
};

// ---- InitialGameState (chara/moon/color/stage snapshot) ------------------
// Wire: [tag=4][u64 indexedFrame][u32 stage][u8 netplayState][u8 isTraining]
//       [u8 chara0][u8 chara1][u8 moon0][u8 moon1][u8 color0][u8 color1]
// Size: 1 + 8 + 4 + 1 + 1 + 6 = 21 bytes

struct InitialGameState {
    IndexedFrame indexedFrame = {{0, 0}};
    uint32_t stage = 0;
    uint8_t netplayState = 0;
    uint8_t isTraining = 0;
    std::array<uint8_t, 2> chara = {{UNKNOWN_POSITION, UNKNOWN_POSITION}};
    std::array<uint8_t, 2> moon  = {{UNKNOWN_POSITION, UNKNOWN_POSITION}};
    std::array<uint8_t, 2> color = {{0, 0}};

    InitialGameState() = default;
    InitialGameState(IndexedFrame f, uint8_t state, bool training)
        : indexedFrame(f), netplayState(state), isTraining(training ? 1 : 0) {}

    // Read from game memory (defined in dll_hacks.cpp)
    void readFromGame(IndexedFrame f, uint8_t state, bool training);

    std::vector<uint8_t> serialize() const;
    static InitialGameState deserialize(const uint8_t* data, std::size_t len);
    static constexpr std::size_t wire_size() {
        return 1 + 8 + 4 + 1 + 1 + 6;
    }
};

// ---- NetplayConfig (match configuration) ---------------------------------
// Wire: [tag=5][ClientMode 2B][u8 delay][u8 rollback][u8 rollbackDelay]
//       [u8 winCount][u8 hostPlayer][u16 broadcastPort]
//       [u8 name0Len][name0][u8 name1Len][name1]
//       [u8 sessionIdLen][sessionId]
// Size: variable (2+1+1+1+1+1+2 + names + sessionId + 3 length bytes)

struct NetplayConfigMsg {
    ClientMode mode;
    uint8_t delay = 0xFF;
    uint8_t rollback = 0;
    uint8_t rollbackDelay = 0;
    uint8_t winCount = 2;
    uint8_t hostPlayer = 0;
    uint16_t broadcastPort = 0;
    std::array<std::string, 2> names;
    std::string sessionId;

    void setNames(const std::string& localName, const std::string& remoteName) {
        if (mode.isHost()) {
            names[hostPlayer - 1] = localName;
            names[2 - hostPlayer] = remoteName;
        } else {
            names[hostPlayer - 1] = remoteName;
            names[2 - hostPlayer] = localName;
        }
    }

    void clear() {
        mode.clear();
        delay = 0xFF;
        rollback = rollbackDelay = hostPlayer = 0;
        winCount = 2;
        broadcastPort = 0;
        names[0].clear();
        names[1].clear();
        sessionId.clear();
    }

    std::vector<uint8_t> serialize() const;
    static NetplayConfigMsg deserialize(const uint8_t* data, std::size_t len);
};

// ---- ConfirmConfig (empty — just a tag) ----------------------------------

struct ConfirmConfig {
    std::vector<uint8_t> serialize() const {
        return {static_cast<uint8_t>(MsgType::ConfirmConfig)};
    }
    static ConfirmConfig deserialize(const uint8_t*, std::size_t) {
        return ConfirmConfig{};
    }
    static constexpr std::size_t wire_size() { return 1; }
};

// ---- SyncHash (xxHash128 + game state snapshot for desync detection) ------
// Wire: [tag=7][u64 indexedFrame][16 bytes hash][u32 roundTimer][u32 realTimer]
//       [i32 cameraX][i32 cameraY]
//       [CharaHash×2: u32 seq, u32 seqState, u32 health, u32 redHealth,
//        u32 meter, u32 heat, f32 guardBar, f32 guardQuality,
//        i32 x, i32 y, u16 chara, u16 moon]
// Size: 1 + 8 + 16 + 4 + 4 + 4 + 4 + 2*(4+4+4+4+4+4+4+4+4+4+2+2) = 145 bytes

struct SyncHash {
    struct CharaHash {
        uint32_t seq = 0;
        uint32_t seqState = 0;
        uint32_t health = 0;
        uint32_t redHealth = 0;
        uint32_t meter = 0;
        uint32_t heat = 0;
        float    guardBar = 0.0f;
        float    guardQuality = 0.0f;
        int32_t  x = 0;
        int32_t  y = 0;
        uint16_t chara = 0;
        uint16_t moon = 0;
    };

    IndexedFrame indexedFrame = {{0, 0}};
    std::array<uint8_t, 16> hash{};
    uint32_t roundTimer = 0;
    uint32_t realTimer = 0;
    int32_t  cameraX = 0;
    int32_t  cameraY = 0;
    std::array<CharaHash, 2> chara{};

    SyncHash() = default;
    explicit SyncHash(IndexedFrame f) : indexedFrame(f) {}

    // Read from game memory (defined in dll_hacks.cpp)
    void readFromGame(IndexedFrame f);

    bool operator==(const SyncHash& other) const;
    bool operator!=(const SyncHash& other) const { return !(*this == other); }

    std::vector<uint8_t> serialize() const;
    static SyncHash deserialize(const uint8_t* data, std::size_t len);
    static constexpr std::size_t wire_size() {
        return 1 + 8 + 16 + 4 + 4 + 4 + 4 + 2 * sizeof(CharaHash);
    }
};

// ---- MenuIndex (retry menu navigation sync) ------------------------------
// Wire: [tag=8][u32 index][i8 menuIndex]
// Size: 1 + 4 + 1 = 6 bytes

struct MenuIndex {
    uint32_t index = 0;
    int8_t   menuIndex = -1;

    MenuIndex() = default;
    MenuIndex(uint32_t idx, int8_t menu) : index(idx), menuIndex(menu) {}

    std::vector<uint8_t> serialize() const;
    static MenuIndex deserialize(const uint8_t* data, std::size_t len);
    static constexpr std::size_t wire_size() { return 1 + 4 + 1; }
};

// ---- ChangeConfig (delay/rollback change mid-match) ----------------------
// Wire: [tag=9][u8 value][u64 indexedFrame][u8 delay][u8 rollbackDelay][u8 rollback]
// Size: 1 + 1 + 8 + 1 + 1 + 1 = 13 bytes

struct ChangeConfig {
    enum class Type : uint8_t { Delay = 1, Rollback = 2, RollbackDelay = 3 };
    Type    value = Type::Delay;
    IndexedFrame indexedFrame = {{0, 0}};
    uint8_t delay = 0xFF;
    uint8_t rollbackDelay = 0;
    uint8_t rollback = 0;

    uint8_t getOffset() const {
        if (delay < rollback) return 0;
        return delay - rollback;
    }

    std::vector<uint8_t> serialize() const;
    static ChangeConfig deserialize(const uint8_t* data, std::size_t len);
    static constexpr std::size_t wire_size() { return 1 + 1 + 8 + 1 + 1 + 1; }
};

// ---- TransitionIndex (state transition trigger) --------------------------
// Wire: [tag=10][u32 index]
// Size: 1 + 4 = 5 bytes

struct TransitionIndex {
    uint32_t index = 0;

    TransitionIndex() = default;
    explicit TransitionIndex(uint32_t idx) : index(idx) {}

    std::vector<uint8_t> serialize() const;
    static TransitionIndex deserialize(const uint8_t* data, std::size_t len);
    static constexpr std::size_t wire_size() { return 1 + 4; }
};

// ---- Helper: read/write little-endian primitives -------------------------

inline void write_u16_le(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

inline void write_u32_le(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

inline void write_u64_le(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}

inline void write_i32_le(std::vector<uint8_t>& out, int32_t v) {
    write_u32_le(out, static_cast<uint32_t>(v));
}

inline void write_f32_le(std::vector<uint8_t>& out, float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    write_u32_le(out, u);
}

inline void write_string_lp(std::vector<uint8_t>& out, const std::string& s) {
    // Length-prefixed string: [u8 len][bytes]. Max 255.
    uint8_t len = static_cast<uint8_t>(std::min<std::size_t>(s.size(), 255));
    out.push_back(len);
    out.insert(out.end(), s.begin(), s.begin() + len);
}

inline uint16_t read_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t read_u64_le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(p[i]) << (i * 8);
    return v;
}

inline int32_t read_i32_le(const uint8_t* p) {
    return static_cast<int32_t>(read_u32_le(p));
}

inline float read_f32_le(const uint8_t* p) {
    uint32_t u = read_u32_le(p);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

inline std::string read_string_lp(const uint8_t*& p, std::size_t& remaining) {
    if (remaining < 1) return {};
    uint8_t len = *p++;
    --remaining;
    if (len > remaining) len = static_cast<uint8_t>(remaining);
    std::string s(reinterpret_cast<const char*>(p), len);
    p += len;
    remaining -= len;
    return s;
}

} // namespace caster::dll
