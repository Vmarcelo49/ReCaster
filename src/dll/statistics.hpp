// src/dll/statistics.hpp
// Ported from CCCaster lib/Statistics.hpp. Removed cereal/SerializableSequence.
// Uses Welford's online algorithm for variance.

#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdint>

namespace caster::dll {

class Statistics {
public:
    template<typename T>
    void addSample(T value) {
        if (value > _worst) _worst = value;
        double delta = value - _mean;
        ++_count;
        _mean += delta / _count;
        _sumOfSquaredDeltas += delta * (value - _mean);
    }

    void reset() {
        _count = 0;
        _worst = -std::numeric_limits<double>::infinity();
        _mean = _sumOfSquaredDeltas = 0.0;
    }

    size_t getNumSamples() const { return _count; }
    double getWorst() const { return _worst; }
    double getMean() const { return _count < 1 ? 0.0 : _mean; }

    double getVariance() const {
        if (_count < 2) return 0.0;
        return _sumOfSquaredDeltas / (_count - 1);
    }

    double getStdDev() const { return std::sqrt(getVariance()); }

    double getStdErr() const {
        if (_count < 2) return 0.0;
        return getStdDev() / std::sqrt(static_cast<double>(_count));
    }

    void merge(const Statistics& stats) {
        _worst = std::max(_worst, stats._worst);
        if (_count + stats._count > 0)
            _mean = (_mean * _count + stats._mean * stats._count) / (_count + stats._count);
        _sumOfSquaredDeltas += stats._sumOfSquaredDeltas;
        _count += stats._count;
    }

private:
    size_t _count = 0;
    double _worst = -std::numeric_limits<double>::infinity();
    double _mean = 0.0;
    double _sumOfSquaredDeltas = 0.0;
};

} // namespace caster::dll
