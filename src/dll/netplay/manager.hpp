// src/dll/netplay/manager.hpp
//
// Ported from CCCaster's targets/DllNetplayManager.{hpp,cpp}.
//
// The NetplayManager is the brain of the DLL-side netplay engine. It owns:
//   - The NetplayState FSM (PreInitial → Initial → CharaSelect → Loading →
//     CharaIntro → InGame → RetryMenu → ...)
//   - The per-player InputsContainer (indexed by transition index, then frame)
//   - The RngState history (one per transition index, used for desync
//     prevention between peers)
//   - The retry-menu-index history (used to synchronize retry menu
//     selections between peers)
//   - The current IndexedFrame (transition index + frame-within-transition)
//
// Each frame, the DLL's frameStep() calls netMan.updateFrame() to refresh
// _indexedFrame from CC_WORLD_TIMER_ADDR, then calls
// netMan.getInput(localPlayer) and netMan.getInput(remotePlayer) to get
// the inputs to write to the game. The NetplayManager synthesizes
// appropriate inputs per NetplayState — for example, mashing Confirm
// through the Startup/Opening/Title screens in getPreInitialInput, and
// auto-navigating the chara-select screen in getAutoCharaSelectInput.
//
// Adaptations from CCCaster:
//   - Removed: trial mode (getDemoInput, _demoCountdown, _exitCountdown,
//     TrialManager hooks in getInGameInput), replay code (exportInputs,
//     exportResults, getInGameIndexes, inGameIndexes[30], _roundRngStates,
//     autoReplaySave), UDP debug logger (LOG() calls replaced with
//     caster::common::logger), spectate/broadcast mode branches in
//     getRetryMenuInput (spectate is cut from v1), splitDelay (kept
//     simple — same delay for both players), training-reset state
//     machine in getInGameInput (was coupled to TrialManager).
//   - Replaced: MsgPtr (cereal-based smart pointer) with std::shared_ptr<T>
//     for RngState storage. PlayerInputs/BothInputs return values are
//     returned by value (or std::optional when nullability is meaningful).
//   - Kept: every other method 1:1 with CCCaster, including the exact
//     semantics of setState (which manages _startIndex/_spectateStartIndex/
//     _startIndex garbage-collection on Loading transitions), the exact
//     menu-navigation state machine in getMenuNavInput (state 0→1→2→...
//     →39), and the exact hasUpDownInHistory/hasButtonInHistory/
//     heldButtonInHistory helpers.

#pragma once

#include "game/addresses.hpp"
#include "protocol/messages.hpp"
#include "inputs_container.hpp"
#include "states.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace caster::dll {

// Forward declare for friend access in NetplayManager.
class RollbackManager;

// Combine direction (low nibble) and buttons (high bits) into the
// packed uint16_t format used by the InputsContainer. Matches the
// CCCaster COMBINE_INPUT macro: (direction) | (buttons << 4).
inline constexpr uint16_t COMBINE_INPUT(uint16_t direction, uint16_t buttons) {
    return static_cast<uint16_t>((direction & 0x000F) | ((buttons & 0x0FFF) << 4));
}

class NetplayManager {
public:
    // Netplay config (received from the launcher via IPC).
    NetplayConfigMsg config;

    // Initial game state (chara/moon/color/stage snapshot — used by
    // getAutoCharaSelectInput to write the chosen characters into the
    // game's chara-select memory directly, bypassing the player having
    // to navigate the cursor).
    InitialGameState initial;

    // Preserve input/RngState/MenuIndex starting from this index. Used
    // by the SpectatorManager (which we don't have in v1, but the field
    // is kept because setState() reads it during Loading transitions to
    // decide how much history to garbage-collect). UINT_MAX = no
    // preservation constraint.
    uint32_t preserveStartIndex = UINT32_MAX;

    // The number of frames it takes to register a held start button
    // input. Used by getInGameInput to prevent accidental pausing in
    // versus mode (the player must hold Start for this many frames
    // before the pause registers).
    uint32_t heldStartDuration = 0;

    // Indicate which player is the remote player. Derives _localPlayer
    // and _remotePlayer (which are 1-indexed: 1 = P1, 2 = P2).
    void setRemotePlayer(uint8_t player);

    // Update _indexedFrame.parts.frame from CC_WORLD_TIMER_ADDR. Called
    // once per frame at the top of frameStep().
    void updateFrame();

    // ---- Frame / index accessors ----
    uint32_t getFrame() const { return _indexedFrame.parts.frame; }
    uint32_t getIndex() const { return _indexedFrame.parts.index; }
    IndexedFrame getIndexedFrame() const { return _indexedFrame; }
    uint32_t getRemoteIndex() const;
    uint32_t getRemoteFrame() const;
    IndexedFrame getRemoteIndexedFrame() const;

    // The delta between local and remote frames. Returns 0 if the
    // transition index differs (in that case the comparison is
    // meaningless). Used for the overlay debug display and for the
    // rollback pacing check.
    int getRemoteFrameDelta() const {
        if (getIndex() == getRemoteIndex())
            return static_cast<int>(getFrame()) -
                   static_cast<int>(getRemoteFrame() + config.delay - config.rollbackDelay);
        return 0;
    }

