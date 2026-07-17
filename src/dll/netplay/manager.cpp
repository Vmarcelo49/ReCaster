// src/dll/netplay/manager.cpp
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

#include "manager.hpp"
#include "thread_affinity.hpp"
#include "hooks/asm_patches.hpp"
#include "game/character_tables.hpp"
#include "game/game_io.hpp"
#include "../common/logger.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

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
        if (getFrameLocked() % 2)                                         \
            return 0;                                               \
        return COMBINE_INPUT((DIRECTION), (BUTTONS));               \
    } while (0)


// ============================================================================
// Per-state input generators
// ============================================================================

uint16_t NetplayManager::getPreInitialInputLocked(uint8_t player) {
    // Drive the game past Startup → Opening → Title → Main by mashing
    // Confirm. menuConfirmState=2 tells hijackMenu to let our injected
    // confirms actually advance the menus.
    if ((*asU32(CC_GAME_MODE_ADDR)) == CC_GAME_MODE_MAIN)
        return 0;

    asm_hacks::menuConfirmState = 2;
    RETURN_MASH_INPUT(0, CC_BUTTON_CONFIRM);
}

uint16_t NetplayManager::getInitialInputLocked(uint8_t player) {
    if ((*asU32(CC_GAME_MODE_ADDR)) != CC_GAME_MODE_MAIN)
        return getPreInitialInputLocked(player);

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

uint16_t NetplayManager::getAutoCharaSelectInputLocked(uint8_t player) {
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

uint16_t NetplayManager::getCharaSelectInputLocked(uint8_t player) {
    uint16_t input = getRawInputLocked(player);

    // Prevent hitting Confirm until 150f after beginning of CharaSelect.
    // This works around a moon-selector desync where pressing Confirm
    // too early can lock the moon selection out of sync between peers.
    if (config.mode.isOnline() && getFrameLocked() < 150) {
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
    if (hasButtonInHistoryLocked(player,
                           CC_BUTTON_A | CC_BUTTON_CONFIRM | CC_BUTTON_B | CC_BUTTON_CANCEL,
                           1, 3)) {
        input &= ~COMBINE_INPUT(0, CC_BUTTON_A | CC_BUTTON_CONFIRM |
                                  CC_BUTTON_B | CC_BUTTON_CANCEL);
    }

    return input;
}

uint16_t NetplayManager::getSkippableInputLocked(uint8_t player) {
    // Only allow the confirm and cancel buttons during skippable states
    // (round transitions, post-game, pre-retry). This prevents the
    // player from accidentally pausing or doing other state-changing
    // actions during the skippable cinematic.
    return (getRawInputLocked(player) &
            COMBINE_INPUT(0, CC_BUTTON_CONFIRM | CC_BUTTON_CANCEL));
}

uint16_t NetplayManager::getInGameInputLocked(uint8_t player) {
    uint16_t input = getRawInputLocked(player);

    // Disable pausing in netplay versus mode. Also only allow start
    // button in offline versus after holding it for `heldStartDuration`
    // frames (prevents accidental pausing).
    const bool disable_start =
        ((config.mode.isNetplay() && config.mode.isVersus()) ||
         (!*asU8(CC_PAUSE_FLAG_ADDR) &&
          config.mode.isVersus() &&
          heldStartDuration &&
          !heldButtonInHistoryLocked(player, CC_BUTTON_START, 0, heldStartDuration)));
    if (disable_start) {
        input &= ~COMBINE_INPUT(0, CC_BUTTON_START);
    }

    // If the pause menu is up, suppress Confirm for 3f after cursor
    // movement (same currentMenuIndex lag workaround as chara select),
    // and disable returning to main menu (position 6 = versus, 16 = training).
    if (*asU8(CC_PAUSE_FLAG_ADDR)) {
        asm_hacks::menuConfirmState = 2;

        if (hasUpDownInHistoryLocked(player, 0, 3))
            input &= ~COMBINE_INPUT(0, CC_BUTTON_A | CC_BUTTON_CONFIRM);

        // 6 = versus, 16 = training. Don't allow Confirm on these
        // positions (would return to main menu and desync the FSM).
        const uint8_t blocked_menu_index = config.mode.isTraining() ? 16 : 6;
        if (asm_hacks::currentMenuIndex == blocked_menu_index)
            input &= ~COMBINE_INPUT(0, CC_BUTTON_A | CC_BUTTON_CONFIRM);
    }

    return input;
}

uint16_t NetplayManager::getReplayMenuInputLocked(uint8_t player) {
    uint16_t input = getRawInputLocked(player);

    // Prevent exiting character select (same desync concern as
    // getCharaSelectInput).
    input &= ~COMBINE_INPUT(0, CC_BUTTON_B | CC_BUTTON_CANCEL);

    return input;
}

uint16_t NetplayManager::getRetryMenuInputLocked(uint8_t player) {
    // Ignore remote input on netplay — only the local player navigates
    // the retry menu, and the chosen index is synced via MenuIndex
    // messages.
    if (player != _localPlayer && config.mode.isNetplay())
        return 0;

    // Auto-navigate when the final retry menu index has been decided
    // (both peers have selected, and we computed the max).
    if (_targetMenuState != -1 && _targetMenuIndex != -1)
        return getMenuNavInputLocked();

    uint16_t input = 0;

    if (config.mode.isNetplay()) {
        input = getRawInputLocked(player);

        // Don't allow hitting Confirm until 3f after we have stopped
        // moving the cursor (currentMenuIndex lag workaround).
        if (hasUpDownInHistoryLocked(player, 0, 3))
            input &= ~COMBINE_INPUT(0, CC_BUTTON_A | CC_BUTTON_CONFIRM);

        // Limit retry menu selectable options (don't allow returning
        // to main menu).
        if (asm_hacks::currentMenuIndex > MAX_RETRY_MENU_INDEX)
            input &= ~COMBINE_INPUT(0, CC_BUTTON_A | CC_BUTTON_CONFIRM);

        // Allow confirms when the player has navigated to "Save Replay"
        // (index 2) or when a submenu has opened in front of the retry
        // menu (CC_MENU_STATE_COUNTER increased beyond the value we
        // captured on entry). Without this branch, the netplay branch
        // below unconditionally resets menuConfirmState to 0, blocking
        // ALL confirms and hanging the game if a submenu opens.
        //
        // Ported from CCCaster DllNetplayManager.cpp:384-397. The
        // autoReplaySaveStatePtr check is omitted (ReCaster stripped the
        // saveReplay ASM hook).
        if (asm_hacks::currentMenuIndex == 2 ||
            *asU32(CC_MENU_STATE_COUNTER_ADDR) > _retryMenuStateCounter) {
            asm_hacks::menuConfirmState = 2;
            return input;
        }

        // Special netplay retry menu behaviour: only select the final
        // option after both sides have selected. The host's index takes
        // priority (max of local and remote, clamped to 1 to prevent
        // returning to main menu).
        if (_remoteRetryMenuIndex != -1 && _localRetryMenuIndex != -1) {
            _targetMenuState = 0;
            _targetMenuIndex = std::max(_localRetryMenuIndex, _remoteRetryMenuIndex);
            _targetMenuIndex = std::min(_targetMenuIndex, static_cast<int8_t>(1));
            setRetryMenuIndexLocked(getIndexLocked(), _targetMenuIndex);
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
        // Offline: pass through the player's raw input and allow
        // regular retry menu operation.
        input = getRawInputLocked(player);
        asm_hacks::menuConfirmState = 2;
    }

    return input;
}

// ============================================================================
// Menu navigation state machine
// ============================================================================

uint16_t NetplayManager::getMenuNavInputLocked() {
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

bool NetplayManager::hasUpDownInHistoryLocked(uint8_t player, uint32_t start, uint32_t end) const {
    // player==0 means "check both players" (used when the menu can be
    // navigated by either player, e.g. offline versus retry menu).
    for (uint32_t i = start; i < end; ++i) {
        if (i > getFrameLocked())
            break;

        if (player == 0) {
            const uint16_t p1dir = 0xF & getRawInputLocked(1, getFrameLocked() - i);
            const uint16_t p2dir = 0xF & getRawInputLocked(2, getFrameLocked() - i);
            if (p1dir == 2 || p1dir == 8 || p2dir == 2 || p2dir == 8)
                return true;
        } else {
            const uint16_t dir = 0xF & getRawInputLocked(player, getFrameLocked() - i);
            if (dir == 2 || dir == 8)
                return true;
        }
    }
    return false;
}

bool NetplayManager::hasButtonInHistoryLocked(uint8_t player, uint16_t button,
                                        uint32_t start, uint32_t end) const {
    for (uint32_t i = start; i < end; ++i) {
        if (i > getFrameLocked())
            break;
        const uint16_t buttons = getRawInputLocked(player, getFrameLocked() - i) >> 4;
        if (buttons & button)
            return true;
    }
    return false;
}

bool NetplayManager::heldButtonInHistoryLocked(uint8_t player, uint16_t button,
                                         uint32_t start, uint32_t end) const {
    for (uint32_t i = start; i < end; ++i) {
        if (i > getFrameLocked())
            return false;
        const uint16_t buttons = getRawInputLocked(player, getFrameLocked() - i) >> 4;
        if (!(buttons & button))
            return false;
    }
    return true;
}

// ============================================================================
// Player slot / frame tracking
// ============================================================================

void NetplayManager::setRemotePlayer(uint8_t player) {
    NETMAN_LOCK_GUARD();
    setRemotePlayerLocked(player);
}

void NetplayManager::updateFrame() {
    NETMAN_LOCK_GUARD();
    updateFrameLocked();
}

uint32_t NetplayManager::getBufferedPreserveStartIndexLocked() const {
    if (preserveStartIndex == UINT_MAX)
        return UINT_MAX;
    if (preserveStartIndex <= PRESERVE_START_INDEX_BUFFER)
        return 0;
    return preserveStartIndex - PRESERVE_START_INDEX_BUFFER;
}

// ============================================================================
// State transitions
// ============================================================================

void NetplayManager::setStateLocked(NetplayState state) {
    if (!isValidNextLocked(state)) {
        common::logger::err("netMan: invalid transition {} -> {}",
                            netplayStateStr(_state), netplayStateStr(state));
        return;
    }

    common::logger::info("netMan: setState indexedFrame=[idx={},frame={}] {} -> {}",
                         getIndexLocked(), getFrameLocked(),
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
            _spectateStartIndex = getIndexLocked();

        // Entering Loading — garbage-collect old transition indices.
        if (state == NetplayState::Loading) {
            _spectateStartIndex = getIndexLocked();

            const uint32_t newStartIndex =
                std::min(getBufferedPreserveStartIndexLocked(), getIndexLocked());

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

uint16_t NetplayManager::getInputLocked(uint8_t player) {
    switch (_state) {
        case NetplayState::PreInitial:
            return getPreInitialInputLocked(player);

        case NetplayState::Initial:
            return getInitialInputLocked(player);

        case NetplayState::AutoCharaSelect:
            return getAutoCharaSelectInputLocked(player);

        case NetplayState::CharaSelect:
            return getCharaSelectInputLocked(player);

        case NetplayState::Loading:
        case NetplayState::CharaIntro:
        case NetplayState::Skippable: {
            // If the remote index is ahead, we should mash to skip
            // (the peer has already advanced past this state).
            if (_startIndex + _inputs[_remotePlayer - 1].getEndIndex() > getIndexLocked() + 1) {
                asm_hacks::menuConfirmState = 2;
                RETURN_MASH_INPUT(0, CC_BUTTON_CONFIRM);
            }
            return getSkippableInputLocked(player);
        }

        case NetplayState::InGame:
            return getInGameInputLocked(player);

        case NetplayState::RetryMenu:
            return getRetryMenuInputLocked(player);

        case NetplayState::ReplayMenu:
            return getReplayMenuInputLocked(player);

        default:
            return 0;
    }
}

// ============================================================================
// Raw input access
// ============================================================================

// Phase B / Phase 4: Stateful predictor.
//
// When enabled (CASTER_PREDICTOR=stateful), predictions for the remote
// player check the game's NO_INPUT_FLAG. If set, the character can't
// accept input (round over, hitstop, cinematic, etc.), so the real
// remote input is almost certainly neutral (0). Returning 0 instead of
// lastInputBefore avoids unnecessary rollbacks when the opponent is
// stuck in such a state.
//
// Default (no env var, or CASTER_PREDICTOR=last) keeps the original
// lastInputBefore behavior — safe fallback if the stateful predictor
// ever causes unexpected behavior.
//
// The flag is read once at first use; subsequent calls are branch-free
// on the resulting bool.
enum class PredictorMode { Last, Stateful };
PredictorMode predictorMode() {
    static const PredictorMode mode = []{
        const char* v = std::getenv("CASTER_PREDICTOR");
        return (v && std::strcmp(v, "stateful") == 0)
            ? PredictorMode::Stateful
            : PredictorMode::Last;
    }();
    return mode;
}

uint16_t NetplayManager::getRawInputLocked(uint8_t player, uint32_t frame) const {
    // The InputsContainer is indexed by (transition_index - _startIndex,
    // frame). The container returns lastInputBefore() if the index is
    // unknown and _inputs[index].back() if the frame is beyond the end,
    // so the caller gets the last known input as the prediction (which
    // is what GGPO-style rollback wants).
    //
    // Phase B / Phase 4: For the REMOTE player, when the requested
    // frame is beyond what we have (i.e. we're predicting, not reading
    // a real input), and the stateful predictor is enabled, check
    // CC_P{N}_NO_INPUT_FLAG_ADDR. If set, return 0 (neutral) instead
    // of lastInputBefore — the character can't accept input in that
    // state, so the opponent is almost certainly sending 0.
    if (player == _remotePlayer && predictorMode() == PredictorMode::Stateful) {
        const uint32_t relIndex = getIndexLocked() - _startIndex;
        const auto& cont = _inputs[player - 1];

        // Are we predicting? (frame beyond what we have for current index,
        // OR index unknown entirely)
        const bool predicting = (relIndex >= cont.getEndIndex()) ||
                                 (frame >= cont.getEndFrame(relIndex));

        if (predicting) {
            // Check the game's NO_INPUT_FLAG for the remote player.
            // P1 = 0x5552A7, P2 = 0x5552A7 + CC_PLR_STRUCT_SIZE.
            const std::uintptr_t flagAddr =
                (_remotePlayer == 1) ? CC_P1_NO_INPUT_FLAG_ADDR
                                     : CC_P2_NO_INPUT_FLAG_ADDR;
            if (*asU8(flagAddr)) {
                return 0;  // Opponent can't input — predict neutral.
            }
        }
    }

    return _inputs[player - 1].get(getIndexLocked() - _startIndex, frame);
}

// ============================================================================
// Single-frame input storage
// ============================================================================

void NetplayManager::setInputLocked(uint8_t player, uint16_t input) {
    // During rollback, write to frame + rollbackDelay (we're re-running
    // from a past state and need to overwrite the predicted inputs).
    // During RetryMenu, write to the current frame (no delay — the menu
    // navigation needs immediate feedback).
    // Otherwise, write to frame + delay (the input won't be "due" until
    // `delay` frames from now, which is what gives rollback its window).
    if (isInRollbackLocked()) {
        _inputs[player - 1].set(getIndexLocked() - _startIndex,
                                getFrameLocked() + config.rollbackDelay, input);
    } else if (_state == NetplayState::RetryMenu) {
        _inputs[player - 1].set(getIndexLocked() - _startIndex, getFrameLocked(), input);
    } else {
        _inputs[player - 1].set(getIndexLocked() - _startIndex,
                                getFrameLocked() + config.delay, input);
    }
}

void NetplayManager::assignInputLocked(uint8_t player, uint16_t input, uint32_t frame) {
    assignInputLocked(player, input, {{frame, getIndexLocked()}});
}

void NetplayManager::assignInputLocked(uint8_t player, uint16_t input, IndexedFrame indexedFrame) {
    _inputs[player - 1].assign(indexedFrame.parts.index - _startIndex,
                               indexedFrame.parts.frame, input);
}

// ============================================================================
// Batch input accessors (wire protocol)
// ============================================================================

std::optional<PlayerInputs> NetplayManager::getInputsLocked(uint8_t player) const {
    if (_inputs[player - 1].empty(getIndexLocked() - _startIndex))
        return std::nullopt;

    const uint32_t endFrame = _inputs[player - 1].getEndFrame(getIndexLocked() - _startIndex);
    if (endFrame == 0)
        return std::nullopt;

    PlayerInputs pi;
    pi.indexedFrame.parts.index = getIndexLocked();
    pi.indexedFrame.parts.frame = endFrame - 1;  // last frame we have

    _inputs[player - 1].get(pi.getIndex() - _startIndex, pi.getStartFrame(),
                            pi.inputs.data(), pi.size());

    return pi;
}

void NetplayManager::setInputsLocked(uint8_t player, const PlayerInputs& playerInputs) {
    // Drop batches that are older than the current transition index by
    // more than 1 (no point keeping stale remote inputs), or that are
    // older than our startIndex (already garbage-collected).
    if (playerInputs.getIndex() + 1 < getIndexLocked() ||
        playerInputs.getIndex() < _startIndex)
        return;

    // During rollback, enable divergence detection: if the new inputs
    // disagree with what we predicted, mark the earliest disagreement
    // via _lastChangedFrame (the rollback trigger).
    const uint32_t checkStartingFromIndex = isInRollbackLocked() ? (getIndexLocked() - _startIndex) : UINT32_MAX;

    _inputs[player - 1].set(playerInputs.getIndex() - _startIndex,
                            playerInputs.getStartFrame(),
                            playerInputs.inputs.data(),
                            playerInputs.size(),
                            checkStartingFromIndex);
}

std::optional<BothInputs> NetplayManager::getBothInputsLocked(IndexedFrame& pos) const {
    if (pos.parts.index > getIndexLocked())
        return std::nullopt;

    IndexedFrame orig = pos;

    // The most recent frame, in the spectator's transition index, that
    // the spectator is allowed to "see".
    uint32_t commonEndFrame = std::min(
        _inputs[0].getEndFrame(orig.parts.index - _startIndex),
        _inputs[1].getEndFrame(orig.parts.index - _startIndex));

    if (orig.parts.index == getIndexLocked()) {
        // During the same transition index.
        if (isInRollbackLocked()) {
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

void NetplayManager::setBothInputsLocked(const BothInputs& bothInputs) {
    if (bothInputs.getIndex() + 1 < getIndexLocked() ||
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

bool NetplayManager::isRemoteInputReadyLocked() const {
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
    if (_startIndex + _inputs[_remotePlayer - 1].getEndIndex() - 1 < getIndexLocked()) {
        return false;
    }

    // If remote index is ahead, we're in an older state — no need to
    // wait, we'll catch up via mash-confirm.
    if (_startIndex + _inputs[_remotePlayer - 1].getEndIndex() - 1 > getIndexLocked())
        return true;

    // Same index — need at least one frame at or beyond our current frame.
    //
    // Use getEndFrame(getIndexLocked() - _startIndex) to check the CURRENT index,
    // not getEndFrame() (which checks the LAST index). When both peers
    // transition to InGame simultaneously, the remote may have frames in
    // the previous index but not yet in the current one.
    const uint32_t remoteEndFrame =
        _inputs[_remotePlayer - 1].getEndFrame(getIndexLocked() - _startIndex);
    if (remoteEndFrame == 0) {
        // No remote frames for this index yet — WAIT for the remote to
        // catch up within 1 RTT. Do NOT predict via lastInputBefore.
        //
        // This matches CCCaster's DllNetplayManager.cpp:1002-1006:
        //   if (_inputs[_remotePlayer - 1].getEndFrame() == 0)
        //       return false;   // WAIT, never predict
        //
        // A previous revision of this function returned true here when
        // rollback was enabled (added in commit 6c83816 to fix a
        // rollback deadlock). That branch predicted the first InGame
        // frames from the stale Confirm press inherited from the prior
        // CharaSelect/CharaIntro transition. When the real remote input
        // arrived 1 RTT later and diverged, the rollback engine tried
        // to correct — but no saveState had run yet on frame 0
        // (saveState fires at END of the first frameStep), so loadState
        // returned false and the desync propagated irrecoverably.
        //
        // Returning false trades a brief 1-RTT stall for correctness.
        // See GitHub issue #1.
        return false;
    }

    // Phase B1: Speculative rollback.
    //
    // During InGame with rollback enabled, allow advancing up to
    // MAX_ROLLBACK frames (15) ahead of the latest received remote input.
    // The missing frames are predicted via lastInputBefore (the last
    // known remote input is repeated). When the real inputs arrive and
    // diverge from the prediction, the rollback engine corrects via
    // loadState + rerun.
    //
    // Previously this used config.rollback (typically 4-7), which meant
    // the spin-lock blocked the game thread on any connection with more
    // than ~80-120ms RTT. With MAX_ROLLBACK (15), the game runs at 60fps
    // up to ~250ms RTT before falling back to lockstep.
    //
    // The cap at MAX_ROLLBACK is intentional: beyond 15 frames of
    // replay, the rollback burst would take >15ms (one memcpy of 1.18MB
    // state per frame), eating the entire 16.6ms frame budget. Better
    // to stall than to replay 20+ frames every time the opponent does
    // something new.
    //
    // CASTER_DETERMINISTIC=1 env var reverts to the old behavior
    // (config.rollback as the cap) for debugging desyncs.
    static const bool s_deterministic = []{
        const char* v = std::getenv("CASTER_DETERMINISTIC");
        return v && v[0] == '1';
    }();
    const uint8_t maxFramesAhead = isInRollbackLocked()
        ? (s_deterministic ? config.rollback : MAX_ROLLBACK)
        : 0;
    if ((remoteEndFrame - 1 + maxFramesAhead) < getFrameLocked()) {
        return false;
    }

    return true;
}

// ============================================================================
// RngState
// ============================================================================

std::shared_ptr<RngState> NetplayManager::getRngStateLocked(uint32_t index) const {
    // Offline: no RngState sync (the local RNG is the only RNG).
    if (config.mode.isOffline())
        return nullptr;

    if (index >= _startIndex + _rngStates.size())
        return nullptr;

    return _rngStates[index - _startIndex];
}

void NetplayManager::setRngStateLocked(const RngState& rngState) {
    if (config.mode.isOffline() || rngState.index == 0 || rngState.index < _startIndex)
        return;

    if (rngState.index >= _startIndex + _rngStates.size())
        _rngStates.resize(rngState.index + 1 - _startIndex);

    _rngStates[rngState.index - _startIndex] = std::make_shared<RngState>(rngState);
}

bool NetplayManager::isRngStateReadyLocked(bool shouldSyncRngState) const {
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

    if ((_startIndex + _rngStates.size() - 1) < getIndexLocked())
        return false;

    return true;
}

// ============================================================================
// Retry menu index sync
// ============================================================================

std::optional<MenuIndex> NetplayManager::getLocalRetryMenuIndexLocked() const {
    if (_state == NetplayState::RetryMenu && _localRetryMenuIndex != -1)
        return MenuIndex(getIndexLocked(), _localRetryMenuIndex);
    return std::nullopt;
}

void NetplayManager::setRemoteRetryMenuIndexLocked(int8_t menuIndex) {
    _remoteRetryMenuIndex = menuIndex;
    common::logger::info("netMan: remoteRetryMenuIndex={}", _remoteRetryMenuIndex);
}

void NetplayManager::setLocalRetryMenuIndexLocked(int8_t menuIndex) {
    // Force-capture the local retry-menu selection. Used by auto-input
    // mode to directly select "Rematch" (index 1) without relying on
    // the human-driven menuConfirmState capture in getRetryMenuInputLocked().
    // The forced index still flows through the normal netplay path:
    // getLocalRetryMenuIndexLocked() returns it → sendMenuIndex() fires →
    // peer receives it → both sides compute _targetMenuIndex →
    // getMenuNavInputLocked() navigates the cursor to the target.
    _localRetryMenuIndex = menuIndex;
    common::logger::info("netMan: localRetryMenuIndex (forced)={}", _localRetryMenuIndex);
}

std::optional<MenuIndex> NetplayManager::getRetryMenuIndexLocked(uint32_t index) const {
    if (config.mode.isOffline())
        return std::nullopt;

    if (index >= _startIndex + _retryMenuIndicies.size())
        return std::nullopt;

    if (_retryMenuIndicies[index - _startIndex] < 0)
        return std::nullopt;

    return MenuIndex(index, _retryMenuIndicies[index - _startIndex]);
}

void NetplayManager::setRetryMenuIndexLocked(uint32_t index, int8_t menuIndex) {
    if (config.mode.isOffline() || index == 0 || index < _startIndex || menuIndex < 0)
        return;

    if (index >= _startIndex + _retryMenuIndicies.size())
        _retryMenuIndicies.resize(index + 1 - _startIndex, -1);

    _retryMenuIndicies[index - _startIndex] = menuIndex;
}

// ============================================================================
// Remote frame / index accessors
// ============================================================================

uint32_t NetplayManager::getRemoteIndexLocked() const {
    uint32_t remoteIndex = _inputs[_remotePlayer - 1].getEndIndex() + _startIndex;
    if (remoteIndex > 0)
        --remoteIndex;
    return remoteIndex;
}

uint32_t NetplayManager::getRemoteFrameLocked() const {
    uint32_t remoteFrame = _inputs[_remotePlayer - 1].getEndFrame();
    if (remoteFrame > 0)
        --remoteFrame;
    return remoteFrame;
}

IndexedFrame NetplayManager::getRemoteIndexedFrameLocked() const {
    IndexedFrame f;
    f.parts.frame = _inputs[_remotePlayer - 1].getEndFrame();
    f.parts.index = _inputs[_remotePlayer - 1].getEndIndex() + _startIndex;
    if (f.parts.frame > 0) --f.parts.frame;
    if (f.parts.index > 0) --f.parts.index;
    return f;
}

IndexedFrame NetplayManager::getLastChangedFrameLocked() const {
    IndexedFrame f = _inputs[_remotePlayer - 1].getLastChangedFrame();
    if (f.value == MaxIndexedFrame.value)
        return MaxIndexedFrame;
    // Convert from container-local index back to absolute index.
    return {{f.parts.frame, _startIndex + f.parts.index}};
}

void NetplayManager::clearLastChangedFrameLocked() {
    _inputs[_remotePlayer - 1].clearLastChangedFrame();
}

void NetplayManager::setRemoteIndexLocked(uint32_t remoteIndex) {
    if (remoteIndex < _startIndex)
        return;

    common::logger::info("netMan: setRemoteIndex={}", remoteIndex);

    // Pre-allocate space in the remote player's container so future
    // setInputs calls don't have to resize.
    _inputs[_remotePlayer - 1].resize(remoteIndex - _startIndex, 0, 0);
}

// ============================================================================
// Public wrappers (acquire _mutex, dispatch to *Locked helper)
// ============================================================================
//
// Convention (see Decision 1 in docs/threading-migration.md Layer 4):
//   - Each public function acquires std::lock_guard on entry
//   - Dispatches to the *Locked helper that does the real work
//   - NEVER calls another public function (would self-deadlock on the
//     non-recursive mutex)
//
// These wrappers are deliberately trivial — every line of business
// logic lives in the *Locked helpers above.

uint32_t NetplayManager::getFrame() const {
    NETMAN_LOCK_GUARD();
    return getFrameLocked();
}

uint32_t NetplayManager::getIndex() const {
    NETMAN_LOCK_GUARD();
    return getIndexLocked();
}

IndexedFrame NetplayManager::getIndexedFrame() const {
    NETMAN_LOCK_GUARD();
    return getIndexedFrameLocked();
}

uint32_t NetplayManager::getRemoteIndex() const {
    NETMAN_LOCK_GUARD();
    return getRemoteIndexLocked();
}

uint32_t NetplayManager::getRemoteFrame() const {
    NETMAN_LOCK_GUARD();
    return getRemoteFrameLocked();
}

IndexedFrame NetplayManager::getRemoteIndexedFrame() const {
    NETMAN_LOCK_GUARD();
    return getRemoteIndexedFrameLocked();
}

int NetplayManager::getRemoteFrameDelta() const {
    NETMAN_LOCK_GUARD();
    return getRemoteFrameDeltaLocked();
}

int NetplayManager::getRemoteFrameDeltaLocked() const {
    if (getIndexLocked() == getRemoteIndexLocked())
        return static_cast<int>(getFrameLocked()) -
               static_cast<int>(getRemoteFrameLocked() + config.delay - config.rollbackDelay);
    return 0;
}

IndexedFrame NetplayManager::getLastChangedFrame() const {
    NETMAN_LOCK_GUARD();
    return getLastChangedFrameLocked();
}

void NetplayManager::clearLastChangedFrame() {
    NETMAN_LOCK_GUARD();
    clearLastChangedFrameLocked();
}

NetplayState NetplayManager::getState() const {
    NETMAN_LOCK_GUARD();
    return getStateLocked();
}

void NetplayManager::setState(NetplayState state) {
    NETMAN_LOCK_GUARD();
    setStateLocked(state);
}

bool NetplayManager::isInGame() const {
    NETMAN_LOCK_GUARD();
    return isInGameLocked();
}

bool NetplayManager::isInRollback() const {
    NETMAN_LOCK_GUARD();
    return isInRollbackLocked();
}

uint16_t NetplayManager::getInput(uint8_t player) {
    NETMAN_LOCK_GUARD();
    return getInputLocked(player);
}

uint16_t NetplayManager::getRawInput(uint8_t player) const {
    NETMAN_LOCK_GUARD();
    return getRawInputLocked(player);
}

uint16_t NetplayManager::getRawInput(uint8_t player, uint32_t frame) const {
    NETMAN_LOCK_GUARD();
    return getRawInputLocked(player, frame);
}

void NetplayManager::setInput(uint8_t player, uint16_t input) {
    NETMAN_LOCK_GUARD();
    setInputLocked(player, input);
}

void NetplayManager::assignInput(uint8_t player, uint16_t input, uint32_t frame) {
    NETMAN_LOCK_GUARD();
    assignInputLocked(player, input, frame);
}

void NetplayManager::assignInput(uint8_t player, uint16_t input, IndexedFrame indexedFrame) {
    NETMAN_LOCK_GUARD();
    assignInputLocked(player, input, indexedFrame);
}

std::optional<PlayerInputs> NetplayManager::getInputs(uint8_t player) const {
    NETMAN_LOCK_GUARD();
    return getInputsLocked(player);
}

void NetplayManager::setInputs(uint8_t player, const PlayerInputs& playerInputs) {
    NETMAN_LOCK_GUARD();
    setInputsLocked(player, playerInputs);
}

std::optional<BothInputs> NetplayManager::getBothInputs(IndexedFrame& pos) const {
    NETMAN_LOCK_GUARD();
    return getBothInputsLocked(pos);
}

void NetplayManager::setBothInputs(const BothInputs& bothInputs) {
    NETMAN_LOCK_GUARD();
    setBothInputsLocked(bothInputs);
}

bool NetplayManager::isRemoteInputReady() const {
    NETMAN_LOCK_GUARD();
    return isRemoteInputReadyLocked();
}

std::shared_ptr<RngState> NetplayManager::getRngState() const {
    NETMAN_LOCK_GUARD();
    return getRngStateLocked();
}

std::shared_ptr<RngState> NetplayManager::getRngState(uint32_t index) const {
    NETMAN_LOCK_GUARD();
    return getRngStateLocked(index);
}

void NetplayManager::setRngState(const RngState& rngState) {
    NETMAN_LOCK_GUARD();
    setRngStateLocked(rngState);
}

bool NetplayManager::isRngStateReady(bool shouldSyncRngState) const {
    NETMAN_LOCK_GUARD();
    return isRngStateReadyLocked(shouldSyncRngState);
}

std::optional<MenuIndex> NetplayManager::getLocalRetryMenuIndex() const {
    NETMAN_LOCK_GUARD();
    return getLocalRetryMenuIndexLocked();
}

void NetplayManager::setRemoteRetryMenuIndex(int8_t menuIndex) {
    NETMAN_LOCK_GUARD();
    setRemoteRetryMenuIndexLocked(menuIndex);
}

std::optional<MenuIndex> NetplayManager::getRetryMenuIndex(uint32_t index) const {
    NETMAN_LOCK_GUARD();
    return getRetryMenuIndexLocked(index);
}

void NetplayManager::setRetryMenuIndex(uint32_t index, int8_t menuIndex) {
    NETMAN_LOCK_GUARD();
    setRetryMenuIndexLocked(index, menuIndex);
}

void NetplayManager::setLocalRetryMenuIndex(int8_t menuIndex) {
    NETMAN_LOCK_GUARD();
    setLocalRetryMenuIndexLocked(menuIndex);
}

uint8_t NetplayManager::getDelay() const {
    NETMAN_LOCK_GUARD();
    return getDelayLocked();
}

void NetplayManager::setDelay(uint8_t delay) {
    NETMAN_LOCK_GUARD();
    if (isInRollbackLocked()) config.rollbackDelay = delay;
    else config.delay = delay;
}

void NetplayManager::setRemoteIndex(uint32_t remoteIndex) {
    NETMAN_LOCK_GUARD();
    setRemoteIndexLocked(remoteIndex);
}

bool NetplayManager::isValidNext(NetplayState state) const {
    NETMAN_LOCK_GUARD();
    return isValidNextLocked(state);
}

} // namespace caster::dll
