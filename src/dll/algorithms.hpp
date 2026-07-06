// src/dll/algorithms.hpp
// Ported from CCCaster lib/Algorithms.hpp. Adapted for C++23.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace caster::dll {

template<typename T>
inline T sorted(const T& list) {
    T result(list);
    std::sort(result.begin(), result.end());
    return result;
}

template<typename T, typename F>
inline T sorted(const T& list, const F& compare) {
    T result(list);
    std::sort(result.begin(), result.end(), compare);
    return result;
}

template<typename T>
inline T clamped(T value, T min, T max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

inline bool isPowerOfTwo(uint32_t x) {
    return (x != 0) && ((x & (x - 1)) == 0);
}

inline std::string generateRandomId() {
    std::string id;
    for (int i = 0; i < 10; ++i) {
        id += static_cast<char>('A' + (rand() % 26));
        id += static_cast<char>('a' + (rand() % 26));
        id += static_cast<char>('0' + (rand() % 10));
    }
    return id;
}

inline double getNegativeQuadraticScale(size_t i, size_t count) {
    return 1.0 - std::pow((double(i) / count) - 1, 2.0);
}

template<typename T>
inline T incremented(T x) { ++x; return x; }

} // namespace caster::dll