    // Get the index for spectators to start inputs on. During
    // CharaSelect this is the beginning of the current CharaSelect
    // state; during any other state it's the beginning of the current
    // game's Loading state. Kept for completeness (SpectatorManager is
    // not in v1 but the field is wired into setState's bookkeeping).
    uint32_t getSpectateStartIndex() const { return _spectateStartIndex; }

    // ---- Rollback trigger ----
    // getLastChangedFrame returns the earliest {index, frame} at which
    // a remote input arrived that disagreed with the locally-predicted
    // input. The rollback loop checks if this is < getIndexedFrame()
    // and, if so, rolls back to it. Returns MaxIndexedFrame if no
    // divergence has been detected since the last clearLastChangedFrame.
    IndexedFrame getLastChangedFrame() const;
    void clearLastChangedFrame();

    // ---- State accessors ----
    NetplayState getState() const { return _state; }
    void setState(NetplayState state);
    bool isInGame() const { return _state == NetplayState::InGame; }
    bool isInRollback() const {
        return isInGame() && config.rollback != 0 && config.mode.isNetplay();
    }

    // ---- Per-player input accessors ----
    // getInput returns the input that should be written to the game
    // for `player` this frame. This dispatches on _state to one of the
    // get<State>Input helpers below (which may synthesize menu-nav
    // inputs, filter out forbidden buttons, etc.).
    uint16_t getInput(uint8_t player);

    // getRawInput returns the stored input without any state-specific
    // filtering. Used internally by the get<State>Input helpers and by
    // the rollback rerun path.
    uint16_t getRawInput(uint8_t player) const {
        return getRawInput(player, getFrame());
    }
    uint16_t getRawInput(uint8_t player, uint32_t frame) const;

    // setInput stores the local player's input for the current frame
    // (offset by config.delay, or config.rollbackDelay if in rollback).
    void setInput(uint8_t player, uint16_t input);

    // assignInput overwrites an existing input — used by the rollback
    // rerun path to replace predicted inputs with the actual remote
    // inputs.
    void assignInput(uint8_t player, uint16_t input, uint32_t frame);
    void assignInput(uint8_t player, uint16_t input, IndexedFrame indexedFrame);

    // ---- Batch input accessors (for netplay wire protocol) ----
    // getInputs returns a PlayerInputs message containing the last
    // NUM_INPUTS frames of `player`'s input history. Sent to the peer
    // every frame in netplay mode. Returns std::nullopt if there are
    // no inputs yet.
    std::optional<PlayerInputs> getInputs(uint8_t player) const;

    // setInputs stores a batch of remote inputs received from the peer.
    // Drops the batch if it's older than the current transition index
    // (no point keeping stale remote inputs). The checkStartingFromIndex
    // parameter is forwarded to InputsContainer::set to enable
    // divergence detection (the rollback trigger).
    void setInputs(uint8_t player, const PlayerInputs& playerInputs);

    // getBothInputs returns a BothInputs message containing both
    // players' input history starting at `pos`. Used by the
    // SpectatorManager (not in v1, but the API is kept for completeness).
    // Advances `pos` by NUM_INPUTS on success. Returns std::nullopt if
    // not enough inputs are ready yet (the spectator must wait).
    std::optional<BothInputs> getBothInputs(IndexedFrame& pos) const;

    // setBothInputs stores a batch of both players' inputs — used in
    // spectate mode (not in v1, but kept for completeness).
    void setBothInputs(const BothInputs& bothInputs);

    // True if the remote input is ready for the current frame, i.e. we
    // have at least one input at or beyond (getIndex, getFrame). The
    // frameStep loop blocks on this before advancing.
    bool isRemoteInputReady() const;

    // ---- RngState ----
    std::shared_ptr<RngState> getRngState() const { return getRngState(getIndex()); }
    std::shared_ptr<RngState> getRngState(uint32_t index) const;
    void setRngState(const RngState& rngState);

    // True if the RngState is ready for the current frame. The host
    // generates RngStates and sends them to the client, so the client
    // must wait for the RngState to arrive before advancing.
    bool isRngStateReady(bool shouldSyncRngState) const;

    // ---- Retry menu index sync ----
    std::optional<MenuIndex> getLocalRetryMenuIndex() const;
    void setRemoteRetryMenuIndex(int8_t menuIndex);
    std::optional<MenuIndex> getRetryMenuIndex(uint32_t index) const;
    void setRetryMenuIndex(uint32_t index, int8_t menuIndex);

    // ---- Delay / rollback accessors ----
    uint8_t getDelay() const {
        return isInRollback() ? config.rollbackDelay : config.delay;
    }
    uint8_t getRollbackDelay() const { return config.rollbackDelay; }
    void setDelay(uint8_t delay) {
        if (isInRollback()) config.rollbackDelay = delay;
        else config.delay = delay;
    }
    void setRollbackDelay(uint8_t delay) { config.rollbackDelay = delay; }

