// src/dll/netplay/connector.cpp
//
// Synchronous ENet transport for netplay. See netplay_connector.hpp.
//
// Design: all ENet I/O happens in poll(), called once per frame from
// frameStep() on the game's main thread. No worker thread, no locks.
//
// Message routing:
//   - Outbox: sendXxx() serializes the message into an ENet packet and
//     pushes it to the peer immediately (enet_peer_send + enet_host_flush).
//   - Inbox: poll() decodes incoming ENET_EVENT_TYPE_RECEIVE events and
//     pushes the deserialized message into the matching std::queue.
//     recvXxx() pops one message at a time.
//
// This queue-based design keeps the transport layer dumb: the
// NetplayManager doesn't know about ENet, and the connector doesn't
// know about FSM state. frameStep() is the orchestrator that drains
// the inboxes into the NetplayManager and pushes NetplayManager output
// into the outbox.

#include "connector.hpp"
#include "protocol/decoder.hpp"
#include "../common/logger.hpp"

#include <enet/enet.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <queue>
#include <string>

namespace caster::dll::netplay {

namespace {

// ---- Connection state ----
bool     g_started   = false;
bool     g_isNetplay = false;
bool     g_isHost    = false;
uint16_t g_localPort = 0;
std::string g_peerAddr;
uint16_t g_peerPort  = 0;

ENetHost* g_host = nullptr;
ENetPeer* g_peer = nullptr;
std::atomic<bool> g_connected{false};

// ---- Network simulator (for testing rollback under laggy conditions) ----
//
// When CASTER_SIM_LAG_MS, CASTER_SIM_JITTER_MS, or CASTER_SIM_LOSS_PCT
// are set in the environment, incoming PlayerInputs messages are held in
// a delay queue and only delivered to the inbox after the simulated
// network delay has elapsed. This forces the rollback engine to predict
// inputs and then correct via rollback when the real (delayed) input
// arrives — exactly what happens with real network latency.
//
// Only PlayerInputs is delayed (per-frame, UNRELIABLE). Control messages
// (TransitionIndex, MenuIndex, RngState, SyncHash) are delivered
// immediately — they use RELIABLE channels and delaying them would
// break the handshake/state-sync logic, not test rollback.
struct SimConfig {
    bool   enabled    = false;
    int    lag_ms     = 0;    // base delay added to each PlayerInputs
    int    jitter_ms  = 0;    // random ±jitter added to each delay
    int    loss_pct   = 0;    // 0-100, percent of PlayerInputs to drop
};
SimConfig g_sim;

struct DelayedMessage {
    PlayerInputs msg;
    std::chrono::steady_clock::time_point deliver_at;
};
std::deque<DelayedMessage> g_delayQueue;

// ---- Inboxes (peer → frameStep) ----
//
// Bounded by the rate at which frameStep drains them. In normal play
// these stay small (a few messages per frame), but during a network
// burst they could grow. We don't cap them — if memory becomes an
// issue we can add a high-watermark drop-oldest policy.
std::queue<PlayerInputs> g_inboxPlayerInputs;
std::queue<uint32_t>     g_inboxTransitionIndex;
std::queue<MenuIndex>    g_inboxMenuIndex;
std::queue<RngState>     g_inboxRngState;
std::queue<SyncHash>     g_inboxSyncHash;

// ---- Helpers ----

// Send raw bytes as an ENet packet on channel 0. RELIABLE for control
// messages (TransitionIndex, MenuIndex, RngState) and UNRELIABLE for
// per-frame PlayerInputs (we'd rather drop a stale input than delay
// the newer one — the NetplayManager's InputsContainer fills gaps via
// lastInputBefore prediction).
void sendPacket(const std::vector<uint8_t>& bytes, bool reliable) {
    if (!g_isNetplay || !g_connected.load() || !g_peer) return;

    const uint32_t flags = reliable ? ENET_PACKET_FLAG_RELIABLE
                                    : ENET_PACKET_FLAG_UNSEQUENCED;
    ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), flags);
    if (!packet) return;

    enet_peer_send(g_peer, 0, packet);
    enet_host_flush(g_host);
}

} // namespace

// ============================================================================
// Lifecycle
// ============================================================================

