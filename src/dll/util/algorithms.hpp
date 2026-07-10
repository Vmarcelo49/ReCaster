// src/dll/util/algorithms.hpp
// Ported from CCCaster lib/Algorithms.hpp. Adapted for C++23.

#pragma once

#include <algorithm>
#include <vector>

namespace caster::dll {

template<typename T, typename F>
inline T sorted(const T& list, const F& compare) {
    // Sort via pointers to avoid requiring MoveAssignable (MemDump has const members).
    std::vector<typename T::const_pointer> ptrs;
    ptrs.reserve(list.size());
    for (const auto& x : list)
        ptrs.push_back(&x);
    std::sort(ptrs.begin(), ptrs.end(),
              [&](typename T::const_pointer a, typename T::const_pointer b) { return compare(*a, *b); });
    T result;
    result.reserve(ptrs.size());
    for (const auto& x : ptrs)
        result.push_back(*x);
    return result;
}

template<typename T>
inline T incremented(T x) { ++x; return x; }

} // namespace caster::dll
