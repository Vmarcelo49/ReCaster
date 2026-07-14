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
    // peerCapacity=2 for both host and client. Increasing to 16 caused
    // ENet binding issues under Wine — the DLL's ENet host stopped
    // receiving CONNECT events when peerCapacity > 2. Root cause likely
    // related to Wine's SO_REUSEADDR behavior with larger peer tables.
    // Will re-enable 16 peers when spectator mode is properly validated.
    const std::size_t peerCapacity = 2;
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
                case ENET_EVENT_TYPE_CONNECT:
                    // Simple accept: any connection becomes the primary peer.
                    // Spectator detection is DISABLED — see docs/spectator-plan.md
                    // for the status of Phase C and what needs to be resolved
                    // before re-enabling.
                    peer_ = ev.peer;
                    connected_.store(true, std::memory_order_release);
                    common::logger::info(
                        "network_thread: peer CONNECTED from {}:{}",
                        ev.peer->address.host, ev.peer->address.port);
                    break;

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

                case ENET_EVENT_TYPE_DISCONNECT:
                    // Only clear state if the disconnecting peer is the one
                    // we're tracking. The host may receive DISCONNECT events
                    // from stale launcher sockets — those must NOT clear the
                    // real opponent's peer_ pointer.
                    common::logger::info("network_thread: peer DISCONNECTED from {}:{}",
                                         ev.peer->address.host, ev.peer->address.port);
                    if (ev.peer == peer_) {
                        peer_ = nullptr;
                        connected_.store(false, std::memory_order_release);
                    }
                    break;

                default:
                    break;
            }
        }

        // 3. Drain outbox — send pending outgoing packets.
        // Only meaningful when peer_ is connected; otherwise we'd just
        // create packets and immediately destroy them. But we still
        // drain the queue to avoid memory growth if the game thread
        // keeps enqueueing during disconnect.
        if (peer_ && connected_.load(std::memory_order_acquire)) {
            OutboxEntry entry;
            while (outbox_.try_pop(entry)) {
                const uint32_t flags = entry.reliable
                    ? ENET_PACKET_FLAG_RELIABLE
                    : ENET_PACKET_FLAG_UNSEQUENCED;
                ENetPacket* packet = enet_packet_create(
                    entry.bytes.data(), entry.bytes.size(), flags);
                if (packet) {
                    enet_peer_send(peer_, 0, packet);
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