void start(const caster::common::ipc::config_buffer::Config& cfg) {
    if (g_started) return;
    g_started = true;

    // ---- Network simulator config (read from env vars) ----
    //
    // CASTER_SIM_LAG_MS=N     — add N ms delay to each PlayerInputs
    // CASTER_SIM_JITTER_MS=N  — add random ±N ms to the delay
    // CASTER_SIM_LOSS_PCT=N   — drop N% of PlayerInputs (0-100)
    //
    // All three are optional. If none are set, the simulator is disabled
    // and the connector behaves normally (zero overhead).
    {
        const char* lag    = std::getenv("CASTER_SIM_LAG_MS");
        const char* jitter = std::getenv("CASTER_SIM_JITTER_MS");
        const char* loss   = std::getenv("CASTER_SIM_LOSS_PCT");
        if (lag)    g_sim.lag_ms    = std::atoi(lag);
        if (jitter) g_sim.jitter_ms = std::atoi(jitter);
        if (loss)   g_sim.loss_pct  = std::atoi(loss);
        g_sim.enabled = (g_sim.lag_ms > 0 || g_sim.jitter_ms > 0 || g_sim.loss_pct > 0);
        if (g_sim.enabled) {
            common::logger::info("netplay: SIMULATOR enabled — lag={}ms jitter={}ms loss={}%%",
                                 g_sim.lag_ms, g_sim.jitter_ms, g_sim.loss_pct);
        }
    }

    if (!cfg.is_netplay()) {
        g_isNetplay = false;
        return;  // offline training/versus — nothing to do
    }
    g_isNetplay = true;
    g_isHost    = cfg.is_host();
    g_localPort = cfg.local_udp_port;
    g_peerAddr  = cfg.peer_addr;
    g_peerPort  = cfg.peer_port;

    common::logger::info(
        "netplay: starting (host={} bind_port={} peer={}:{})",
        g_isHost, g_localPort, g_peerAddr, g_peerPort);

    if (enet_initialize() != 0) {
        common::logger::err("netplay: enet_initialize failed");
        g_isNetplay = false;
        return;
    }

    ENetAddress bindAddr;
    enet_address_set_host(&bindAddr, "0.0.0.0");
    bindAddr.port = g_localPort;
    g_host = enet_host_create(&bindAddr, 2 /* peers */, 2 /* channels */, 0, 0);
    if (!g_host) {
        common::logger::err("netplay: enet_host_create failed (port {})", g_localPort);
        g_isNetplay = false;
        return;
    }
    common::logger::info("netplay: ENet host bound on port {}", g_localPort);

    if (!g_isHost) {
        // Client: initiate the connection to the peer.
        ENetAddress peerAddr;
        if (enet_address_set_host(&peerAddr, g_peerAddr.c_str()) < 0) {
            common::logger::err("netplay: enet_address_set_host('{}') failed", g_peerAddr);
            return;
        }
        peerAddr.port = g_peerPort;
        g_peer = enet_host_connect(g_host, &peerAddr, 2, 0);
        if (!g_peer) {
            common::logger::err("netplay: enet_host_connect failed");
            return;
        }
        common::logger::info("netplay: connecting to {}:{} ...", g_peerAddr, g_peerPort);
    } else {
        common::logger::info("netplay: waiting for peer to connect...");
    }
}

// Deliver any delayed PlayerInputs whose timestamp has expired.
// Called at the top of poll() so the delay queue is drained every frame.
void deliverExpiredDelayed() {
    if (g_delayQueue.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    while (!g_delayQueue.empty() && g_delayQueue.front().deliver_at <= now) {
        g_inboxPlayerInputs.push(g_delayQueue.front().msg);
        g_delayQueue.pop_front();
    }
}

void poll() {
    if (!g_isNetplay || !g_host) return;

    // Drain the delay queue first — deliver any messages whose simulated
    // network delay has elapsed.
    deliverExpiredDelayed();

    ENetEvent ev;
    while (enet_host_service(g_host, &ev, 0) > 0) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                g_peer = ev.peer;
                g_connected.store(true);
                common::logger::info("netplay: peer CONNECTED from {}:{}",
                                     ev.peer->address.host, ev.peer->address.port);
                break;

            case ENET_EVENT_TYPE_RECEIVE: {
                const uint8_t* data = ev.packet->data;
                const size_t   len  = ev.packet->dataLength;
                DecodedMessage msg;
                if (DecodedMessage::decode(data, len, msg)) {
                    // Route to the matching inbox.
                    switch (msg.type) {
                        case MsgType::PlayerInputs: {
                            // Network simulator: apply packet loss and
                            // delay to PlayerInputs (per-frame, UNRELIABLE).
                            // Control messages are always delivered
                            // immediately — only per-frame inputs are
                            // delayed to test the rollback engine.
                            if (g_sim.enabled) {
                                // Packet loss: drop this packet.
                                if (g_sim.loss_pct > 0 &&
                                    (std::rand() % 100) < g_sim.loss_pct) {
                                    // Simulated packet loss — drop it.
                                    break;
                                }
                                // Lag + jitter: queue for later delivery.
                                if (g_sim.lag_ms > 0 || g_sim.jitter_ms > 0) {
                                    int delay_ms = g_sim.lag_ms;
                                    if (g_sim.jitter_ms > 0) {
                                        delay_ms += (std::rand() % (2 * g_sim.jitter_ms + 1)) - g_sim.jitter_ms;
                                        if (delay_ms < 0) delay_ms = 0;
                                    }
                                    g_delayQueue.push_back({
                                        msg.playerInputs,
                                        std::chrono::steady_clock::now() +
                                            std::chrono::milliseconds(delay_ms)
                                    });
                                    break;
                                }
                            }
                            g_inboxPlayerInputs.push(msg.playerInputs);
                            break;
                        }
                        case MsgType::TransitionIndex:
                            g_inboxTransitionIndex.push(msg.transitionIndex.index);
                            break;
                        case MsgType::MenuIndex:
                            g_inboxMenuIndex.push(msg.menuIndex);
                            break;
                        case MsgType::RngState:
                            g_inboxRngState.push(msg.rngState);
                            break;
                        case MsgType::SyncHash:
                            g_inboxSyncHash.push(msg.syncHash);
                            break;
                        default:
                            // Other message types (BothInputs, SyncHash,
                            // NetplayConfig, etc.) are not used in v1
                            // host/client netplay — ignore.
                            break;
                    }
                }
                enet_packet_destroy(ev.packet);
                break;
            }

            case ENET_EVENT_TYPE_DISCONNECT:
                common::logger::info("netplay: peer DISCONNECTED");
                g_peer = nullptr;
                g_connected.store(false);
                break;

            default:
                break;
        }
    }
}

