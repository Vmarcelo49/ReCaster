// src/dll/netplay/debug_log.hpp
//
// Structured netplay debug logger. Writes to a SEPARATE file from the main
// debug.log so host and joiner logs don't interleave.
//
// Files:
//   caster/host_debug.log   — DLL running as host
//   caster/join_debug.log   — DLL running as joiner
//
// Two log modes:
//   1. log_frame() — compact one-liner per frameStep, throttled adaptively.
//      Format: "F <tick> | st=<state> idx=<i> frm=<f> | rmt:idx=<i> frm=<f> lcf=<i>/<f> | spin:<pass|block>(<ms>) | rb:<action> | in:<p1> <p2>"
//
//   2. log_event() — critical events, always logged immediately.
//      Format: "EVENT <type> <key=value ...>"
//
// Usage:
//   netplay_debug::init(true);  // host
//   netplay_debug::log_frame(tick, netMan, p1, p2, "save", 2);
//   netplay_debug::log_event("rollback-trigger", "target", 4, 78);
//
// Throttling:
//   - Stable InGame (no rollback, no spin-block): every 60 frames (~1s)
//   - During rollback/rerun: every frame
//   - During spin-lock block: caller handles its own throttle
//   - State transitions: always (caller uses log_event)

#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace caster::dll::netplay_debug {

// Forward-declare — we only need public methods.
class NetplayManager;

namespace {

struct DebugState {
    std::mutex mtx;
    std::ofstream file;
    bool initialized = false;
    bool is_host = false;
    uint32_t frame_tick = 0;       // monotonic frameStep counter
    uint32_t last_logged_frame = 0; // for throttle: last frame tick we logged
    uint32_t since_flush = 0;      // lines since last flush
    bool was_in_rollback = false;   // for transition detection
    bool was_in_rerun = false;      // for transition detection
    // Per-InGame-entry frame counter. Resets to 0 on each InGame entry
    // (driven by state-transition detection in log_frame). Used to force
    // per-frame logging during the first ~5s of InGame — the desync
    // window where the original report says the bug fires.
    uint32_t ingame_tick = 0;
    uint32_t last_state = 0xFFFF;  // last NetplayState enum value seen
};

DebugState& state() {
    static DebugState s;
    return s;
}

std::filesystem::path debug_log_path(bool is_host) {
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len > 0) {
        std::filesystem::path exePath(buf);
        auto dir = exePath.parent_path() / "caster";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir / (is_host ? "host_debug.log" : "join_debug.log");
    }
#endif
    return std::filesystem::current_path() / (is_host ? "host_debug.log" : "join_debug.log");
}

} // namespace

// Initialize the debug logger. Call once after the host/joiner role is
// determined (in doIpcAndModePatch, after g_isHost is set).
inline void init(bool is_host) {
    DebugState& s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.is_host = is_host;
    s.file.open(debug_log_path(is_host), std::ios::app | std::ios::binary);
    s.initialized = s.file.is_open();
    if (s.initialized) {
        s.file << "\n========================================"
                  "========================================\n"
               << "[session start] role=" << (is_host ? "HOST" : "JOINER") << "\n";
        s.file.flush();
    }
}

// Log a critical event (always logged, no throttle).
// Example: log_event("rollback-trigger", "target_idx", 4, "target_frm", 78)
template <typename... Args>
inline void log_event(std::string_view type, Args&&... args) {
    DebugState& s = state();
    if (!s.initialized) return;
    std::lock_guard<std::mutex> lk(s.mtx);
    s.file << "EVENT " << type;
    ((s.file << ' ' << std::forward<Args>(args)), ...);
    s.file << '\n';
    if (++s.since_flush >= 10) { s.file.flush(); s.since_flush = 0; }
}

// Convenience: log_event with a single formatted string.
inline void log_event_str(std::string_view type, std::string_view detail) {
    DebugState& s = state();
    if (!s.initialized) return;
    std::lock_guard<std::mutex> lk(s.mtx);
    s.file << "EVENT " << type << ' ' << detail << '\n';
    if (++s.since_flush >= 10) { s.file.flush(); s.since_flush = 0; }
}

