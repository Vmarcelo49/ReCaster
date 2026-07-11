// src/common/concurrency.hpp
//
// Concurrency primitives for the multi-threaded launcher.
//
// Layer 0 of the threading migration (see docs/threading-migration.md).
// This header is the foundation: a thread-safe `BlockingQueue<T>` built
// on `std::mutex` + `std::condition_variable_any` with first-class
// support for `std::stop_token` (so workers based on `std::jthread` can
// shut down cleanly without dangling on `wait_and_pop`).
//
// Design principles (from the C++23 threading guide):
//   - Communicate via queues, not shared mutable state.
//   - RAII locking only (lock_guard / unique_lock).
//   - `std::condition_variable_any` (not `condition_variable`) because
//     only the `_any` variant supports `stop_token` in `wait()`.
//   - No `volatile` for cross-thread signaling.
//
// This header is header-only and has no dependencies beyond the C++23
// standard library. Safe to include from any TU.

#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <stop_token>
#include <utility>

namespace caster::common::concurrency {

// A thread-safe FIFO queue for passing work/items between threads.
//
// Typical use: one producer thread pushes commands, one worker thread
// (usually a `std::jthread`) drains them in its loop. The worker calls
// `wait_and_pop(item, stop_token)` which blocks until either an item
// arrives or the stop token is requested — whichever comes first.
//
// Thread-safety: all public methods are safe to call concurrently from
// multiple threads. The queue is a single producer / single consumer by
// convention, but the implementation supports multiple producers and
// multiple consumers if needed.
template <typename T>
class BlockingQueue {
public:
    BlockingQueue() = default;
    ~BlockingQueue() = default;

    BlockingQueue(const BlockingQueue&)            = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;
    BlockingQueue(BlockingQueue&&)                 = delete;
    BlockingQueue& operator=(BlockingQueue&&)      = delete;

    // Push an item onto the queue. Notifies one waiting consumer.
    // Thread-safe; can be called from multiple producers.
    void push(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(item));
        }
        cv_.notify_one();
    }

    // Try to pop an item without blocking. Returns true if an item was
    // popped (written to `out`), false if the queue was empty.
    // Thread-safe.
    bool try_pop(T& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    // Try to pop an item, returning it in an optional. Convenience
    // wrapper around `try_pop(T&)` for callers that prefer the optional
    // style.
    std::optional<T> try_pop() {
        T tmp;
        if (try_pop(tmp)) return std::move(tmp);
        return std::nullopt;
    }

    // Block until either an item is available or `stop_token` is
    // requested. Returns true if an item was popped (written to `out`),
    // false if the stop was requested (in which case the queue may or
    // may not be empty — the caller should drain remaining items with
    // `try_pop` if it cares).
    //
    // The `std::condition_variable_any::wait` overload that accepts a
    // `stop_token` wakes up immediately when stop is requested, so this
    // call never deadlocks during shutdown.
    bool wait_and_pop(T& out, std::stop_token stop_token) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, stop_token, [this] { return !queue_.empty(); });
        // `wait` returns false if it was woken by stop_requested.
        if (stop_token.stop_requested() && queue_.empty()) {
            return false;
        }
        // Either we have an item, or stop was requested but there are
        // still items to drain. Either way, pop what's available.
        if (queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    // Remove all items from the queue. Useful during shutdown to release
    // any pending items' resources. Thread-safe.
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!queue_.empty()) {
            queue_.pop();
        }
    }

    // Returns true if the queue is currently empty. Note that the result
    // may be stale by the time the caller reads it — use only for
    // advisory checks (e.g. logging), not for synchronization.
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    // Returns the current number of items. Same caveat as `empty()`.
    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex               mutex_;
    std::condition_variable_any      cv_;
    std::queue<T>                    queue_;
};

} // namespace caster::common::concurrency