void shutdown() {
    if (g_peer) {
        enet_peer_disconnect(g_peer, 0);
        enet_host_flush(g_host);
    }
    if (g_host) {
        enet_host_destroy(g_host);
        g_host = nullptr;
    }
    g_peer = nullptr;
    g_connected.store(false);
    if (g_isNetplay) {
        enet_deinitialize();
    }
    g_isNetplay = false;
    g_started = false;

    // Clear inboxes
    while (!g_inboxPlayerInputs.empty()) g_inboxPlayerInputs.pop();
    while (!g_inboxTransitionIndex.empty()) g_inboxTransitionIndex.pop();
    while (!g_inboxMenuIndex.empty()) g_inboxMenuIndex.pop();
    while (!g_inboxRngState.empty()) g_inboxRngState.pop();
    while (!g_inboxSyncHash.empty()) g_inboxSyncHash.pop();

    common::logger::info("netplay: shut down");
}

bool connected() { return g_connected.load(); }
bool isHost()    { return g_isHost; }

// ============================================================================
// Outbox (frameStep → peer)
// ============================================================================

void sendPlayerInputs(const PlayerInputs& pi) {
    // PlayerInputs is the per-frame high-frequency message. We send it
    // UNRELIABLE+UNSEQUENCED so that if a packet is lost or delayed, the
    // next one (with a more recent indexedFrame) supersedes it. The
    // receiver's InputsContainer.assign() overwrites older frames.
    //
    // The NetplayManager always sends the last NUM_INPUTS frames, so a
    // single dropped packet is recovered by the next one (which still
    // contains the dropped frames in its window).
    sendPacket(pi.serialize(), /*reliable=*/false);
}

void sendTransitionIndex(uint32_t index) {
    TransitionIndex ti(index);
    // Control message — must arrive. RELIABLE.
    sendPacket(ti.serialize(), /*reliable=*/true);
}

void sendMenuIndex(const MenuIndex& mi) {
    // Retry menu selection sync — must arrive exactly once. RELIABLE.
    sendPacket(mi.serialize(), /*reliable=*/true);
}

void sendRngState(const RngState& rs) {
    // RNG sync — must arrive (desync prevention). RELIABLE.
    sendPacket(rs.serialize(), /*reliable=*/true);
}

void sendSyncHash(const SyncHash& sh) {
    // Desync detection — must arrive (we'd miss a desync if dropped).
    // RELIABLE.
    sendPacket(sh.serialize(), /*reliable=*/true);
}

// ============================================================================
// Inbox (peer → frameStep)
// ============================================================================

std::optional<PlayerInputs> recvPlayerInputs() {
    if (g_inboxPlayerInputs.empty()) return std::nullopt;
    PlayerInputs pi = std::move(g_inboxPlayerInputs.front());
    g_inboxPlayerInputs.pop();
    return pi;
}

std::optional<uint32_t> recvTransitionIndex() {
    if (g_inboxTransitionIndex.empty()) return std::nullopt;
    uint32_t idx = g_inboxTransitionIndex.front();
    g_inboxTransitionIndex.pop();
    return idx;
}

std::optional<MenuIndex> recvMenuIndex() {
    if (g_inboxMenuIndex.empty()) return std::nullopt;
    MenuIndex mi = std::move(g_inboxMenuIndex.front());
    g_inboxMenuIndex.pop();
    return mi;
}

std::optional<RngState> recvRngState() {
    if (g_inboxRngState.empty()) return std::nullopt;
    RngState rs = std::move(g_inboxRngState.front());
    g_inboxRngState.pop();
    return rs;
}

std::optional<SyncHash> recvSyncHash() {
    if (g_inboxSyncHash.empty()) return std::nullopt;
    SyncHash sh = std::move(g_inboxSyncHash.front());
    g_inboxSyncHash.pop();
    return sh;
}

} // namespace caster::dll::netplay