    uint8_t getRollback() const { return config.rollback; }
    void setRollback(uint8_t rollback) { config.rollback = rollback; }

    // ---- Remote transition index ----
    void setRemoteIndex(uint32_t remoteIndex);

    // Check if the next state transition is valid (delegates to
    // isValidNextState in netplay_states.hpp).
    bool isValidNext(NetplayState state) const {
        return isValidNextState(_state, state);
    }

    // RollbackManager needs to read/write our private state during
    // loadState (it overwrites _state, _startWorldTime, _indexedFrame
    // with the saved values, and reads them during saveState).
    friend class RollbackManager;

private:
    // ---- Netplay state ----
    NetplayState _state = NetplayState::PreInitial;

    // ---- Menu navigation state machine ----
    // _targetMenuState / _targetIndex drive the auto-navigation in
    // getMenuNavInput(). State transitions:
    //   -1                    = no navigation pending
    //    0                    = target menu index determined, start moving
    //    1                    = issue up/down press to move cursor
    //    2..4                 = wait for currentMenuIndex to update
    //    5..38                = keep navigating (loop back to 1 if needed)
    //    39                   = mash Confirm to select the final option
    int32_t _targetMenuState = -1;
    int8_t  _targetMenuIndex = -1;

    // ---- Retry menu sync ----
    int8_t _localRetryMenuIndex = -1;
    int8_t _remoteRetryMenuIndex = -1;

    // The value of *CC_MENU_STATE_COUNTER_ADDR at the beginning of the
    // RetryMenu state. Used to detect if any other menus are open in
    // front of the retry menu (in which case we shouldn't auto-confirm).
    uint32_t _retryMenuStateCounter = 0;

    // ---- Frame tracking ----
    // The starting value of CC_WORLD_TIMER_ADDR for the current
    // transition index. frame = (*CC_WORLD_TIMER_ADDR) - _startWorldTime.
    // Reset whenever the NetplayState changes (see setState).
    uint32_t _startWorldTime = 0;

    // Current transition index (incremented on each state change past
    // CharaSelect) + current frame within this transition.
    IndexedFrame _indexedFrame = {{0, 0}};

    // The current starting index for the offset index data. Inputs,
    // RngStates, and retry-menu-indices are all stored relative to
    // _startIndex to allow garbage-collecting old transition indices.
    uint32_t _startIndex = 0;

    // The starting index for spectators (beginning of current
    // CharaSelect or current Loading).
    uint32_t _spectateStartIndex = 0;

    // ---- Input history ----
    // Mapping: player (0 or 1) → InputsContainer (indexed by
    // transition_index - _startIndex, then frame).
    std::array<InputsContainer<uint16_t>, 2> _inputs;

    // ---- RngState history ----
    // Mapping: transition_index - _startIndex → RngState (may be null
    // for indices we haven't received yet). Null entries are stored
    // explicitly because the host may send RngStates out of order.
    std::vector<std::shared_ptr<RngState>> _rngStates;

    // ---- Retry menu index history ----
    // Mapping: transition_index - _startIndex → menuIndex.
    // -1 means "no retry menu index for this transition yet".
    std::vector<int8_t> _retryMenuIndicies;

    // ---- Player slots ----
    // _localPlayer is the slot we control via setInput (1 or 2).
    // _remotePlayer is the slot the peer controls (1 or 2).
    uint8_t _localPlayer = 1;
    uint8_t _remotePlayer = 2;

    // ---- Per-state input generators ----
    uint16_t getPreInitialInput(uint8_t player);
    uint16_t getInitialInput(uint8_t player);
    uint16_t getAutoCharaSelectInput(uint8_t player);
    uint16_t getCharaSelectInput(uint8_t player);
    uint16_t getSkippableInput(uint8_t player);
    uint16_t getInGameInput(uint8_t player);
    uint16_t getRetryMenuInput(uint8_t player);
    uint16_t getReplayMenuInput(uint8_t player);

    // Auto-navigate the menu to _targetMenuIndex. Implements the
    // _targetMenuState state machine described above.
    uint16_t getMenuNavInput();

    // Detect if a key has been pressed / held by either player in the
    // input history. The start and end indices count backwards from
    // the current frame (0 = current frame, 1 = previous, etc.).
    // Used to suppress Confirm presses for a few frames after cursor
    // movement (workaround for the game's currentMenuIndex lag).
    bool hasUpDownInHistory(uint8_t player, uint32_t start, uint32_t end) const;
    bool hasButtonInHistory(uint8_t player, uint16_t button, uint32_t start, uint32_t end) const;
    bool heldButtonInHistory(uint8_t player, uint16_t button, uint32_t start, uint32_t end) const;

    // Compute the buffered preserveStartIndex (preserveStartIndex minus
    // a safety buffer for chained spectators).
    uint32_t getBufferedPreserveStartIndex() const;
};

} // namespace caster::dll
