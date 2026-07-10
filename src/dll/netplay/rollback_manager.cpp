// src/dll/netplay/rollback_manager.cpp
//
// F.5 implementation — faithful to CCCaster's DllRollbackManager.cpp
// with the following adaptations:
//   - No SFX history (saveRerunSounds/finishedRerunSounds removed).
//     v1 accepts audio glitch during reroll. See docs/port-status.md (blockers da Fase F).
//   - Uses buildRollbackAddresses() instead of binary_res_rollback_bin.
//   - Takes NetplayManager& so loadState can write back FSM state.

#include "rollback_manager.hpp"
#include "manager.hpp"
#include "game/addresses.hpp"
#include "../common/logger.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstring>

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
        _freeStack.push(i * _stateSize);

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

// ============================================================================
// saveState — eviction strategy matching CCCaster DllRollbackManager.cpp:84
// ============================================================================
//
// When the pool is full (_freeStack empty), CCCaster doesn't always drop
// the front (oldest) state. Instead:
//   - If the front state's frame is still within the remote's rollback
//     window (front.frame <= getRemoteFrame), keep the front and drop
//     the SECOND state instead. This is because the front might still
//     be a valid rollback target if the peer is behind.
//   - Otherwise, drop the front (it's too old to be useful).
//
// The ReCaster's previous implementation always dropped the front,
// which could discard a state that was still a valid rollback target,
// causing loadState to fail silently.

void RollbackManager::saveState(const NetplayManager& netMan) {
    if (!_allocated) return;

    if (_freeStack.empty()) {
        // Pool full — evict.
        if (!_statesList.empty()) {
            auto it = _statesList.begin();
            ++it;  // second element
            if (it != _statesList.end() &&
                _statesList.front().indexedFrame.parts.frame <= netMan.getRemoteFrame()) {
                // Front is still within remote's window — drop second instead.
                _freeStack.push(it->rawBytes - _memoryPool.get());
                _statesList.erase(it);
            } else {
                // Front is too old — drop it.
                _freeStack.push(_statesList.front().rawBytes - _memoryPool.get());
                _statesList.pop_front();
            }
        }
        if (_freeStack.empty()) return;
    }

    const size_t offset = _freeStack.top();
    _freeStack.pop();

    GameState gs;
    gs.netplayState = netMan._state;
    gs.startWorldTime = netMan._startWorldTime;
    gs.indexedFrame = netMan._indexedFrame;
    gs.rawBytes = _memoryPool.get() + offset;

    std::fegetenv(&gs.fpEnv);

    // saveState is temporarily disabled — crash on Wine 10.0 caused by
    // MemDumpPtr children following pointers into uninitialized game heap
    // during early InGame. VirtualQuery and IsBadReadPtr both failed to
    // prevent the crash reliably on Wine. The crash was identified by
    // crash cursor logging: it happens in the effects array (1000 elements
    // × 3-level pointer chasing at offset 0x320+0x38).
    //
    // TODO: implement a safe pointer validation that works on all Wine
    // versions, or identify which specific effect elements have bad
    // pointers and skip them individually.
    static bool s_saveDisabled = true;
    if (s_saveDisabled) {
        _freeStack.push(offset);
        return;
    }

    gs.save(_allAddrs);

    _statesList.push_back(std::move(gs));
}

// ============================================================================
// loadState — CCCaster DllRollbackManager.cpp:123
// ============================================================================
//
// Iterates _statesList in reverse (newest first) and loads the first
// state with indexedFrame <= target. In RELEASE, if no state matches,
// force-loads the front (oldest) state as a fallback — better than
// returning false and leaving the game in a corrupted state.
//
// After loading:
//   1. Updates netMan._state, _startWorldTime, _indexedFrame to the
//      saved values (the FSM must resume from the restored frame).
//   2. Erases all states after the loaded one (they're now invalid).
//   3. RepInputContainer fixup: for each rolled-back frame, decrements
//      the game's internal replay struct frame counts so the replay
//      system stays in sync with the netplay state.

