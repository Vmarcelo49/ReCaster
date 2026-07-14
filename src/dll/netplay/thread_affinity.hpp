// src/dll/netplay/thread_affinity.hpp
//
// Layer 4 — Subtask 4.9 fallback.
//
// ThreadSanitizer is not available on MinGW (neither Clang-MinGW i686
// nor x86_64; GCC-MinGW lacks the runtime lib). The validation of
// Layer 4 therefore relies on:
//   1. Rigorous code review of the *Locked convention
//   2. The SyncHash desync detection system (runtime safety net)
//   3. The debug-only asserts in this header
//
// These asserts are NO-OPs in Release (NDEBUG) builds. In Debug builds
// they fire on:
//   - enet_host_service() called from a thread other than the
//     NetworkThread's jthread (would race on ENetHost state)
//   - NetplayManager::_mutex held while calling netplay::send* (would
//     block on outbox enqueue while holding the FSM lock — potential
//     deadlock if the outbox's BlockingQueue cv notification ever
//     needs the FSM lock to make progress)
//   - NetworkThread::stop() called from the network jthread itself
//     (would self-join and crash)
//
// Usage: call the check_* functions at the entry points listed below.
// The "set network thread id" hook is called once from NetworkThread::loop.

#pragma once

#include "../common/logger.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

namespace caster::dll::netplay::thread_affinity {

// The thread ID of the NetworkThread's jthread. Set once on loop entry,
// read by check_network_thread_only() in debug builds.
//
// Default-initialized to a sentinel (empty std::thread::id) so that
// checks pass before the NetworkThread is started (e.g. during offline
// mode or before netplay::start()).
inline std::atomic<std::thread::id> g_network_thread_id{};

// True if the calling thread is the NetworkThread jthread.
// Always false before NetworkThread::loop() runs.
inline bool is_network_thread() {
    return std::this_thread::get_id() == g_network_thread_id.load(std::memory_order_acquire);
}

// Called once at the top of NetworkThread::loop(). Records the calling
// thread's ID so that check_network_thread_only() can verify affinity
// later.
inline void set_current_thread_as_network_thread() {
    g_network_thread_id.store(std::this_thread::get_id(), std::memory_order_release);
}

// Called from NetworkThread::stop() after the join completes, so that
// any later (spurious) check_network_thread_only() doesn't match a
// recycled thread ID.
inline void clear_network_thread() {
    g_network_thread_id.store({}, std::memory_order_release);
}

#ifdef NDEBUG
// Release: all checks are no-ops.
inline void check_network_thread_only(const char* /*fn*/) {}
inline void check_not_holding_netman_mutex(const char* /*fn*/) {}
inline void check_mutex_not_held(const char* /*fn*/) {}
// The macro is defined as empty in Release so call sites don't need
// #ifdef guards. The debug version (below) creates a scope-tracking
// object that increments/decrements g_netman_mutex_depth.
#define SCOPED_NETMAN_MUTEX_HELD()  ((void)0)
#define NETMAN_LOCK_GUARD()  std::lock_guard<std::mutex> lock(_mutex)
#else
// Debug: assert thread affinity and lock ordering invariants.

// Asserts that the calling thread is the NetworkThread's jthread. Use
// at the top of any function that touches ENetHost*/ENetPeer* directly.
inline void check_network_thread_only(const char* fn) {
    const auto expected = g_network_thread_id.load(std::memory_order_acquire);
    if (expected != std::thread::id{} &&
        std::this_thread::get_id() != expected) {
        common::logger::err(
            "thread_affinity: '{}' called from non-network thread! "
            "ENet is single-threaded by contract.", fn);
        std::abort();
    }
}

// Recursion-safe mutex-state tracker for NetplayManager::_mutex.
//
// We can't directly query "is this mutex held by anyone" from std::mutex,
// so we keep a thread_local counter that the lock helpers increment on
// acquire and decrement on release. If a function calls netplay::send*
// while the counter is non-zero, we know it's holding the FSM lock and
// abort (would block on the outbox's BlockingQueue push while holding
// the lock, which is a deadlock risk if the network thread needs the
// FSM lock to make progress).
//
// This is opt-in: NetplayManager's public functions call
// SCOPED_NETMAN_MUTEX_HELD() right after acquiring _mutex, and the
// check_not_holding_netman_mutex() asserts in netplay::send* see the
// thread_local state.
inline thread_local int g_netman_mutex_depth = 0;

class netman_mutex_scope {
public:
    netman_mutex_scope()  { ++g_netman_mutex_depth; }
    ~netman_mutex_scope() { --g_netman_mutex_depth; }
};

// Macro to be used inside NetplayManager's public functions, right
// after the lock_guard line:
//   std::lock_guard<std::mutex> lock(_mutex);
//   SCOPED_NETMAN_MUTEX_HELD();
#define SCOPED_NETMAN_MUTEX_HELD() \
    ::caster::dll::netplay::thread_affinity::netman_mutex_scope \
        _netman_mutex_scope_;  // NOLINT

// Asserts that the calling thread is NOT holding NetplayManager::_mutex.
// Use at the top of any function that does blocking work (e.g.
// BlockingQueue::push, which under contention may cv-wait). Calling a
// blocking op while holding the FSM lock is a deadlock risk.
inline void check_not_holding_netman_mutex(const char* fn) {
    if (g_netman_mutex_depth > 0) {
        common::logger::err(
            "thread_affinity: '{}' called while holding "
            "NetplayManager::_mutex — deadlock risk (blocking op "
            "while FSM lock held).", fn);
        std::abort();
    }
}


// Asserts that the calling thread is NOT already holding _mutex.
// Called BEFORE lock_guard in every public NetplayManager function.
// If depth > 0, a *Locked function is calling a public function —
// which would self-deadlock on the non-recursive mutex.
inline void check_mutex_not_held(const char* fn) {
    if (g_netman_mutex_depth > 0) {
        common::logger::err(
            "thread_affinity: SELF-DEADLOCK PREVENTED — '{}' (public) "
            "called while _mutex is already held by this thread. "
            "A *Locked function is calling a public function — "
            "violation of the *Locked convention.", fn);
        std::abort();
    }
}

// Combined macro: pre-check + lock_guard + depth tracking.
// Use in every public NetplayManager function instead of:
//   std::lock_guard<std::mutex> lock(_mutex);
//   SCOPED_NETMAN_MUTEX_HELD();
#define NETMAN_LOCK_GUARD() \
    ::caster::dll::netplay::thread_affinity::check_mutex_not_held(__func__); \
    std::lock_guard<std::mutex> lock(_mutex); \
    SCOPED_NETMAN_MUTEX_HELD()
#endif  // NDEBUG

} // namespace caster::dll::netplay::thread_affinity
