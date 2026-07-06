// src/dll/netplay_manager.cpp
//
// Ported from CCCaster's targets/DllNetplayManager.cpp.
//
// See netplay_manager.hpp for the high-level overview of what was kept,
// removed, and adapted. The implementation below mirrors the CCCaster
// line-by-line for the kept portions — the NetplayState FSM is delicate
// enough that "improvements" here tend to introduce desyncs.
//
// All LOG() calls from the CCCaster (which broadcast over UDP to port
// 17474 for the sync-test tool) are replaced with caster::common::logger
// calls. The most verbose ones are downgraded to debug level to avoid
// flooding the log file in normal play.

#include "netplay_manager.hpp"
#include "asm_hacks.hpp"
#include "character_select.hpp"
#include "dll_process_manager.hpp"
#include "../common/logger.hpp"

#include <algorithm>
#include <cstdint>

namespace caster::dll {

// Max allowed retry menu index (chara select, save replay).
// Prevents returning to main menu (which would desync the FSM).
inline constexpr int8_t MAX_RETRY_MENU_INDEX = 2;

// Extra number to add to preserveStartIndex — safety buffer for
// chained spectators (spectator-of-spectator scenarios).
inline constexpr uint32_t PRESERVE_START_INDEX_BUFFER = 5;

// RETURN_MASH_INPUT — pulse the given input on even frames, return 0
// on odd frames. Used by getPreInitialInput / getInitialInput /
// getAutoCharaSelectInput to mash Confirm through the Startup/Opening/
// Title/Main screens.
#define RETURN_MASH_INPUT(DIRECTION, BUTTONS)                       \
    do {                                                            \
        if (getFrame() % 2)                                         \
            return 0;                                               \
        return COMBINE_INPUT((DIRECTION), (BUTTONS));               \
    } while (0)


// ============================================================================
// Per-state input generators
// ============================================================================

uint16_t NetplayManager::getPreInitialInput(uint8_t player) {
    // Drive the game past Startup → Opening → Title → Main by mashing
    // Confirm. menuConfirmState=2 tells hijackMenu to let our injected
    // confirms actually advance the menus.
    if ((*asU32(CC_GAME_MODE_ADDR)) == CC_GAME_MODE_MAIN)
        return 0;

    asm_hacks::menuConfirmState = 2;
    RETURN_MASH_INPUT(0, CC_BUTTON_CONFIRM);
}

uint16_t NetplayManager::getInitialInput(uint8_t player) {
    if ((*asU32(CC_GAME_MODE_ADDR)) != CC_GAME_MODE_MAIN)
        return getPreInitialInput(player);

    // The host player selects the main menu, so that the host controls
    // training mode selection.
    if (player != config.hostPlayer)
        return 0;

    // Wait until we know what game mode to go to (the IPC config will
    // fill in config.mode before we reach Initial).
    if (config.mode.value == ClientMode::Mode::Unknown)
        return 0;

    asm_hacks::menuConfirmState = 2;
    RETURN_MASH_INPUT(0, CC_BUTTON_CONFIRM);
}

uint16_t NetplayManager::getAutoCharaSelectInput(uint8_t player) {
    // Spectate-only state: write the initial chara/moon/color/stage
    // directly into the game's memory. This bypasses the player having
    // to navigate the cursor — the host already chose, and we're
    // catching up.
    *asU32(CC_P1_CHARA_SELECTOR_ADDR) = (uint32_t)charaToSelector(initial.chara[0]);
    *asU32(CC_P2_CHARA_SELECTOR_ADDR) = (uint32_t)charaToSelector(initial.chara[1]);

    *asU32(CC_P1_CHARACTER_ADDR) = (uint32_t)initial.chara[0];
    *asU32(CC_P2_CHARACTER_ADDR) = (uint32_t)initial.chara[1];

    *asU32(CC_P1_MOON_SELECTOR_ADDR) = (uint32_t)initial.moon[0];
    *asU32(CC_P2_MOON_SELECTOR_ADDR) = (uint32_t)initial.moon[1];

    *asU32(CC_P1_COLOR_SELECTOR_ADDR) = (uint32_t)initial.color[0];
    *asU32(CC_P2_COLOR_SELECTOR_ADDR) = (uint32_t)initial.color[1];

    *asU32(CC_STAGE_SELECTOR_ADDR) = initial.stage;

    RETURN_MASH_INPUT(0, CC_BUTTON_CONFIRM);
}

uint16_t NetplayManager::getCharaSelectInput(uint8_t player) {
    uint16_t input = getRawInput(player);

    // Prevent hitting Confirm until 150f after beginning of CharaSelect.
    // This works around a moon-selector desync where pressing Confirm
    // too early can lock the moon selection out of sync between peers.
    if (config.mode.isOnline() && getFrame() < 150) {
        input &= ~COMBINE_INPUT(0, CC_BUTTON_A | CC_BUTTON_CONFIRM);
    }

    // Prevent exiting character select (pressing B/Cancel would go back
    // to the main menu, which would desync the FSM).
    if ((*asU32(player == 1 ? CC_P1_SELECTOR_MODE_ADDR : CC_P2_SELECTOR_MODE_ADDR))
        == CC_SELECT_CHARA) {
        input &= ~COMBINE_INPUT(0, CC_BUTTON_B | CC_BUTTON_CANCEL);
    }

    // Don't allow hitting Confirm/Cancel until 2f after we have stopped
    // changing the selector mode. Works around the issue where pressing
    // Confirm right after a selector-mode change can register on a
    // different selector than the player intended.
    if (hasButtonInHistory(player,
                           CC_BUTTON_A | CC_BUTTON_CONFIRM | CC_BUTTON_B | CC_BUTTON_CANCEL,
                           1, 3)) {
        input &= ~COMBINE_INPUT(0, CC_BUTTON_A | CC_BUTTON_CONFIRM |
                                  CC_BUTTON_B | CC_BUTTON_CANCEL);
    }

    return input;
}

uint16_t NetplayManager::getSkippableInput(uint8_t player) {
    // Only allow the confirm and cancel buttons during skippable states
    // (round transitions, post-game, pre-retry). This prevents the
    // player from accidentally pausing or doing other state-changing
    // actions during the skippable cinematic.
    return (getRawInput(player) &
            COMBINE_INPUT(0, CC_BUTTON_CONFIRM | CC_BUTTON_CANCEL));
}

uint16_t NetplayManager::getInGameInput(uint8_t player) {
    uint16_t input = getRawInput(player);

    // Disable pausing in netplay versus mode. Also only allow start
    // button in offline versus after holding it for `heldStartDuration`
    // frames (prevents accidental pausing).
    const bool disable_start =
        ((config.mode.isNetplay() && config.mode.isVersus()) ||
         (!*asU8(CC_PAUSE_FLAG_ADDR) &&
          config.mode.isVersus() &&
          heldStartDuration &&
          !heldButtonInHistory(player, CC_BUTTON_START, 0, heldStartDuration)));
    if (disable_start) {
        input &= ~COMBINE_INPUT(0, CC_BUTTON_START);
    }

    // If the pause menu is up, suppress Confirm for 3f after cursor
    // movement (same currentMenuIndex lag workaround as chara select),
    // and disable returning to main menu (position 6 = versus, 16 = training).
    if (*asU8(CC_PAUSE_FLAG_ADDR)) {
        asm_hacks::menuConfirmState = 2;

        if (hasUpDownInHistory(player, 0, 3))
            input &= ~COMBINE_INPUT(0, CC_BUTTON_A | CC_BUTTON_CONFIRM);

        // 6 = versus, 16 = training. Don't allow Confirm on these
        // positions (would return to main menu and desync the FSM).
        const uint8_t blocked_menu_index = config.mode.isTraining() ? 16 : 6;
        if (asm_hacks::currentMenuIndex == blocked_menu_index)
            input &= ~COMBINE_INPUT(0, CC_BUTTON_A | CC_BUTTON_CONFIRM);
    }

    return input;
}

uint16_t NetplayManager::getReplayMenuInput(uint8_t player) {
    uint16_t input = getRawInput(player);

    // Prevent exiting character select (same desync concern as
    // getCharaSelectInput).
    input &= ~COMBINE_INPUT(0, CC_BUTTON_B | CC_BUTTON_CANCEL);

    return input;
}

uint16_t NetplayManager::getRetryMenuInput(uint8_t player) {
    // Ignore remote input on netplay — only the local player navigates
    // the retry menu, and the chosen index is synced via MenuIndex
    // messages.
    if (player != _localPlayer && config.mode.isNetplay())
        return 0;

    // Auto-navigate when the final retry menu index has been decided
    // (both peers have selected, and we computed the max).
    if (_targetMenuState != -1 && _targetMenuIndex != -1)
        return getMenuNavInput();

    uint16_t input;

    if (config.mode.isNetplay()) {
        input = getRawInput(player);

        // Don't allow hitting Confirm until 3f after we have stopped
        // moving the cursor (currentMenuIndex lag workaround).
        if (hasUpDownInHistory(player, 0, 3))
            input &= ~COMBINE_INPUT(0, CC_BUTTON_A | CC_BUTTON_CONFIRM);

        // Limit retry menu selectable options (don't allow returning
        // to main menu).
        if (asm_hacks::currentMenuIndex > MAX_RETRY_MENU_INDEX)
            input &= ~COMBINE_INPUT(0, CC_BUTTON_A | CC_BUTTON_CONFIRM);

        // Special netplay retry menu behaviour: only select the final
        // option after both sides have selected. The host's index takes
        // priority (max of local and remote, clamped to 1 to prevent
        // returning to main menu).
        if (_remoteRetryMenuIndex != -1 && _localRetryMenuIndex != -1) {
            _targetMenuState = 0;
            _targetMenuIndex = std::max(_localRetryMenuIndex, _remoteRetryMenuIndex);
            _targetMenuIndex = std::min(_targetMenuIndex, static_cast<int8_t>(1));
            setRetryMenuIndex(getIndex(), _targetMenuIndex);
            input = 0;
        } else if (_localRetryMenuIndex != -1) {
            // We've selected but the peer hasn't yet — wait.
            input = 0;
        } else if (asm_hacks::menuConfirmState == 1) {
            // The player just confirmed a menu item — capture the index.
            _localRetryMenuIndex = static_cast<int8_t>(asm_hacks::currentMenuIndex);
            input = 0;
            common::logger::info("netMan: localRetryMenuIndex={}", _localRetryMenuIndex);
        }

        // Disable menu confirms (we're handling them ourselves).
        asm_hacks::menuConfirmState = 0;
    } else {
        // Offline: allow regular retry menu operation.
        asm_hacks::menuConfirmState = 2;
    }

    return input;
}

// ============================================================================
// Menu navigation state machine
// ============================================================================

uint16_t NetplayManager::getMenuNavInput() {
    if (_targetMenuState == -1 || _targetMenuIndex == -1)
        return 0;

    if (_targetMenuState == 0) {
        // Target determined — start moving.
        common::logger::info("netMan: targetMenuIndex={}", _targetMenuIndex);
        _targetMenuState = 1;
    } else if (_targetMenuState == 1) {
        // Issue up/down press to move cursor towards target.
        _targetMenuState = 2;
        if (_targetMenuIndex != static_cast<int8_t>(asm_hacks::currentMenuIndex)) {
            // 8 = up, 2 = down (numpad notation).
            const uint16_t dir = (_targetMenuIndex < static_cast<int8_t>(asm_hacks::currentMenuIndex)) ? 8 : 2;
            return COMBINE_INPUT(dir, 0);
        }
    } else if (_targetMenuState >= 2 && _targetMenuState <= 4) {
        // Wait for currentMenuIndex to update (the game lags by a few
        // frames between input and the currentMenuIndex write).
        ++_targetMenuState;
    } else if (_targetMenuState == 39) {
        // Mash Confirm to select the final option.
        asm_hacks::menuConfirmState = 2;
        RETURN_MASH_INPUT(0, CC_BUTTON_CONFIRM);
    } else if (_targetMenuIndex != static_cast<int8_t>(asm_hacks::currentMenuIndex)) {
        // Not there yet — restart the move.
        _targetMenuState = 1;
    } else {
        // Reached targetMenuIndex — start mashing Confirm.
        common::logger::info("netMan: reached targetMenuIndex={}; currentMenuIndex={}",
                             _targetMenuIndex, asm_hacks::currentMenuIndex);
        _targetMenuState = 39;
    }

    return 0;
}

// ============================================================================
// History helpers
// ============================================================================

bool NetplayManager::hasUpDownInHistory(uint8_t player, uint32_t start, uint32_t end) const {
    // player==0 means "check both players" (used when the menu can be
    // navigated by either player, e.g. offline versus retry menu).
    for (uint32_t i = start; i < end; ++i) {
        if (i > getFrame())
            break;

        if (player == 0) {
            const uint16_t p1dir = 0xF & getRawInput(1, getFrame() - i);
            const uint16_t p2dir = 0xF & getRawInput(2, getFrame() - i);
            if (p1dir == 2 || p1dir == 8 || p2dir == 2 || p2dir == 8)
                return true;
        } else {
            const uint16_t dir = 0xF & getRawInput(player, getFrame() - i);
            if (dir == 2 || dir == 8)
                return true;
        }
    }
    return false;
}

bool NetplayManager::hasButtonInHistory(uint8_t player, uint16_t button,
                                        uint32_t start, uint32_t end) const {
    for (uint32_t i = start; i < end; ++i) {
        if (i > getFrame())
            break;
        const uint16_t buttons = getRawInput(player, getFrame() - i) >> 4;
        if (buttons & button)
            return true;
    }
    return false;
}

bool NetplayManager::heldButtonInHistory(uint8_t player, uint16_t button,
                                         uint32_t start, uint32_t end) const {
    for (uint32_t i = start; i < end; ++i) {
        if (i > getFrame())
            return false;
        const uint16_t buttons = getRawInput(player, getFrame() - i) >> 4;
        if (!(buttons & button))
            return false;
    }
    return true;
}

// ============================================================================
// Player slot / frame tracking
// ============================================================================

void NetplayManager::setRemotePlayer(uint8_t player) {
    _localPlayer = 3 - player;
    _remotePlayer = player;
}

void NetplayManager::updateFrame() {
    _indexedFrame.parts.frame = (*asU32(CC_WORLD_TIMER_ADDR)) - _startWorldTime;
}

uint32_t NetplayManager::getBufferedPreserveStartIndex() const {
    if (preserveStartIndex == UINT_MAX)
        return UINT_MAX;
    if (preserveStartIndex <= PRESERVE_START_INDEX_BUFFER)
        return 0;
    return preserveStartIndex - PRESERVE_START_INDEX_BUFFER;
}

// ============================================================================
// State transitions
// ============================================================================

void NetplayManager::setState(NetplayState state) {
    if (!isValidNext(state)) {
        common::logger::err("netMan: invalid transition {} -> {}",
                            netplayStateStr(_state), netplayStateStr(state));
        return;
    }

    common::logger::info("netMan: setState indexedFrame=[idx={},frame={}] {} -> {}",
                         getIndex(), getFrame(),
                         netplayStateStr(_state), netplayStateStr(state));

    // The first time we leave AutoCharaSelect, we reset the world timer
    // to the initial indexed frame (which the host sent us). After that,
    // each state change increments the transition index and resets
    // frame=0 with _startWorldTime = current world timer.
    if (state != NetplayState::PreInitial && state != NetplayState::Initial) {
        if (_state == NetplayState::AutoCharaSelect) {
            // Start from the initial index and frame.
            _startWorldTime = 0;
            *asU32(CC_WORLD_TIMER_ADDR) = initial.indexedFrame.parts.frame;
            _indexedFrame = initial.indexedFrame;
        } else {
            // Increment the index whenever the NetplayState changes.
            ++_indexedFrame.parts.index;
            // Start counting from frame=0 again.
            _startWorldTime = *asU32(CC_WORLD_TIMER_ADDR);
            _indexedFrame.parts.frame = 0;
        }

        // Entering CharaSelect — record the spectate start index.
        if (state == NetplayState::CharaSelect)
            _spectateStartIndex = getIndex();

        // Entering Loading — garbage-collect old transition indices.
        if (state == NetplayState::Loading) {
            _spectateStartIndex = getIndex();

            const uint32_t newStartIndex =
                std::min(getBufferedPreserveStartIndex(), getIndex());

            if (newStartIndex > _startIndex) {
                const uint32_t offset = newStartIndex - _startIndex;

                _inputs[0].eraseIndexOlderThan(offset);
                _inputs[1].eraseIndexOlderThan(offset);

                if (offset >= _rngStates.size())
                    _rngStates.clear();
                else
                    _rngStates.erase(_rngStates.begin(),
                                     _rngStates.begin() + offset);

                if (offset >= _retryMenuIndicies.size())
                    _retryMenuIndicies.clear();
                else
                    _retryMenuIndicies.erase(_retryMenuIndicies.begin(),
                                             _retryMenuIndicies.begin() + offset);

                _startIndex = newStartIndex;
            }

            _localRetryMenuIndex = -1;
            _remoteRetryMenuIndex = -1;
        }

        // Entering RetryMenu — record the menu state counter so we can
        // detect if other menus open in front of the retry menu later.
        if (state == NetplayState::RetryMenu) {
            _retryMenuStateCounter = *asU32(CC_MENU_STATE_COUNTER_ADDR) + 1;
        }

        // Reset menu navigation state on every transition.
        asm_hacks::currentMenuIndex = 0;
        asm_hacks::menuConfirmState = 0;
        _targetMenuState = -1;
        _targetMenuIndex = -1;
    }

    _state = state;
}

// ============================================================================
// Per-frame input dispatch
// ============================================================================

uint16_t NetplayManager::getInput(uint8_t player) {
    switch (_state) {
        case NetplayState::PreInitial:
            return getPreInitialInput(player);

        case NetplayState::Initial:
            return getInitialInput(player);

        case NetplayState::AutoCharaSelect:
            return getAutoCharaSelectInput(player);

        case NetplayState::CharaSelect:
            return getCharaSelectInput(player);

        case NetplayState::Loading:
        case NetplayState::CharaIntro:
        case NetplayState::Skippable: {
            // If the remote index is ahead, we should mash to skip
            // (the peer has already advanced past this state).
            if (_startIndex + _inputs[_remotePlayer - 1].getEndIndex() > getIndex() + 1) {
                asm_hacks::menuConfirmState = 2;
                RETURN_MASH_INPUT(0, CC_BUTTON_CONFIRM);
            }
            return getSkippableInput(player);
        }

        case NetplayState::InGame:
            return getInGameInput(player);

        case NetplayState::RetryMenu:
            return getRetryMenuInput(player);

        case NetplayState::ReplayMenu:
            return getReplayMenuInput(player);

        default:
            return 0;
    }
}

// ============================================================================
// Raw input access
// ============================================================================

uint16_t NetplayManager::getRawInput(uint8_t player, uint32_t frame) const {
    // The InputsContainer is indexed by (transition_index - _startIndex,
    // frame). The container returns lastInputBefore() if the index is
    // unknown and _inputs[index].back() if the frame is beyond the end,
    // so the caller gets the last known input as the prediction (which
    // is what GGPO-style rollback wants).
    return _inputs[player - 1].get(getIndex() - _startIndex, frame);
}

// ============================================================================
// Single-frame input storage
// ============================================================================

void NetplayManager::setInput(uint8_t player, uint16_t input) {
    // During rollback, write to frame + rollbackDelay (we're re-running
    // from a past state and need to overwrite the predicted inputs).
    // During RetryMenu, write to the current frame (no delay — the menu
    // navigation needs immediate feedback).
    // Otherwise, write to frame + delay (the input won't be "due" until
    // `delay` frames from now, which is what gives rollback its window).
    if (isInRollback()) {
        _inputs[player - 1].set(getIndex() - _startIndex,
                                getFrame() + config.rollbackDelay, input);
    } else if (_state == NetplayState::RetryMenu) {
        _inputs[player - 1].set(getIndex() - _startIndex, getFrame(), input);
    } else {
        _inputs[player - 1].set(getIndex() - _startIndex,
                                getFrame() + config.delay, input);
    }
}

void NetplayManager::assignInput(uint8_t player, uint16_t input, uint32_t frame) {
    assignInput(player, input, {{frame, getIndex()}});
}

void NetplayManager::assignInput(uint8_t player, uint16_t input, IndexedFrame indexedFrame) {
    _inputs[player - 1].assign(indexedFrame.parts.index - _startIndex,
                               indexedFrame.parts.frame, input);
}

// ============================================================================
// Batch input accessors (wire protocol)
// ============================================================================

std::optional<PlayerInputs> NetplayManager::getInputs(uint8_t player) const {
    if (_inputs[player - 1].empty(getIndex() - _startIndex))
        return std::nullopt;

    const uint32_t endFrame = _inputs[player - 1].getEndFrame(getIndex() - _startIndex);
    if (endFrame == 0)
        return std::nullopt;

    PlayerInputs pi;
    pi.indexedFrame.parts.index = getIndex();
    pi.indexedFrame.parts.frame = endFrame - 1;  // last frame we have

    _inputs[player - 1].get(pi.getIndex() - _startIndex, pi.getStartFrame(),
                            pi.inputs.data(), pi.size());

    return pi;
}

void NetplayManager::setInputs(uint8_t player, const PlayerInputs& playerInputs) {
    // Drop batches that are older than the current transition index by
    // more than 1 (no point keeping stale remote inputs), or that are
    // older than our startIndex (already garbage-collected).
    if (playerInputs.getIndex() + 1 < getIndex() ||
        playerInputs.getIndex() < _startIndex)
        return;

    // During rollback, enable divergence detection: if the new inputs
    // disagree with what we predicted, mark the earliest disagreement
    // via _lastChangedFrame (the rollback trigger).
    const uint32_t checkStartingFromIndex = isInRollback() ? (getIndex() - _startIndex) : UINT32_MAX;

    _inputs[player - 1].set(playerInputs.getIndex() - _startIndex,
                            playerInputs.getStartFrame(),
                            playerInputs.inputs.data(),
                            playerInputs.size(),
                            checkStartingFromIndex);
}

std::optional<BothInputs> NetplayManager::getBothInputs(IndexedFrame& pos) const {
    if (pos.parts.index > getIndex())
        return std::nullopt;

    IndexedFrame orig = pos;

    // The most recent frame, in the spectator's transition index, that
    // the spectator is allowed to "see".
    uint32_t commonEndFrame = std::min(
        _inputs[0].getEndFrame(orig.parts.index - _startIndex),
        _inputs[1].getEndFrame(orig.parts.index - _startIndex));

    if (orig.parts.index == getIndex()) {
        // During the same transition index.
        if (isInRollback()) {
            // Add a buffer to the end frame during rollback (we don't
            // want to send the spectator inputs that we're about to
            // roll back and overwrite).
            if (commonEndFrame > 2 * NUM_INPUTS)
                commonEndFrame -= 2 * NUM_INPUTS;
            else
                commonEndFrame = 0;
        }

        if (orig.parts.frame + 1 <= commonEndFrame) {
            // Increment by NUM_INPUTS when behind.
            pos.parts.frame += NUM_INPUTS;
        } else {
            // Otherwise return empty — spectator must wait.
            return std::nullopt;
        }
    } else {
        // During an older transition index.
        if (orig.parts.frame + 1 <= commonEndFrame) {
            pos.parts.frame += NUM_INPUTS;
        } else {
            // End of this transition index — advance to the next one.
            pos.parts.frame = NUM_INPUTS - 1;
            ++pos.parts.index;
            if (commonEndFrame == 0)
                return std::nullopt;
            orig.parts.frame = commonEndFrame - 1;
        }
    }

    BothInputs bi(orig);

    _inputs[0].get(bi.getIndex() - _startIndex, bi.getStartFrame(),
                   bi.inputs[0].data(), bi.size());
    _inputs[1].get(bi.getIndex() - _startIndex, bi.getStartFrame(),
                   bi.inputs[1].data(), bi.size());

    return bi;
}

void NetplayManager::setBothInputs(const BothInputs& bothInputs) {
    if (bothInputs.getIndex() + 1 < getIndex() ||
        bothInputs.getIndex() < _startIndex)
        return;

    _inputs[0].set(bothInputs.getIndex() - _startIndex, bothInputs.getStartFrame(),
                   bothInputs.inputs[0].data(), bothInputs.size());
    _inputs[1].set(bothInputs.getIndex() - _startIndex, bothInputs.getStartFrame(),
                   bothInputs.inputs[1].data(), bothInputs.size());
}

// ============================================================================
// Remote-input readiness
// ============================================================================

bool NetplayManager::isRemoteInputReady() const {
    // For states before CharaSelect, plus Loading/CharaIntro/Skippable/
    // RetryMenu, we don't need remote inputs (we're either pre-game or
    // in a state where we can mash-confirm to skip).
    if (_state == NetplayState::PreInitial ||
        _state == NetplayState::Initial ||
        _state == NetplayState::AutoCharaSelect ||
        _state == NetplayState::Loading ||
        _state == NetplayState::CharaIntro ||
        _state == NetplayState::Skippable ||
        _state == NetplayState::RetryMenu) {
        return true;
    }

    if (_inputs[_remotePlayer - 1].empty()) {
        return false;
    }

    // Need at least one input at or beyond our current transition index.
    if (_startIndex + _inputs[_remotePlayer - 1].getEndIndex() - 1 < getIndex()) {
        return false;
    }

    // If remote index is ahead, we're in an older state — no need to
    // wait, we'll catch up via mash-confirm.
    if (_startIndex + _inputs[_remotePlayer - 1].getEndIndex() - 1 > getIndex())
        return true;

    // Same index — need at least one frame at or beyond our current frame.
    if (_inputs[_remotePlayer - 1].getEndFrame() == 0)
        return false;

    const uint8_t maxFramesAhead = isInRollback() ? config.rollback : 0;
    if ((_inputs[_remotePlayer - 1].getEndFrame() - 1 + maxFramesAhead) < getFrame()) {
        return false;
    }

    return true;
}

// ============================================================================
// RngState
// ============================================================================

std::shared_ptr<RngState> NetplayManager::getRngState(uint32_t index) const {
    // Offline: no RngState sync (the local RNG is the only RNG).
    if (config.mode.isOffline())
        return nullptr;

    if (index >= _startIndex + _rngStates.size())
        return nullptr;

    return _rngStates[index - _startIndex];
}

void NetplayManager::setRngState(const RngState& rngState) {
    if (config.mode.isOffline() || rngState.index == 0 || rngState.index < _startIndex)
        return;

    if (rngState.index >= _startIndex + _rngStates.size())
        _rngStates.resize(rngState.index + 1 - _startIndex);

    _rngStates[rngState.index - _startIndex] = std::make_shared<RngState>(rngState);
}

bool NetplayManager::isRngStateReady(bool shouldSyncRngState) const {
    // The host generates RngStates; the client waits for them. We don't
    // need to sync if shouldSyncRngState is false, or if we're the host
    // (host generates), or if we're offline, or if we're pre-CharaSelect.
    if (!shouldSyncRngState ||
        config.mode.isHost() ||
        config.mode.isOffline() ||
        _state == NetplayState::PreInitial ||
        _state == NetplayState::Initial ||
        _state == NetplayState::AutoCharaSelect) {
        return true;
    }

    if (_rngStates.empty())
        return false;

    if ((_startIndex + _rngStates.size() - 1) < getIndex())
        return false;

    return true;
}

// ============================================================================
// Retry menu index sync
// ============================================================================

std::optional<MenuIndex> NetplayManager::getLocalRetryMenuIndex() const {
    if (_state == NetplayState::RetryMenu && _localRetryMenuIndex != -1)
        return MenuIndex(getIndex(), _localRetryMenuIndex);
    return std::nullopt;
}

void NetplayManager::setRemoteRetryMenuIndex(int8_t menuIndex) {
    _remoteRetryMenuIndex = menuIndex;
    common::logger::info("netMan: remoteRetryMenuIndex={}", _remoteRetryMenuIndex);
}

std::optional<MenuIndex> NetplayManager::getRetryMenuIndex(uint32_t index) const {
    if (config.mode.isOffline())
        return std::nullopt;

    if (index >= _startIndex + _retryMenuIndicies.size())
        return std::nullopt;

    if (_retryMenuIndicies[index - _startIndex] < 0)
        return std::nullopt;

    return MenuIndex(index, _retryMenuIndicies[index - _startIndex]);
}

void NetplayManager::setRetryMenuIndex(uint32_t index, int8_t menuIndex) {
    if (config.mode.isOffline() || index == 0 || index < _startIndex || menuIndex < 0)
        return;

    if (index >= _startIndex + _retryMenuIndicies.size())
        _retryMenuIndicies.resize(index + 1 - _startIndex, -1);

    _retryMenuIndicies[index - _startIndex] = menuIndex;
}

// ============================================================================
// Remote frame / index accessors
// ============================================================================

uint32_t NetplayManager::getRemoteIndex() const {
    uint32_t remoteIndex = _inputs[_remotePlayer - 1].getEndIndex() + _startIndex;
    if (remoteIndex > 0)
        --remoteIndex;
    return remoteIndex;
}

uint32_t NetplayManager::getRemoteFrame() const {
    uint32_t remoteFrame = _inputs[_remotePlayer - 1].getEndFrame();
    if (remoteFrame > 0)
        --remoteFrame;
    return remoteFrame;
}

IndexedFrame NetplayManager::getRemoteIndexedFrame() const {
    IndexedFrame f;
    f.parts.frame = _inputs[_remotePlayer - 1].getEndFrame();
    f.parts.index = _inputs[_remotePlayer - 1].getEndIndex() + _startIndex;
    if (f.parts.frame > 0) --f.parts.frame;
    if (f.parts.index > 0) --f.parts.index;
    return f;
}

IndexedFrame NetplayManager::getLastChangedFrame() const {
    IndexedFrame f = _inputs[_remotePlayer - 1].getLastChangedFrame();
    if (f.value == MaxIndexedFrame.value)
        return MaxIndexedFrame;
    // Convert from container-local index back to absolute index.
    return {{f.parts.frame, _startIndex + f.parts.index}};
}

void NetplayManager::clearLastChangedFrame() {
    _inputs[_remotePlayer - 1].clearLastChangedFrame();
}

void NetplayManager::setRemoteIndex(uint32_t remoteIndex) {
    if (remoteIndex < _startIndex)
        return;

    common::logger::info("netMan: setRemoteIndex={}", remoteIndex);

    // Pre-allocate space in the remote player's container so future
    // setInputs calls don't have to resize.
    _inputs[_remotePlayer - 1].resize(remoteIndex - _startIndex, 0, 0);
}

} // namespace caster::dll