bool RollbackManager::loadState(IndexedFrame target, NetplayManager& netMan) {
    if (!_allocated || _statesList.empty()) return false;

    caster::common::logger::info(
        "rollback: loadState target=[idx={},frame={}] states=[{}..{}]",
        target.parts.index, target.parts.frame,
        _statesList.front().indexedFrame.parts.frame,
        _statesList.back().indexedFrame.parts.frame);

    const uint32_t origFrame = netMan.getFrame();

    for (auto it = _statesList.rbegin(); it != _statesList.rend(); ++it) {
        if (it->indexedFrame.value <= target.value || &(*it) == &_statesList.front()) {
            // Found a state to load (either <= target, or the front
            // as a RELEASE fallback).

            // 1. Update NetplayManager state to match the saved state.
            netMan._state = it->netplayState;
            netMan._startWorldTime = it->startWorldTime;
            netMan._indexedFrame = it->indexedFrame;

            // 2. Load the raw game memory from the saved state.
            std::fesetenv(&it->fpEnv);
            it->load(_allAddrs);

            // Count rolled-back frames for the RepInputContainer fixup.
            int rbFrames = 0;
            if (!netMan.config.mode.isTraining()) {
                rbFrames = static_cast<int>(
                    _statesList.back().indexedFrame.value - it->indexedFrame.value);
                caster::common::logger::info("rollback: rolled back {} frames", rbFrames);
            }

            // 3. Erase all states after the loaded one.
            //    it.base() points one past the reverse iterator, which
            //    is the first state AFTER the loaded one in forward order.
            for (auto jt = it.base(); jt != _statesList.end(); ++jt) {
                _freeStack.push(jt->rawBytes - _memoryPool.get());
            }
            _statesList.erase(it.base(), _statesList.end());

            // 4. RepInputContainer fixup.
            //    For each rolled-back frame, decrement the frame count
            //    in the game's internal replay struct. Without this,
            //    the replay would have more frames recorded than
            //    actually happened, causing desyncs when the replay
            //    is saved or read back.
            if (!netMan.config.mode.isTraining() && rbFrames > 0) {
                for (; rbFrames > 0; --rbFrames) {
                    // CC_REPROUND_TBL_ENDPTR_ADDR points to the end of
                    // the replay round table. The current round is at
                    // (endptr - 1).
                    auto* endPtr = *reinterpret_cast<RepRound**>(
                        const_cast<std::uintptr_t&>(CC_REPROUND_TBL_ENDPTR_ADDR));
                    if (!endPtr) {
                        caster::common::logger::warn("rollback: missing replay table");
                        break;
                    }
                    RepRound* curRound = endPtr - 1;
                    if (!curRound->inputs) {
                        caster::common::logger::warn("rollback: missing replay inputs");
                        break;
                    }
                    // The game stores containers for 4 players (even
                    // in 2-player mode). Decrement each one.
                    for (int i = 0; i < 4; ++i) {
                        RepInputContainer* inputs = &curRound->inputs[i];
                        if (!inputs->states) continue;
                        RepInputState* state = &inputs->states[inputs->activeIndex];
                        if (!state->frameCount) continue;
                        if (state->frameCount == 1) {
                            // Last frame in this state — clear it and
                            // decrement the active index.
                            std::memset(state, 0, sizeof(RepInputState));
                            inputs->statesEnd -= sizeof(RepInputState);
                            if (inputs->activeIndex > 0)
                                --inputs->activeIndex;
                        } else {
                            --state->frameCount;
                        }
                    }
                }
            }

            caster::common::logger::info(
                "rollback: loaded state [idx={},frame={}] (orig frame={})",
                netMan.getIndex(), netMan.getFrame(), origFrame);

            return true;
        }
    }

    caster::common::logger::warn("rollback: loadState failed — no state <= target");
    return false;
}

} // namespace caster::dll
