// src/dll/inputs_container.hpp
//
// Ported from CCCaster's netplay/InputsContainer.hpp. Template-only.
//
// Maps {transition_index → frame → input} with auto-resize and
// last-changed-frame tracking. Used by NetplayManager for storing both
// players' input history, by RollbackManager to detect divergence
// between predicted and real remote inputs, and by SpectatorManager to
// send input windows to spectators.
//
// Semantics (faithful to CCCaster):
//
// - `set(index, frame, value)` CANNOT change an existing input — if the
//   slot already has a value, the call is a no-op. This is the contract
//   the NetplayManager relies on for the local player's inputs: once a
//   frame's input is committed, it's immutable.
//
// - `assign(index, frame, value)` CAN change an existing input — used by
//   the rollback re-run path to overwrite predicted inputs with the
//   actual remote inputs.
//
// - `get(index, frame)` returns the last known input at or before the
//   requested position. If `index` is beyond the end of known indices,
//   returns `lastInputBefore(index)` (the back of the last non-empty
//   index, or 0 if none). If `frame` is beyond the end of `index`,
//   returns `_inputs[index].back()`. This avoids "phantom input drops"
//   when a remote frame hasn't arrived yet — the player keeps doing what
//   they were doing, which is the correct GGPO-style prediction.
//
// - `_lastChangedFrame` is a single `IndexedFrame` (not per-index), set
//   to `MaxIndexedFrame` by `clearLastChangedFrame()`. The rollback loop
//   uses it as the trigger: if `getLastChangedFrame() < getIndexedFrame()`,
//   a remote input arrived that disagrees with what we predicted, so we
//   must roll back.
//
// - The `set(index, frame, T* t, n, checkStartingFromIndex)` overload
//   scans the new inputs against the existing ones starting at
//   `checkStartingFromIndex` and updates `_lastChangedFrame` to the
//   earliest frame where the new value differs from the existing one.
//   This is how the NetplayManager detects that a remote PlayerInputs
//   message diverges from the locally-predicted input.

#pragma once

#include "constants.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace caster::dll {

template<typename T>
class InputsContainer {
public:
    // ---- Single-element read ----

    // Get a single input for the given index:frame.
    // Returns `lastInputBefore(index)` if the index is empty/unknown,
    // or `_inputs[index].back()` if `frame` is beyond the end of the
    // known inputs for that index. Returns 0 if no input is known at all.
    T get(uint32_t index, uint32_t frame) const {
        if (index >= _inputs.size() || _inputs[index].empty())
            return lastInputBefore(index);

        if (frame >= _inputs[index].size())
            return _inputs[index].back();

        return _inputs[index][frame];
    }

    // ---- Batch read ----

    // Get n inputs starting from the given index:frame.
    // Caller MUST ensure that `index < _inputs.size()` and
    // `frame + n <= _inputs[index].size()` — this is an ASSERT in the
    // CCCaster and is used by the spectator broadcast path where the
    // caller has already bounds-checked via getEndFrame().
    void get(uint32_t index, uint32_t frame, T* t, size_t n) const {
        // Caller-contract assert; in RELEASE the CCCaster keeps the ASSERT
        // but we degrade to a no-op to avoid crashing the game on a
        // protocol bug.
        if (index >= _inputs.size() || frame + n > _inputs[index].size())
            return;

        std::copy(_inputs[index].begin() + frame,
                  _inputs[index].begin() + frame + n, t);
    }

    // ---- Single-element write (immutable) ----

    // Set a single input for the given index:frame. CANNOT change an
    // existing input — if the slot already has a value, this is a no-op.
    // Use `assign()` to overwrite.
    void set(uint32_t index, uint32_t frame, T t) {
        if (_inputs.size() > index && _inputs[index].size() > frame)
            return;  // Already set; immutable.

        resize(index, frame);

        _inputs[index][frame] = t;
    }

    // ---- Single-element write (mutable) ----

    // Assign a single input for the given index:frame. CAN change an
    // existing input — used by the rollback re-run path.
    void assign(uint32_t index, uint32_t frame, T t) {
        resize(index, frame);

        _inputs[index][frame] = t;
    }

    // ---- Batch write: fill ----

    // Fill n inputs with the same value starting from the given
    // index:frame. CAN change existing inputs. Used by the NetplayManager
    // when seeding predicted inputs.
    void set(uint32_t index, uint32_t frame, T t, size_t n) {
        resize(index, frame, n);

        std::fill(_inputs[index].begin() + frame,
                  _inputs[index].begin() + frame + n, t);
    }

    // ---- Batch write: copy with divergence detection ----

