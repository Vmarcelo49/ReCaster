// src/dll/timer.hpp
// Ported from CCCaster lib/Timer.hpp + lib/TimerManager.hpp.
// Simplified for C++23: uses std::chrono instead of QPC/timeGetTime.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <unordered_set>

namespace caster::dll {

// Simple timer manager using std::chrono::steady_clock.
// Singleton — call initialize() once, then check() each frame.

class Timer {
public:
    using Callback = std::function<void(Timer*)>;

    Callback callback;

    Timer() = default;
    explicit Timer(Callback cb) : callback(std::move(cb)) {}

    void start(uint64_t delay_ms) { delay_ = delay_ms; expiry_ = 0; }
    void stop() { delay_ = expiry_ = 0; }
    uint64_t getDelay() const { return delay_; }
    bool isStarted() const { return delay_ > 0 || expiry_ > 0; }

private:
    uint64_t delay_ = 0;
    uint64_t expiry_ = 0;

    friend class TimerManager;
};

class TimerManager {
public:
    static TimerManager& get() {
        static TimerManager instance;
        return instance;
    }

    void initialize() { initialized_ = true; updateNow(); }
    void deinitialize() { initialized_ = false; clear(); }
    bool isInitialized() const { return initialized_; }

    uint64_t getNow() const { return now_; }
    uint64_t getNow(bool update) { if (update) updateNow(); return now_; }
    uint64_t getNextExpiry() const { return nextExpiry_; }

    void add(Timer* t) { allocated_.insert(t); changed_ = true; }
    void remove(Timer* t) { allocated_.erase(t); changed_ = true; }
    void clear() { active_.clear(); allocated_.clear(); changed_ = true; }

    void updateNow() {
        now_ = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void check() {
        if (!initialized_) return;

        if (changed_) {
            for (auto* t : allocated_) {
                if (active_.find(t) == active_.end()) active_.insert(t);
            }
            for (auto it = active_.begin(); it != active_.end();) {
                if (allocated_.find(*it) == allocated_.end())
                    it = active_.erase(it);
                else
                    ++it;
            }
            changed_ = false;
        }

        nextExpiry_ = UINT64_MAX;
        if (active_.empty()) return;

        updateNow();
        for (auto* t : active_) {
            if (allocated_.find(t) == allocated_.end()) continue;

            if (t->expiry_ > 0 && now_ >= t->expiry_) {
                t->delay_ = t->expiry_ = 0;
                if (t->callback) t->callback(t);
                if (allocated_.find(t) == allocated_.end()) continue;
            }

            if (t->delay_ > 0) {
                t->expiry_ = now_ + t->delay_;
                t->delay_ = 0;
            }

            if (t->expiry_ > 0 && t->expiry_ < nextExpiry_)
                nextExpiry_ = t->expiry_;
        }
    }

private:
    TimerManager() = default;
    std::unordered_set<Timer*> active_;
    std::unordered_set<Timer*> allocated_;
    uint64_t now_ = 0;
    uint64_t nextExpiry_ = 0;
    bool changed_ = false;
    bool initialized_ = false;
};

} // namespace caster::dll
