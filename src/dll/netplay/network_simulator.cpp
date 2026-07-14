// src/dll/netplay/network_simulator.cpp
//
// Layer 4 — Network thread foundation (subtask 4.2).
//
// See network_simulator.hpp for the design rationale.

#include "network_simulator.hpp"
#include "../../common/logger.hpp"

#include <cstdlib>
#include <utility>

namespace caster::dll::netplay {

// ============================================================================
// Configuration
// ============================================================================

void NetworkSimulator::configure() {
    const char* lag    = std::getenv("CASTER_SIM_LAG_MS");
    const char* jitter = std::getenv("CASTER_SIM_JITTER_MS");
    const char* loss   = std::getenv("CASTER_SIM_LOSS_PCT");
    const char* seed   = std::getenv("CASTER_SIM_SEED");

    if (lag)    config_.lag_ms    = std::atoi(lag);
    if (jitter) config_.jitter_ms = std::atoi(jitter);
    if (loss)   config_.loss_pct  = std::atoi(loss);

    config_.enabled = (config_.lag_ms > 0 ||
                       config_.jitter_ms > 0 ||
                       config_.loss_pct > 0);

    // Seed the RNG. CASTER_SIM_SEED gives reproducible runs (essential
    // for debugging desyncs — you can re-run with the exact same packet
    // loss/delay pattern). Without it, std::random_device for entropy.
    if (seed) {
        try {
            auto s = static_cast<std::uint32_t>(std::stoul(seed));
            rng_.seed(s);
            common::logger::info(
                "network_simulator: RNG seeded from CASTER_SIM_SEED={}", s);
        } catch (...) {
            // Malformed seed — fall back to random_device with a warning.
            std::random_device rd;
            rng_.seed(rd());
            common::logger::warn(
                "network_simulator: CASTER_SIM_SEED='{}' malformed, "
                "using random seed", seed);
        }
    } else {
        std::random_device rd;
        rng_.seed(rd());
    }

    if (config_.enabled) {
        common::logger::info(
            "network_simulator: enabled — lag={}ms jitter={}ms loss={}%%",
            config_.lag_ms, config_.jitter_ms, config_.loss_pct);
    }
}

// ============================================================================
// Receive path
// ============================================================================

bool NetworkSimulator::shouldDrop() {
    if (!config_.enabled || config_.loss_pct <= 0) return false;
    // Uniform distribution over [0, 100). If the result is < loss_pct,
    // drop the packet.
    std::uniform_int_distribution<int> dist(0, 99);
    return dist(rng_) < config_.loss_pct;
}

std::optional<std::chrono::steady_clock::time_point>
NetworkSimulator::maybeDelay(const PlayerInputs& /*pi*/) {
    if (!config_.enabled) return std::nullopt;
    if (config_.lag_ms <= 0 && config_.jitter_ms <= 0) return std::nullopt;

    int delay_ms = config_.lag_ms;
    if (config_.jitter_ms > 0) {
        // Random ±jitter_ms added to the base delay.
        std::uniform_int_distribution<int> dist(-config_.jitter_ms,
                                                 config_.jitter_ms);
        delay_ms += dist(rng_);
        if (delay_ms < 0) delay_ms = 0;
    }

    if (delay_ms == 0) return std::nullopt;  // deliver now
    return std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);
}

void NetworkSimulator::enqueueDelayed(PlayerInputs pi,
                                      std::chrono::steady_clock::time_point deliver_at) {
    delayQueue_.push_back({std::move(pi), deliver_at});
}

void NetworkSimulator::deliverExpired(std::deque<PlayerInputs>& outbox) {
    if (delayQueue_.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    // The delay queue is naturally ordered by insertion time, which
    // correlates with deliver_at (since deliver_at = now + delay, and
    // delay is bounded). We can't guarantee strict monotonicity with
    // jitter, but in practice the front-of-queue check is sufficient.
    while (!delayQueue_.empty() && delayQueue_.front().deliver_at <= now) {
        outbox.push_back(std::move(delayQueue_.front().msg));
        delayQueue_.pop_front();
    }
}

void NetworkSimulator::clear() {
    while (!delayQueue_.empty()) {
        delayQueue_.pop_front();
    }
}

} // namespace caster::dll::netplay
