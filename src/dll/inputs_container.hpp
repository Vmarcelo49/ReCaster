// src/dll/inputs_container.hpp
// Ported from CCCaster netplay/InputsContainer.hpp. Template-only.
// Maps index → frame → input with auto-resize and last-changed tracking.

#pragma once

#include "constants.hpp"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace caster::dll {

template<typename T>
class InputsContainer {
public:
    // Get the input at the given index and frame.
    T get(uint32_t index, uint32_t frame) const {
        auto it = _data.find(index);
        if (it == _data.end()) return T{};
        if (frame >= it->second.size()) return T{};
        return it->second[frame];
    }

    // Set the input at the given index and frame.
    void set(uint32_t index, uint32_t frame, T value) {
        auto& vec = _data[index];
        if (frame >= vec.size()) {
            vec.resize(frame + NUM_INPUTS, T{});
        }
        if (vec[frame] != value) {
            vec[frame] = value;
            _lastChangedFrame[index] = frame;
        }
    }

    // Assign a range of inputs starting at a frame.
    void assign(uint32_t index, uint32_t frame, const T* inputs, size_t count) {
        auto& vec = _data[index];
        if (frame + count > vec.size()) {
            vec.resize(frame + count + NUM_INPUTS, T{});
        }
        for (size_t i = 0; i < count; ++i) {
            if (vec[frame + i] != inputs[i]) {
                vec[frame + i] = inputs[i];
                _lastChangedFrame[index] = frame + i;
            }
        }
    }

    // Get the end index (max index with data).
    uint32_t getEndIndex() const {
        uint32_t max = 0;
        for (const auto& [idx, _] : _data) {
            if (idx > max) max = idx;
        }
        return max;
    }

    // Get the end frame for an index.
    uint32_t getEndFrame(uint32_t index) const {
        auto it = _data.find(index);
        if (it == _data.end()) return 0;
        return it->second.empty() ? 0 : static_cast<uint32_t>(it->second.size() - 1);
    }

    // Get the last changed frame for an index.
    uint32_t getLastChangedFrame(uint32_t index) const {
        auto it = _lastChangedFrame.find(index);
        return it != _lastChangedFrame.end() ? it->second : 0;
    }

    void clearLastChangedFrame(uint32_t index) {
        _lastChangedFrame.erase(index);
    }

    // Erase indices older than the given index.
    void eraseIndexOlderThan(uint32_t index) {
        for (auto it = _data.begin(); it != _data.end();) {
            if (it->first < index) it = _data.erase(it);
            else ++it;
        }
    }

    // Resize to hold at least `frame + NUM_INPUTS` for the given index.
    void resize(uint32_t index, uint32_t frame) {
        auto& vec = _data[index];
        if (frame + NUM_INPUTS > vec.size()) {
            vec.resize(frame + NUM_INPUTS, T{});
        }
    }

    // Clear all data.
    void clear() {
        _data.clear();
        _lastChangedFrame.clear();
    }

    // Get a pointer to the raw input array for an index (for serialization).
    const std::vector<T>* getInputs(uint32_t index) const {
        auto it = _data.find(index);
        return it != _data.end() ? &it->second : nullptr;
    }

    // Set raw inputs for an index.
    void setInputs(uint32_t index, std::vector<T> inputs) {
        _data[index] = std::move(inputs);
    }

private:
    std::unordered_map<uint32_t, std::vector<T>> _data;
    std::unordered_map<uint32_t, uint32_t> _lastChangedFrame;
};

} // namespace caster::dll
