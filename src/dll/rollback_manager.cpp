// src/dll/rollback_manager.cpp

#include "rollback_manager.hpp"
#include "../common/logger.hpp"

#include <cstring>
#include <algorithm>

namespace caster::dll {

void RollbackManager::GameState::save(const MemDumpList& addrs) {
    char* dump = rawBytes;
    for (const auto& mem : addrs.addrs)
        mem.saveDump(dump);
}

void RollbackManager::GameState::load(const MemDumpList& addrs) {
    const char* dump = rawBytes;
    for (const auto& mem : addrs.addrs)
        mem.loadDump(dump);
}

void RollbackManager::allocateStates() {
    if (_allocated) return;

    _allAddrs = buildRollbackAddresses();
    _stateSize = _allAddrs.totalSize;

    if (_stateSize == 0) {
        caster::common::logger::err("rollback: totalSize is 0!");
        return;
    }

    caster::common::logger::info("rollback: state size = {} bytes, allocating {} states",
                                 _stateSize, NUM_ROLLBACK_STATES);

    _memoryPool = std::shared_ptr<char>(
        new char[_stateSize * NUM_ROLLBACK_STATES],
        std::default_delete<char[]>());

    for (int i = 0; i < NUM_ROLLBACK_STATES; ++i)
        _freeStack.push(i);

    _allocated = true;
}

void RollbackManager::deallocateStates() {
    _memoryPool.reset();
    while (!_freeStack.empty()) _freeStack.pop();
    _statesList.clear();
    _allAddrs.clear();
    _stateSize = 0;
    _allocated = false;
}

void RollbackManager::saveState(NetplayState state, uint32_t worldTime,
                                 IndexedFrame indexedFrame) {
    if (!_allocated || _freeStack.empty()) {
        // Discard oldest state
        if (!_statesList.empty()) {
            GameState& oldest = _statesList.front();
            if (oldest.rawBytes) {
                size_t offset = (oldest.rawBytes - _memoryPool.get()) / _stateSize;
                _freeStack.push(offset);
            }
            _statesList.pop_front();
        }
        if (_freeStack.empty()) return;
    }

    size_t offset = _freeStack.top();
    _freeStack.pop();

    GameState gs;
    gs.netplayState = state;
    gs.startWorldTime = worldTime;
    gs.indexedFrame = indexedFrame;
    gs.rawBytes = _memoryPool.get() + offset * _stateSize;

    std::fegetenv(&gs.fpEnv);
    gs.save(_allAddrs);

    _statesList.push_back(std::move(gs));
}

bool RollbackManager::loadState(IndexedFrame target, NetplayState& outState,
                                 uint32_t& outWorldTime, IndexedFrame& outIndexedFrame) {
    if (!_allocated || _statesList.empty()) return false;

    // Find the newest state that is <= target
    auto it = _statesList.end();
    while (it != _statesList.begin()) {
        --it;
        if (it->indexedFrame <= target) {
            // Found it — load this state
            outState = it->netplayState;
            outWorldTime = it->startWorldTime;
            outIndexedFrame = it->indexedFrame;

            std::fesetenv(&it->fpEnv);
            it->load(_allAddrs);

            // Discard all states after this one
            auto discardStart = it;
            ++discardStart;
            for (auto dit = discardStart; dit != _statesList.end(); ++dit) {
                if (dit->rawBytes) {
                    size_t offset = (dit->rawBytes - _memoryPool.get()) / _stateSize;
                    _freeStack.push(offset);
                }
            }
            _statesList.erase(discardStart, _statesList.end());

            return true;
        }
    }

    return false;
}

} // namespace caster::dll