    // Set n inputs starting from the given index:frame. CAN change
    // existing inputs. If `index >= checkStartingFromIndex`, scans the
    // new inputs against the existing ones and updates
    // `_lastChangedFrame` to the earliest frame where the new value
    // differs from the existing one. This is how the NetplayManager
    // detects that a remote PlayerInputs message diverges from the
    // locally-predicted input — the rollback loop then triggers on
    // `getLastChangedFrame() < getIndexedFrame()`.
    void set(uint32_t index, uint32_t frame, const T* t, size_t n,
             uint32_t checkStartingFromIndex = UINT32_MAX) {
        if (index >= checkStartingFromIndex) {
            IndexedFrame f;
            f.parts.frame = frame;
            f.parts.index = index;
            for (size_t i = 0; i < n; ++i, ++f.parts.frame) {
                if (get(f.parts.index, f.parts.frame) == t[i])
                    continue;

                // Indicate changed if the input is different from the
                // last known input.
                _lastChangedFrame.value = std::min(_lastChangedFrame.value,
                                                    f.value);
                break;
            }
        }

        resize(index, frame, n);

        std::copy(t, t + n, &_inputs[index][frame]);
    }

    // ---- Capacity ----

    // Resize the container so that it can contain inputs up to
    // index:frame+n. New slots are filled with the last known input
    // (or 0 if there is no known input), matching the prediction
    // semantics of `get()`.
    void resize(uint32_t index, uint32_t frame, size_t n = 1) {
        T last = 0;

        if (index >= _inputs.size()) {
            last = lastInputBefore(_inputs.size());
            _inputs.resize(index + 1);
        } else if (!_inputs[index].empty()) {
            last = _inputs[index].back();
        }

        if (frame + n > _inputs[index].size())
            _inputs[index].resize(frame + n, last);
    }

    // ---- Queries ----

    void clear() { _inputs.clear(); }

    bool empty() const { return _inputs.empty(); }

    bool empty(uint32_t index) const {
        if (index >= _inputs.size())
            return true;
        return _inputs[index].empty();
    }

    // Number of indices stored (one past the last valid index).
    uint32_t getEndIndex() const {
        return static_cast<uint32_t>(_inputs.size());
    }

    // Frame count of the last index. Returns 0 if empty.
    uint32_t getEndFrame() const {
        if (_inputs.empty())
            return 0;
        return static_cast<uint32_t>(_inputs.back().size());
    }

    // Frame count of the given index. Returns 0 if index is unknown/empty.
    uint32_t getEndFrame(uint32_t index) const {
        if (index >= _inputs.size())
            return 0;
        return static_cast<uint32_t>(_inputs[index].size());
    }

    // ---- Index pruning ----

    // Erase all indices strictly older than `index`. Used by the
    // NetplayManager when transitioning into Loading to garbage-collect
    // inputs from previous rounds that are no longer reachable.
    void eraseIndexOlderThan(uint32_t index) {
        if (index + 1 >= _inputs.size())
            _inputs.clear();
        else
            _inputs.erase(_inputs.begin(),
                          _inputs.begin() + static_cast<ptrdiff_t>(index));
    }

    // ---- Rollback trigger ----

    // The earliest {index, frame} at which a remote input disagreed with
    // the locally-predicted input. The rollback loop checks
    // `getLastChangedFrame() < getIndexedFrame()` and, if true, rolls
    // back to `getLastChangedFrame()`.
    IndexedFrame getLastChangedFrame() const { return _lastChangedFrame; }

    // Reset the rollback trigger. Called after a rollback completes
    // (so we don't immediately re-trigger) and at the start of each
    // frame after `minRollbackSpacing` has elapsed (so we can detect
    // the next divergence).
    void clearLastChangedFrame() { _lastChangedFrame = MaxIndexedFrame; }

private:
    // Mapping: transition_index → frame → input.
    std::vector<std::vector<T>> _inputs;

    // Earliest {index, frame} where a remote input diverged from the
    // prediction. `MaxIndexedFrame` means "no divergence detected".
    IndexedFrame _lastChangedFrame = MaxIndexedFrame;

    // Get the last known input BEFORE the given index. Returns 0 if
    // there are no inputs at any earlier index. Used by `get()` when
    // the requested index is empty/unknown, and by `resize()` when
    // growing into a new index (so the new slots inherit the previous
    // player's last input as the prediction).
    T lastInputBefore(uint32_t index) const {
        if (_inputs.empty() || index == 0)
            return T{};

        if (index > _inputs.size())
            index = static_cast<uint32_t>(_inputs.size());

        do {
            --index;
            if (!_inputs[index].empty())
                return _inputs[index].back();
        } while (index > 0);

        return T{};
    }
};

} // namespace caster::dll
