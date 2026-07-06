// src/dll/rolling_average.hpp
// Ported from CCCaster lib/RollingAverage.hpp. Template-only, no deps.

#pragma once

namespace caster::dll {

template<typename T, size_t N>
class RollingAverage {
public:
    RollingAverage() { reset(); }
    explicit RollingAverage(T initial) { reset(initial); }

    void set(T value) {
        _sum += value;
        if (_count < N) ++_count;
        else _sum -= _values[_index];
        _average = _sum / _count;
        _values[_index] = value;
        _index = (_index + 1) % N;
    }

    T get() const { return _average; }
    void reset() { _sum = _average = T{}; _index = _count = 0; }
    void reset(T initial) { _sum = _average = initial; _index = _count = 1; }
    size_t count() const { return _count; }
    size_t size() const { return N; }
    bool full() const { return _count == N; }

private:
    T _values[N]{};
    T _sum{};
    T _average{};
    size_t _index = 0;
    size_t _count = 0;
};

} // namespace caster::dll
