// src/dll/rollback_manager.hpp
// Ported from CCCaster DllRollbackManager. Removed SFX history.
// Uses buildRollbackAddresses() instead of binary_res_rollback_bin.

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

    void saveState(NetplayState state, uint32_t worldTime, IndexedFrame indexedFrame);
    bool loadState(IndexedFrame target, NetplayState& outState, uint32_t& outWorldTime, IndexedFrame& outIndexedFrame);

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
