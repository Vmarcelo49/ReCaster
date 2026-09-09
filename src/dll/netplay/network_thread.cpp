// src/dll/netplay/network_thread.cpp
//
// Layer 4 — Network thread foundation.
//
// Subtasks 4.1 + 4.2 + 4.3: NetworkThread owns the ENetHost*, runs a
// dedicated jthread that calls enet_host_service() in a loop, routes
// received packets to the matching inbox BlockingQueue, drains the
// outbox queue and sends via enet_peer_send, and runs the
// NetworkSimulator for testing rollback under laggy conditions.
//
// The game thread communicates exclusively via the inbox queues
// (Network → Game) and the outbox queue (Game → Network). The game
// thread NEVER touches ENet directly.

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "network_thread.hpp"
#include "protocol/decoder.hpp"
#include "thread_affinity.hpp"
#include "../spec/spectator_manager.hpp"
#include "../../common/logger.hpp"

#include <enet/enet.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>

namespace caster::dll::netplay {

NetworkThread::~NetworkThread() {
    // The jthread auto-joins on destruction via stop_token, but we want
    // explicit cleanup of ENetHost. If stop() wasn't called yet, do it
    // now. This is a safety net — DLL_PROCESS_DETACH should call stop()
    // explicitly for clean ordering.
    if (thread_.joinable()) {
        stop();
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

void NetworkThread::start(const caster::common::ipc::config_buffer::Config& cfg) {
    if (thread_.joinable()) {
        common::logger::warn("network_thread: start() called twice — ignoring");
        return;
    }

    isNetplay_ = cfg.is_netplay();
    isHost_    = cfg.is_host();
    localPort_ = cfg.local_udp_port;
    peerAddr_  = cfg.peer_addr;
    peerPort_  = cfg.peer_port;

    // Read simulator config from env vars BEFORE spawning the jthread.
    // After this returns, sim_ is read-only from the network thread's
    // perspective (establishes happens-before).
    sim_.configure();

    if (!isNetplay_) {
        common::logger::info("network_thread: offline mode — no ENet host, no jthread");
        return;
    }

    common::logger::info(
        "network_thread: starting (host={} bind_port={} peer={}:{})",
        isHost_, localPort_, peerAddr_, peerPort_);

    if (enet_initialize() != 0) {
        common::logger::err("network_thread: enet_initialize failed");
        isNetplay_ = false;
        return;
    }

    ENetAddress bindAddr;
    enet_address_set_host(&bindAddr, "0.0.0.0");
    bindAddr.port = localPort_;
    // peerCapacity=16 for both host and client. This accommodates the
    // full spectator topology: 1 opponent + up to 15 spectators
    // (MAX_SPECTATORS = 15). Even clients use 16 for symmetry — clients
    // never accept inbound connections, but the cost of the larger peer
    // table is negligible (one extra allocation in enet_host_create).
    //
    // History: a previous attempt hardcoded peerCapacity=2 because
    // increasing it caused an ENet binding regression under Wine (the
    // host stopped receiving CONNECT events when peerCapacity > 2).
    // The root cause was never fully diagnosed — suspected Wine
    // SO_REUSEADDR behavior with larger peer tables. We're re-enabling
    // 16 now (commit post-027d9ee) to validate spectator mode at full
    // capacity. If the Wine regression resurfaces, the fallback is to
    // gate peerCapacity on isHost_ (host=16, client=2) since clients
    // never accept spectators.
    const std::size_t peerCapacity = 16;
    host_ = enet_host_create(&bindAddr, peerCapacity, 2 /* channels */, 0, 0);
    if (!host_) {
        common::logger::err("network_thread: enet_host_create failed (port {})", localPort_);
        enet_deinitialize();
        isNetplay_ = false;
        return;
    }
    common::logger::info("network_thread: ENet host bound on port {} (peerCapacity={})",
                         localPort_, peerCapacity);

    if (!isHost_) {
        // Client: initiate the connection to the peer.
        ENetAddress peerAddr;
        if (enet_address_set_host(&peerAddr, peerAddr_.c_str()) < 0) {
            common::logger::err("network_thread: enet_address_set_host('{}') failed", peerAddr_);
            // Continue anyway — host_ exists, the jthread will run but
            // never receive a connect event. The 60s connect timeout in
            // dll_main will eventually fire.
        } else {
            peerAddr.port = peerPort_;
            peer_ = enet_host_connect(host_, &peerAddr, 2, 0);
            if (!peer_) {
                common::logger::err("network_thread: enet_host_connect failed");
            } else {
                common::logger::info("network_thread: connecting to {}:{} ...",
                                     peerAddr_, peerPort_);
            }
        }
    } else {
        common::logger::info("network_thread: waiting for peer to connect...");
    }

    // Host-side NAT re-punch target (connectivity Stage 1). The launcher's
    // relay client punched this NAT flow (localPort → peer) but tore down
    // its socket at deinit; on CGNAT the carrier's NAT entry can expire
    // (seconds) before the opponent's ENet CONNECT arrives. While the
    // opponent hasn't connected, loop() keeps the flow alive with 1-byte
    // 0x00 sends on ENet's own socket (same 5-tuple → same NAT entry).
    // Resolve the target once here (game thread, pre-spawn): the relay
    // path always hands us an IP; a direct join may hand us a hostname.
    punchReady_.store(false, std::memory_order_release);
    if (isHost_ && !peerAddr_.empty()) {
        std::uint32_t ip = 0;
        in_addr raw{};
        raw.s_addr = inet_addr(peerAddr_.c_str());
        if (raw.s_addr != INADDR_NONE) {
            ip = raw.s_addr;
        } else {
            struct addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            struct addrinfo* res = nullptr;
            if (getaddrinfo(peerAddr_.c_str(), nullptr, &hints, &res) == 0 && res && res->ai_addr) {
                ip = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
                freeaddrinfo(res);
            }
        }
        if (ip != 0) {
            punchIp_.store(ip, std::memory_order_release);
            punchReady_.store(true, std::memory_order_release);
        } else {
            common::logger::warn("network_thread: punch target '{}' unresolved — re-punch disabled",
                                 peerAddr_);
        }
    }
    start_ = std::chrono::steady_clock::now();

    // Spawn the worker jthread. The stop_token is passed automatically
    // by std::jthread to the loop's first parameter.
    thread_ = std::jthread([this](std::stop_token st) { loop(std::move(st)); });

    common::logger::info("network_thread: jthread spawned");
}

void NetworkThread::stop() {
    if (!thread_.joinable()) {
        // Nothing to stop — either start() was never called, or it was
        // an offline-mode no-op. Still clear any partial ENet state.
        if (host_) {
            enet_host_destroy(host_);
            host_ = nullptr;
        }
        return;
    }

    common::logger::info("network_thread: stop() — requesting stop");

    // Request the jthread to stop. The loop checks st.stop_requested()
    // at the top of each iteration and exits cleanly.
    thread_.request_stop();

    // Join the jthread. This blocks until loop() returns. The 10ms
    // enet_host_service timeout ensures the join completes within
    // ~10ms of request_stop().
    thread_.join();

    common::logger::info("network_thread: jthread joined");
    if (punchCount_ > 0) {
        common::logger::info("network_thread: host re-punch sent {} packet(s) before connect/stop",
                             punchCount_);
    }

    // Subtask 4.9: clear the network thread ID so any later (spurious)
    // check_network_thread_only() call doesn't match a recycled TID.
    thread_affinity::clear_network_thread();

    // Now that the jthread is stopped, it's safe to disconnect the peer
    // and destroy the host. The jthread is no longer touching them.
    if (peer_) {
        enet_peer_disconnect(peer_, 0);
        enet_host_flush(host_);
        peer_ = nullptr;
    }
    if (host_) {
        enet_host_destroy(host_);
        host_ = nullptr;
    }
    connected_.store(false, std::memory_order_release);

    if (isNetplay_) {
        enet_deinitialize();
    }
    isNetplay_ = false;

    // Clear any pending inbox/outbox items so they don't linger.
    inboxPlayerInputs_.clear();
    inboxTransitionIndex_.clear();
    inboxMenuIndex_.clear();
    inboxRngState_.clear();
    inboxSyncHash_.clear();
    inboxBothInputs_.clear();
    inboxInitialGameState_.clear();
    inboxSpectateConfig_.clear();
    outbox_.clear();
    sim_.clear();

    common::logger::info("network_thread: shut down");
}

std::string NetworkThread::connectDiagnostics() const {
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_).count();
    std::string s = isHost_
        ? "host: no CONNECT received on port " + std::to_string(localPort_)
        : "connect to " + peerAddr_ + ":" + std::to_string(peerPort_) + " not acknowledged";
    s += " (" + std::to_string(elapsedMs) + " ms after netplay start)";
    return s;
}

std::string NetworkThread::endpointDescription() const {
    return isHost_
        ? std::string("listen on port ") + std::to_string(localPort_)
        : peerAddr_ + ":" + std::to_string(peerPort_);
}

ConnectStats NetworkThread::connectStats() const {
    ConnectStats s;
    s.connected     = connected_.load(std::memory_order_acquire);
    s.everConnected = everConnected_.load(std::memory_order_acquire);
    s.elapsedMs    = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_).count();
    s.sentPackets  = sentPacketCount_.load(std::memory_order_relaxed);
    s.rttMs       = rttMs_.load(std::memory_order_relaxed);
    s.endpoint    = endpointDescription();
    return s;
}

// ============================================================================
// Worker loop (runs on the jthread)
// ============================================================================
//
// Per-iteration structure:
//   1. sim_.deliverExpired() — drain the simulator's delay queue into
//      inboxPlayerInputs_ (only meaningful if sim_.enabled()).
//   2. enet_host_service(10ms) — receive any pending ENet events:
//      - CONNECT: store peer_, set connected_
//      - RECEIVE: decode + route to matching inbox (with simulator
//        hooks for PlayerInputs)
//      - DISCONNECT: clear peer_, clear connected_
//   3. Drain outbox_ — call enet_peer_send + enet_host_flush for each
//      pending OutboxEntry.
//
// The 10ms enet_host_service timeout is the responsiveness budget for
// stop requests: thread_.request_stop() in stop() will be honored
// within ~10ms when the next service call returns.

void NetworkThread::loop(std::stop_token st) {
    common::logger::info("network_thread: loop started");

    // Subtask 4.9: register this thread as the NetworkThread for
    // debug-only affinity asserts. See thread_affinity.hpp.
    thread_affinity::set_current_thread_as_network_thread();

    // Local staging deque for simulator delivery. We don't push directly
    // to inboxPlayerInputs_ from deliverExpired() because the simulator
    // returns the messages via a std::deque<PlayerInputs> outparam
    // (avoiding a hard dependency between NetworkSimulator and
    // BlockingQueue).
    std::deque<PlayerInputs> simDelivered;

    // Host-side re-punch state (connectivity Stage 1). Loop-local: this
    // thread is the only one that touches it. Zero-initialized time_point
    // means "never punched" → the first punch goes out immediately.
    auto lastPunch = std::chrono::steady_clock::time_point{};
    bool loggedPunchSendError = false;

    // Stage 0: connect-waiting heartbeat state (loop-local). Active only
    // while !connected_; a 2s cadence keeps the log clean. The DLL connects
    // once (host listens / joiner initiates) so there is no retry to count —
    // N is just the number of 2s wait-intervals observed (≈ seconds waited).
    // lastWaitLog starts at start_ so the first heartbeat lands at start+2s
    // (a fast connect produces none — the good case).
    auto lastWaitLog = start_;
    std::uint32_t connectWaitAttempt = 0;

    while (!st.stop_requested()) {
        // 1. Drain the simulator's delay queue (only meaningful when
        // sim_.enabled() && lag_ms > 0; otherwise this is a fast no-op).
        if (sim_.enabled()) {
            sim_.deliverExpired(simDelivered);
            while (!simDelivered.empty()) {
                inboxPlayerInputs_.push(std::move(simDelivered.front()));
                simDelivered.pop_front();
            }
        }

        // 2. Service ENet. The 10ms timeout is the stop-request
        // responsiveness budget.
        //
        // Subtask 4.9: in debug builds, assert that this is the network
        // thread (ENet is single-threaded by contract — see guiding
        // principle #2 in docs/threading-migration.md).
        thread_affinity::check_network_thread_only("NetworkThread::loop/enet_host_service");
        ENetEvent ev;
        while (enet_host_service(host_, &ev, 10) > 0) {
            if (st.stop_requested()) break;

            switch (ev.type) {
                case ENET_EVENT_TYPE_CONNECT: {
                    // Spectator-aware CONNECT dispatch (commit post-027d9ee).
                    //
                    // Rule:
                    //   - Client (the side that called enet_host_connect):
                    //     the only inbound CONNECT it ever receives is
                    //     the peer acknowledging the connection the client
                    //     itself initiated. So this is always the opponent.
                    //     Set peer_ + connected_ = true.
                    //
                    //   - Host (the side that called enet_host_create and
                    //     is listening):
                    //     - First inbound CONNECT (peer_ == nullptr): this
                    //       is the opponent. Set peer_ + connected_ = true.
                    //     - Subsequent inbound CONNECT (peer_ != nullptr):
                    //       this is a spectator. Delegate to
                    //       SpectatorManager::onSpectatorConnect(peer).
                    //       Do NOT touch peer_ or connected_.
                    //
                    // We never need to read any payload on CONNECT — the
                    // distinction is purely positional (am I the first
                    // connection this host has seen?).
                    //
                    // If spectatorMgr_ is null on the host (e.g. host
                    // never called initSpectatorManager, or this is a
                    // spectator-side client), the second CONNECT is
                    // logged and rejected. The peer will eventually
                    // time out on its side.
                    const bool is_opponent = !isHost_ || peer_ == nullptr;

                    if (is_opponent) {
                        peer_ = ev.peer;
                        connected_.store(true, std::memory_order_release);
                        everConnected_.store(true, std::memory_order_release);
                        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start_).count();
                        common::logger::info(
                            "network_thread: opponent CONNECTED from {}:{} ({} ms after start)",
                            ev.peer->address.host, ev.peer->address.port, elapsedMs);
                    } else {
                        // Spectator connection.
                        if (spectatorMgr_) {
                            common::logger::info(
                                "network_thread: spectator CONNECTED from {}:{} — "
                                "delegating to SpectatorManager",
                                ev.peer->address.host, ev.peer->address.port);
                            spectatorMgr_->onSpectatorConnect(ev.peer);
                        } else {
                            // Host has no SpectatorManager — reject by
                            // force-disconnecting the peer. This shouldn't
                            // happen in practice (initSpectatorManager
                            // is called for every host), but we handle it
                            // defensively to avoid leaking the peer.
                            common::logger::warn(
                                "network_thread: spectator CONNECTED from {}:{} "
                                "but no SpectatorManager — force-disconnecting",
                                ev.peer->address.host, ev.peer->address.port);
                            enet_peer_reset(ev.peer);
                        }
                    }
                    break;
                }

                case ENET_EVENT_TYPE_RECEIVE: {
                    const uint8_t* data = ev.packet->data;
                    const size_t   len  = ev.packet->dataLength;
                    DecodedMessage msg;
                    const bool decoded = DecodedMessage::decode(data, len, msg);
                    enet_packet_destroy(ev.packet);
                    if (!decoded) {
                        common::logger::warn(
                            "network_thread: recv decode failed ({} bytes)", len);
                        break;
                    }

                    // Route to the matching inbox. The 5 inbox queues
                    // match the 5 message types used in v1 netplay.
                    // Other types (BothInputs, NetplayConfig, etc.) are
                    // ignored — they're not used in v1 host/client.
                    switch (msg.type) {
                        case MsgType::PlayerInputs: {
                            PlayerInputs pi = std::move(msg.playerInputs);
                            // Network simulator hooks — only apply to
                            // PlayerInputs (per-frame, UNRELIABLE).
                            // Control messages are always delivered
                            // immediately.
                            if (sim_.enabled()) {
                                if (sim_.shouldDrop()) {
                                    // Simulated packet loss — drop it.
                                    break;
                                }
                                auto deliver_at = sim_.maybeDelay(pi);
                                if (deliver_at) {
                                    sim_.enqueueDelayed(std::move(pi), *deliver_at);
                                    break;
                                }
                            }
                            inboxPlayerInputs_.push(std::move(pi));
                            break;
                        }
                        case MsgType::TransitionIndex:
                            inboxTransitionIndex_.push(msg.transitionIndex.index);
                            break;
                        case MsgType::MenuIndex:
                            inboxMenuIndex_.push(std::move(msg.menuIndex));
                            break;
                        case MsgType::RngState:
                            inboxRngState_.push(std::move(msg.rngState));
                            break;
                        case MsgType::SyncHash:
                            inboxSyncHash_.push(std::move(msg.syncHash));
                            break;

                        // Phase C / Fase 3: spectator-only message types.
                        // These are only populated when the local client
                        // is a spectator; the game thread drains them via
                        // drainNetplayInbox() and forwards to SpectateClient.
                        case MsgType::BothInputs:
                            inboxBothInputs_.push(std::move(msg.bothInputs));
                            break;
                        case MsgType::InitialGameState:
                            inboxInitialGameState_.push(std::move(msg.initialGameState));
                            break;
                        case MsgType::SpectateConfig:
                            inboxSpectateConfig_.push(std::move(msg.spectateConfig));
                            break;

                        default:
                            // NetplayConfig, ConfirmConfig, ChangeConfig,
                            // ClientMode, VersionConfig, PingStats,
                            // InitialConfig, ErrorMessage — not used in
                            // v1 host/client/spectator netplay. Ignore.
                            break;
                    }
                    break;
                }

                case ENET_EVENT_TYPE_DISCONNECT: {
                    // Spectator-aware DISCONNECT dispatch (commit post-027d9ee).
                    //
                    // Rule (mirrors CONNECT):
                    //   - If ev.peer == peer_: this is the opponent
                    //     disconnecting. Clear peer_ + connected_ = false.
                    //     This is the case that triggers the "Opponent
                    //     disconnected" auto-close in the launcher.
                    //   - Otherwise: this is a spectator disconnecting.
                    //     Delegate to SpectatorManager::onSpectatorDisconnect(peer).
                    //     Do NOT touch peer_ or connected_.
                    //   - If spectatorMgr_ is null and ev.peer != peer_,
                    //     the disconnect is from an unknown peer (stale
                    //     socket, race during shutdown, etc.) — log and
                    //     ignore. ENet has already cleaned up the peer
                    //     internally.
                    common::logger::info("network_thread: peer DISCONNECTED from {}:{}",
                                         ev.peer->address.host, ev.peer->address.port);
                    if (ev.peer == peer_) {
                        peer_ = nullptr;
                        connected_.store(false, std::memory_order_release);
                    } else if (spectatorMgr_) {
                        spectatorMgr_->onSpectatorDisconnect(ev.peer);
                    } else {
                        // Unknown peer disconnect — already cleaned up
                        // by ENet. Nothing to do.
                    }
                    break;
                }

                default:
                    break;
            }
        }

        // 2b. Host-side NAT re-punch (connectivity Stage 1).
        //
        // While the opponent hasn't connected, keep the outbound
        // (localPort → peer) flow alive so the carrier's NAT entry stays
        // open for the opponent's ENet CONNECT. We send a 1-byte 0x00 on
        // ENet's OWN socket: same 5-tuple, so it hits the same NAT entry
        // the launcher's punch created, and sendto() is send-only — ENet
        // only reads from this socket, so its state is untouched. The
        // opponent's ENet silently drops the short packet (it's smaller
        // than the ENet protocol header). Cadence: 50 ms for the first
        // 3 s (catch a fresh boot fast), then 2 s.
        if (isHost_ && punchReady_.load(std::memory_order_acquire) &&
            !connected_.load(std::memory_order_acquire)) {
            using namespace std::chrono;
            const auto now = steady_clock::now();
            const auto elapsedMs = duration_cast<milliseconds>(now - start_).count();
            const auto interval = elapsedMs < 3000
                ? milliseconds(50)
                : milliseconds(2000);
            if (now - lastPunch >= interval) {
                lastPunch = now;
                sockaddr_in dst{};
                dst.sin_family = AF_INET;
                dst.sin_port = htons(peerPort_);
                dst.sin_addr.s_addr = punchIp_.load(std::memory_order_relaxed);
                const char nul = 0;
                const int rc = sendto(host_->socket, &nul, 1, 0,
                                      reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
                if (rc < 0) {
                    if (!loggedPunchSendError) {
                        common::logger::warn("network_thread: re-punch sendto failed (WSA={}) — "
                                             "disabling re-punch", WSAGetLastError());
                        loggedPunchSendError = true;
                    }
                } else {
                    ++punchCount_;
                }
            }
        }

        // 2c. Stage 0 connect-waiting heartbeat. While the opponent hasn't
        // connected, emit a low-cadence (2s) line so a stuck connect shows up
        // as "attempt 1, 2, 3…" in the log instead of silence. Silent once
        // connected (and on a fast connect, entirely) — that's the good case.
        if (!connected_.load(std::memory_order_acquire)) {
            using namespace std::chrono;
            const auto now = steady_clock::now();
            if (now - lastWaitLog >= milliseconds(2000)) {
                lastWaitLog = now;
                ++connectWaitAttempt;
                const auto elapsedMs = duration_cast<milliseconds>(now - start_).count();
                common::logger::info(
                    "network_thread: still waiting for connect (attempt {}, {}ms elapsed, {})",
                    connectWaitAttempt, elapsedMs, endpointDescription());
            }
        }

        // 3. Drain outbox — send pending outgoing packets.
        // Only meaningful when peer_ is connected; otherwise we'd just
        // create packets and immediately destroy them. But we still
        // drain the queue to avoid memory growth if the game thread
        // keeps enqueueing during disconnect.
        if (peer_ && connected_.load(std::memory_order_acquire)) {
            // Stage 0: refresh the RTT snapshot (network thread owns peer_).
            rttMs_.store(peer_->roundTripTime, std::memory_order_relaxed);
            OutboxEntry entry;
            while (outbox_.try_pop(entry)) {
                const uint32_t flags = entry.reliable
                    ? ENET_PACKET_FLAG_RELIABLE
                    : ENET_PACKET_FLAG_UNSEQUENCED;
                ENetPacket* packet = enet_packet_create(
                    entry.bytes.data(), entry.bytes.size(), flags);
                if (packet) {
                    // enet_peer_send: 0 = queued, -1 = failed. Count successes
                    // for the Stage 0 connectStats() snapshot.
                    if (enet_peer_send(peer_, 0, packet) == 0) {
                        ++sentPacketCount_;
                    }
                }
            }
            enet_host_flush(host_);
        } else {
            // Disconnected — drain and drop to prevent memory growth.
            OutboxEntry entry;
            while (outbox_.try_pop(entry)) {
                // Dropped. (Logging would be too verbose at 60Hz.)
            }
        }

        // 4. Phase C / Fase 2.5: Spectator manager step + drain outbox.
        //
        // step() checks for pending-spectator timeouts (20s) and
        // disconnects expired ones.
        //
        // tryPopOut() drains the SpectatorManager's outbox — packets
        // that frameStepSpectators() / promotePending() queued for
        // individual spectators. Each packet targets a specific ENetPeer*
        // (stored as void* in OutboxEntry.peer).
        if (spectatorMgr_) {
            spectatorMgr_->step();

            caster::dll::spec::SpectatorManager::OutPacket outPkt;
            while (spectatorMgr_->tryPopOut(outPkt)) {
                if (!outPkt.peer) continue;
                const uint32_t flags = outPkt.reliable
                    ? ENET_PACKET_FLAG_RELIABLE
                    : ENET_PACKET_FLAG_UNSEQUENCED;
                ENetPacket* packet = enet_packet_create(
                    outPkt.bytes.data(), outPkt.bytes.size(), flags);
                if (packet) {
                    enet_peer_send(reinterpret_cast<ENetPeer*>(outPkt.peer), 0, packet);
                }
            }
            if (spectatorMgr_->numSpectators() > 0 || spectatorMgr_->numPending() > 0) {
                enet_host_flush(host_);
            }
        }
    }

    common::logger::info("network_thread: loop exiting (stop requested)");
}

} // namespace caster::dll::netplay
