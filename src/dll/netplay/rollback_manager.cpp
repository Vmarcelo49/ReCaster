// src/dll/netplay/rollback_manager.cpp
//
// F.5 implementation — faithful to CCCaster's DllRollbackManager.cpp.
// SFX history (saveRerunSounds/finishedRerunSounds) restored.
// Uses buildRollbackAddresses() instead of binary_res_rollback_bin.
// Takes NetplayManager& so loadState can write back FSM state.

#include "rollback_manager.hpp"
#include "manager.hpp"
#include "thread_affinity.hpp"
#include "game/addresses.hpp"
#include "../hooks/asm_patches.hpp"
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
#include <mutex>

namespace caster::dll {

void RollbackManager::GameState::save(const MemDumpList& addrs) {
    char* dump = rawBytes;
    // Roots get nullptr as parentAddr — MemDump::getAddr ignores it
    // and returns `addr`. Children then receive the freshly-computed
    // root address threaded down through saveDump.
    for (const auto& mem : addrs.addrs)
        mem.saveDump(dump, nullptr);
}

void RollbackManager::GameState::load(const MemDumpList& addrs) {
    const char* dump = rawBytes;
    for (const auto& mem : addrs.addrs)
        mem.loadDump(dump, nullptr);
}

void RollbackManager::allocateStates() {
    if (_allocated) return;

    // Defense-in-depth: clear any leftover states from a previous match.
    // deallocateStates() should have already cleared _statesList when we
    // left the previous InGame, but CCCaster's allocateStates() also
    // clears (DllRollbackManager.cpp:66) as a safety net against partial
    // teardown. Without this, a stale state could survive and be loaded
    // by a rollback in the new match — instant desync.
    _statesList.clear();

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

    // Clear SFX history — matches CCCaster DllRollbackManager.cpp:68-69.
    for (auto& sfxArray : _sfxHistory)
        std::memset(&sfxArray[0], 0, CC_SFX_ARRAY_LEN);

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

    // Acquire the NetplayManager mutex for the duration of the save.
    // We read netMan._state, _startWorldTime, _indexedFrame directly
    // (friend access), which races with the network thread's
    // setInputs()/setRngState() that may mutate _indexedFrame indirectly
    // via setState. The lock is held for the entire save to ensure a
    // consistent snapshot.
    //
    // Note: netMan is const&, but _mutex is mutable, so this locks fine.
    std::lock_guard<std::mutex> netManLock(netMan._mutex); SCOPED_NETMAN_MUTEX_HELD();

    if (_freeStack.empty()) {
        // Pool full — evict.
        if (!_statesList.empty()) {
            auto it = _statesList.begin();
            ++it;  // second element
            if (it != _statesList.end() &&
                _statesList.front().indexedFrame.parts.frame <= netMan.getRemoteFrameLocked()) {
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

    // saveState was previously disabled by a `s_saveDisabled = true`
    // guard. The crash that motivated the guard was a dangling `parent`
    // pointer in MemDumpPtr: after MemDumpList::update() sorted and
    // merged the root MemDump vector, the child MemDumpPtr nodes still
    // held `parent` back-pointers into the pre-reallocation vector
    // buffer. saveDump then dereferenced freed memory, most visibly
    // when walking the effects array (1000 elements × 3-level pointer
    // chasing at offset 0x320+0x38) during early InGame.
    //
    // The fix is in mem_dump.{hpp,cpp}: the `parent` field is gone,
    // and the parent address is now threaded through getAddr/saveDump/
    // loadDump as an explicit parameter. This makes every node safe to
    // relocate inside its vector, so saveState can be re-enabled.
    gs.save(_allAddrs);

    // Save SFX filter state — matches CCCaster DllRollbackManager.cpp:119-120.
    // Stores the current sfxFilterArray into _sfxHistory[frame % NUM_ROLLBACK_STATES]
    // so loadState can later determine which sounds were played between the
    // rollback target and the current frame.
    {
        uint8_t* currentSfxArray = &_sfxHistory[netMan.getFrameLocked() % NUM_ROLLBACK_STATES][0];
        std::memcpy(currentSfxArray, asm_hacks::sfxFilterArray, CC_SFX_ARRAY_LEN);
    }

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

    // Acquire the NetplayManager mutex for the duration of the load.
    // We write netMan._state, _startWorldTime, _indexedFrame directly
    // (friend access), which races with the network thread's reads via
    // getIndex()/getFrame()/getState() inside setInputs()/setRngState().
    // The lock is held for the entire load + fixup to ensure the FSM
    // state is consistent when we return.
    std::lock_guard<std::mutex> netManLock(netMan._mutex); SCOPED_NETMAN_MUTEX_HELD();

    caster::common::logger::info(
        "rollback: loadState target=[idx={},frame={}] states=[{}..{}]",
        target.parts.index, target.parts.frame,
        _statesList.front().indexedFrame.parts.frame,
        _statesList.back().indexedFrame.parts.frame);

    const uint32_t origFrame = netMan.getFrameLocked();

    for (auto it = _statesList.rbegin(); it != _statesList.rend(); ++it) {
        if (it->indexedFrame.value <= target.value || &(*it) == &_statesList.front()) {
            // Found a state to load (either <= target, or the front
            // as a RELEASE fallback).
            //
            // ORDER MATCHES CCCASTER (DllRollbackManager.cpp:147-150):
            //   1. Update netMan._state / _startWorldTime / _indexedFrame
            //   2. Call it->load() which does fesetenv + loadDump
            //
            // The previous ReCaster order (load FIRST, then netMan update)
            // was a divergence from CCCaster. While it shouldn't matter
            // in theory (the load writes game memory, the netMan update
            // writes NetplayManager fields — they don't overlap), matching
            // CCCaster's order eliminates one variable when chasing
            // desyncs that depend on subtle ordering.
            netMan._state = it->netplayState;
            netMan._startWorldTime = it->startWorldTime;
            netMan._indexedFrame = it->indexedFrame;

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
                netMan.getIndexLocked(), netMan.getFrameLocked(), origFrame);

            // 5. SFX filter restore — matches CCCaster DllRollbackManager.cpp:207-222.
            //
            // OR all SFX flags from the rolled-back range (target+1..origFrame)
            // into the current sfxFilterArray. This marks all sounds that were
            // played during the rolled-back frames as "already played" so the
            // rerun doesn't re-trigger them.
            //
            // Then set all non-zero entries to 0x80 (the "played but filtered"
            // flag). The SFX ASM hook treats 0x80 as "sound was played before
            // rollback but may or may not play during rerun". If the sound
            // plays during rerun, the hook increments it past 0x80 (marking
            // it as "confirmed played during rerun"). If it stays at 0x80
            // after rerun, finishedRerunSounds() will cancel it (play muted).
            for (uint32_t i = netMan.getFrameLocked() + 1; i < origFrame; ++i) {
                for (uint32_t j = 0; j < CC_SFX_ARRAY_LEN; ++j)
                    asm_hacks::sfxFilterArray[j] |= _sfxHistory[i % NUM_ROLLBACK_STATES][j];
            }
            for (uint32_t j = 0; j < CC_SFX_ARRAY_LEN; ++j) {
                if (asm_hacks::sfxFilterArray[j])
                    asm_hacks::sfxFilterArray[j] = 0x80;
            }

            return true;
        }
    }

    caster::common::logger::warn("rollback: loadState failed — no state <= target");
    return false;
}

// ============================================================================
// SFX history — CCCaster DllRollbackManager.cpp:232-262
// ============================================================================

void RollbackManager::saveRerunSounds(uint32_t frame) {
    // During rollback rerun, record which SFX actually played. The SFX
    // ASM hook increments sfxFilterArray entries when a sound plays.
    // We record the current state so finishedRerunSounds() can later
    // determine which sounds were played during the rerun vs. which
    // were only played before the rollback (stale).
    uint8_t* currentSfxArray = &_sfxHistory[frame % NUM_ROLLBACK_STATES][0];

    for (uint32_t j = 0; j < CC_SFX_ARRAY_LEN; ++j) {
        // If the SFX has a non-zero filter value that isn't 0x80
        // (the "played before rollback" marker), it played during rerun.
        if (asm_hacks::sfxFilterArray[j] & ~0x80)
            currentSfxArray[j] = 1;
        else
            currentSfxArray[j] = 0;
    }
}

void RollbackManager::finishedRerunSounds() {
    // After rollback rerun completes, cancel unplayed sound effects.
    //
    // Sounds with filter value == 0x80 were played before the rollback
    // but did NOT play during the rerun (their value wasn't incremented
    // past 0x80 by the ASM hook). We cancel them by:
    //   1. Setting CC_SFX_ARRAY_ADDR[j] = 1 (trigger the sound)
    //   2. Setting sfxMuteArray[j] = 1 (play it muted)
    // The next time the game's audio loop processes the SFX array, it
    // will "play" the sound muted, effectively cancelling the stale
    // trigger without producing audible output.
    for (uint32_t j = 0; j < CC_SFX_ARRAY_LEN; ++j) {
        if (asm_hacks::sfxFilterArray[j] == 0x80) {
            // Play the SFX muted to cancel it
            reinterpret_cast<uint8_t*>(CC_SFX_ARRAY_ADDR)[j] = 1;
            asm_hacks::sfxMuteArray[j] = 1;
        }
    }

    // Clear the SFX filter for the next frame.
    std::memset(asm_hacks::sfxFilterArray, 0, CC_SFX_ARRAY_LEN);
}

} // namespace caster::dll