// Log a compact frame summary. Called at the end of frameStep().
//
// Parameters:
//   tick             — monotonic frameStep counter (0, 1, 2, ...)
//   state_str        — netplayStateStr(state) — short state name
//   idx, frm         — local indexedFrame (getIndex(), getFrame())
//   rmt_idx, rmt_frm — remote indexedFrame (best known)
//   lcf_idx, lcf_frm — getLastChangedFrame (0/0 = no divergence)
//   p1_input         — combined input written to P1 (0x0000 if not written)
//   p2_input         — combined input written to P2
//   rollback_action  — "save", "load", "rerun", "none"
//   spin_ms          — time spent in spin-lock this frame (0 if passed immediately)
//   force_log        — if true, bypass throttle (used during rollback/rerun)
inline void log_frame(uint32_t tick,
                      std::string_view state_str,
                      uint32_t idx, uint32_t frm,
                      uint32_t rmt_idx, uint32_t rmt_frm,
                      uint32_t lcf_idx, uint32_t lcf_frm,
                      uint16_t p1_input, uint16_t p2_input,
                      std::string_view rollback_action,
                      uint32_t spin_ms,
                      bool force_log) {
    DebugState& s = state();
    if (!s.initialized) return;

    // Reset the per-InGame counter on entry. NetplayState::InGame is
    // typically 8 in the enum (check source), but we use string match
    // here so the debug log doesn't need the enum value.
    if (state_str == "InGame") {
        if (s.last_state != 8 && s.last_state != 0xFFFF) {
            // Just entered InGame — start the high-resolution window.
            s.ingame_tick = 0;
        }
        ++s.ingame_tick;
    }
    s.last_state = (state_str == "PreInitial") ? 0 :
                   (state_str == "Initial")    ? 1 :
                   (state_str == "AutoCharaSelect") ? 2 :
                   (state_str == "CharaSelect") ? 3 :
                   (state_str == "Loading")     ? 4 :
                   (state_str == "CharaIntro")  ? 5 :
                   (state_str == "Skippable")   ? 6 :
                   (state_str == "RetryMenu")   ? 7 :
                   (state_str == "InGame")      ? 8 :
                   (state_str == "ReplayMenu")  ? 9 : 0xFFFF;

    // High-resolution window: log EVERY frame during the first 5s of
    // InGame (300 frames @ 60fps). This is where the desync fires per
    // the issue report. After the window, fall back to the throttle.
    const bool in_desync_window =
        (state_str == "InGame" && s.ingame_tick <= 300);

    // Adaptive throttle:
    // - in_desync_window (first 5s of InGame): always log
    // - force_log (rollback/rerun active): always log
    // - spin_ms > 0 (blocked this frame): always log
    // - Stable InGame: every 60 frames (~1s)
    // - Non-InGame states: every 30 frames
    if (!in_desync_window && !force_log && spin_ms == 0) {
        uint32_t interval = (state_str == "InGame") ? 60 : 30;
        if (tick - s.last_logged_frame < interval) return;
    }
    s.last_logged_frame = tick;

    std::lock_guard<std::mutex> lk(s.mtx);
    // Prefix with `i{N}` when inside the InGame desync window so it's
    // easy to grep the high-resolution frames out of the log.
    if (in_desync_window) {
        s.file << std::format("i{} F {} | st={} idx={} frm={} | rmt:idx={} frm={} lcf={}/{} | ",
                              s.ingame_tick, tick, state_str, idx, frm,
                              rmt_idx, rmt_frm, lcf_idx, lcf_frm);
    } else {
        s.file << std::format("F {} | st={} idx={} frm={} | rmt:idx={} frm={} lcf={}/{} | ",
                              tick, state_str, idx, frm,
                              rmt_idx, rmt_frm, lcf_idx, lcf_frm);
    }
    if (spin_ms > 0)
        s.file << std::format("spin:block({}ms) | ", spin_ms);
    else
        s.file << "spin:pass | ";
    s.file << std::format("rb:{} | in:{:#06x} {:#06x}\n",
                          rollback_action, p1_input, p2_input);
    if (++s.since_flush >= 10) { s.file.flush(); s.since_flush = 0; }
}

// Log the actual RNG state values (not just an xxHash) for frame-by-frame
// comparison between host and joiner. Used to find the FIRST frame where
// RNG diverges — which pinpoints the rollback/loadState bug.
//
// Format: RNG F <tick> | idx=<i> frm=<f> | r0=0x... r1=0x... r2=0x... r3=<32 hex chars>
//         [rerun] [rngSync=<flag>]
//
// The `tag` parameter is a short label like "periodic", "pre-save",
// "post-save", "pre-load", "post-load" so we can see the RNG state at
// each critical point in the frame.
inline void log_rng(uint32_t tick, uint32_t idx, uint32_t frm,
                    uint32_t r0, uint32_t r1, uint32_t r2,
                    const uint8_t* r3_bytes, size_t r3_len,
                    std::string_view tag,
                    bool in_rerun, bool rng_sync_flag) {
    DebugState& s = state();
    if (!s.initialized) return;
    std::lock_guard<std::mutex> lk(s.mtx);
    // Log first 16 bytes of r3 (enough to detect any divergence without
    // flooding the log — r3 is 220 bytes).
    char r3_hex[33];
    static constexpr const char* hex = "0123456789abcdef";
    size_t hex_idx = 0;
    size_t to_log = r3_len < 16 ? r3_len : 16;
    for (size_t i = 0; i < to_log; ++i) {
        r3_hex[hex_idx++] = hex[r3_bytes[i] >> 4];
        r3_hex[hex_idx++] = hex[r3_bytes[i] & 0x0F];
    }
    r3_hex[hex_idx] = '\0';
    s.file << std::format("RNG F {} | idx={} frm={} | r0=0x{:08x} r1=0x{:08x} r2=0x{:08x} r3={} | tag={} rerun={} rngSync={}\n",
                          tick, idx, frm, r0, r1, r2, r3_hex,
                          tag, in_rerun ? 1 : 0, rng_sync_flag ? 1 : 0);
    if (++s.since_flush >= 10) { s.file.flush(); s.since_flush = 0; }
}

} // namespace caster::dll::netplay_debug
