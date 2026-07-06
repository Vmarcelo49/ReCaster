// src/dll/rollback_manager.hpp
// Ported from CCCaster DllRollbackManager. Removed SFX history (v1 accepts
// audio glitch during reroll — see phase-f-execution-plan.md §2.3d).
// Uses buildRollbackAddresses() instead of binary_res_rollback_bin.
//
// F.5 changes:
//   - saveState/loadState now take NetplayManager& (matching CCCaster),
//     so loadState can write back _state/_startWorldTime/_indexedFrame
//     directly (the FSM must resume from the restored frame, not the
//     current one).
//   - saveState eviction strategy matches CCCaster: when the pool is
//     full, don't always drop the front — keep it if it's still within
//     the remote's rollback window (front().indexedFrame.parts.frame <=
//     getRemoteFrame), and drop the second-oldest instead.
//   - loadState has the RELEASE fallback: if no saved state is <= target,
//     force-load the front state (better than returning false and leaving
//     the game in a corrupted state).
//   - loadState does the RepInputContainer fixup: decrements the game's
//     internal replay struct frame counts for each rolled-back frame,
//     so the game's replay system doesn't desync from the netplay state.
//   - Declared `friend` in NetplayManager so it can access _state,
//     _startWorldTime, _indexedFrame.

#pragma once

#include "constants.hpp"
#include "mem_dump.hpp"
#include "rollback_addresses.hpp"
#include "netplay_states.hpp"

#include <cfenv>
#include <cstdint>
#include <list>
#include <memory>
#include <stack>

namespace caster::dll {

class NetplayManager;

// ---- Game-side replay structs (for RepInputContainer fixup) ----
//
// These are packed structs that mirror the game's internal replay input
// storage. During rollback, we must decrement the frame counts in these
// structs for each rolled-back frame — otherwise the game's replay
// system would have more frames recorded than actually happened, causing
// desyncs when the replay is saved or when the game reads back from it.
//
// Ported from CCCaster's DllRollbackManager.hpp:12-36.

struct __attribute__((packed)) RepInputState {
    char unk1;
    char frameCount;
    char unk2[6];
};

struct __attribute__((packed)) RepInputContainer {
    char           unk1[4];
    RepInputState* states;
    char*          statesEnd;
    char           unk2[4];
    int            totalFrameCount;
    int            totalFrameCount2;
    int            activeIndex;
    char           unk3[4];
};

struct __attribute__((packed)) RepRound {
    char               unk1[0x120];
    RepInputContainer* inputs;      // points to an array of 4 input structs
    char               unk2[0x1C];
};

class RollbackManager {
public:
    struct GameState {
        NetplayState netplayState = NetplayState::PreInitial;
        uint32_t startWorldTime = 0;
        IndexedFrame indexedFrame = {{0, 0}};
        std::fenv_t fpEnv{};
        char* rawBytes = nullptr;

        void save(const MemDumpList& addrs);
        void load(const MemDumpList& addrs);
    };

    RollbackManager() = default;
    ~RollbackManager() { deallocateStates(); }

    void allocateStates();
    void deallocateStates();

    // Save the current game state + NetplayManager state. Called every
    // frame during InGame (when rollback is enabled).
    void saveState(const NetplayManager& netMan);

    // Load the newest saved state that is <= target. Updates the
    // NetplayManager's _state/_startWorldTime/_indexedFrame to match
    // the restored state. Also does the RepInputContainer fixup for
    // each rolled-back frame. Returns false if no state could be
    // loaded (only happens if _statesList is empty).
    bool loadState(IndexedFrame target, NetplayManager& netMan);

    bool hasStates() const { return !_statesList.empty(); }
    size_t numStates() const { return _statesList.size(); }

private:
    std::shared_ptr<char> _memoryPool;
    std::stack<size_t> _freeStack;
    std::list<GameState> _statesList;
    MemDumpList _allAddrs;
    size_t _stateSize = 0;
    bool _allocated = false;
};

} // namespace caster::dll
