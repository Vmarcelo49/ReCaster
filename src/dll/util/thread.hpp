// src/dll/util/thread.hpp
// Ported from CCCaster lib/Thread.hpp. Uses std::mutex/std::thread (C++23).

#pragma once

#include <mutex>
#include <thread>
#include <condition_variable>
#include <memory>

namespace caster::dll {

using Mutex = std::recursive_mutex;
using Lock = std::lock_guard<Mutex>;

class CondVar {
public:
    void wait(Mutex& m) { std::unique_lock<std::recursive_mutex> lk(m, std::defer_lock); cv_.wait(lk); }
    bool wait(Mutex& m, long timeout_ms) {
        std::unique_lock<std::recursive_mutex> lk(m, std::defer_lock);
        return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms)) == std::cv_status::no_timeout;
    }
    void signal() { cv_.notify_one(); }
    void broadcast() { cv_.notify_all(); }
private:
    std::condition_variable_any cv_;
};

class Thread {
public:
    virtual ~Thread() { join(); }
    virtual void start() {
        std::lock_guard<Mutex> lk(mutex_);
        if (running_) return;
        running_ = true;
        thread_ = std::thread(&Thread::run, this);
    }
    virtual void join() {
        std::lock_guard<Mutex> lk(mutex_);
        if (!running_) return;
        if (thread_.joinable()) thread_.join();
        running_ = false;
    }
    void release() { std::lock_guard<Mutex> lk(mutex_); running_ = false; }
    bool isRunning() const { std::lock_guard<Mutex> lk(mutex_); return running_; }
    virtual void run() = 0;
private:
    bool running_ = false;
    std::thread thread_;
    mutable Mutex mutex_;
};

using ThreadPtr = std::shared_ptr<Thread>;

} // namespace caster::dll
