// src/dll/netplay_connector.cpp
//
// Synchronous ENet transport for netplay. See netplay_connector.hpp.
//
// Design: all ENet I/O happens in poll(), called once per frame from
// frameStep() on the game's main thread. No worker thread, no locks.

#include "netplay_connector.hpp"
#include "messages.hpp"
#include "protocol.hpp"
#include "inputs_container.hpp"
#include "input_reader.hpp"  // combine_input
#include "dll_process_manager.hpp"
#include "../common/logger.hpp"

#include <enet/enet.h>

#include <atomic>
#include <cstring>
#include <string>

namespace caster::dll::netplay {

namespace {

// ---- State ----
bool     g_started        = false;   // start() was called (idempotent guard)
bool     g_isNetplay      = false;   // cfg.is_netplay() at start time
bool     g_isHost         = false;   // cfg.is_host() at start time
uint16_t g_localPort      = 0;       // cfg.local_udp_port
std::string g_peerAddr;              // cfg.peer_addr
uint16_t g_peerPort       = 0;       // cfg.peer_port

ENetHost* g_host          = nullptr;
ENetPeer* g_peer          = nullptr;
std::atomic<bool> g_connected{false};

// Local/remote player numbers derived from host_player.
// The IPC Config only carries host_player (=1). Host side is player 1,
// client side is player 2 — matching the launcher's session.cpp.
uint8_t  g_localPlayerSlot  = 0;  // 0 or 1 (array index in BothInputs.inputs[])
uint8_t  g_remotePlayerSlot = 0;  // 0 or 1

// Storage for received remote inputs. The peer's BothInputs carries both
// players' last NUM_INPUTS frames; we extract the remote slot.
InputsContainer<uint16_t> g_remoteInputs;

// Last applied remote input (combined uint16_t), used as a fallback when a
// frame's input hasn't arrived yet (no rollback/delay handling in this slice).
uint16_t g_lastRemoteInput = 0;
bool     g_haveRemoteInput = false;

// ---- Helpers ----

// Pack a (direction, buttons) pair into the combined wire format.
// direction: numpad 0-9 (0=neutral). buttons: CC_BUTTON_* bitmask.
inline uint16_t pack_input(uint16_t direction, uint16_t buttons) {
    return static_cast<uint16_t>((direction & 0x0F) | ((buttons & 0x0FFF) << 4));
}

// Unpack the combined wire format back to the game's split fields.
// Returns {direction, buttons}.
struct UnpackedInput { uint16_t direction; uint16_t buttons; };
inline UnpackedInput unpack_input(uint16_t combined) {
    return { static_cast<uint16_t>(combined & 0x000F),
             static_cast<uint16_t>((combined & 0xFFF0) >> 4) };
}

} // namespace

void start(const caster::common::ipc::config_buffer::Config& cfg) {
    if (g_started) return;
    g_started = true;

    if (!cfg.is_netplay()) {
        g_isNetplay = false;
        return;  // offline training/versus — nothing to do
    }
    g_isNetplay = true;
    g_isHost    = cfg.is_host();
    g_localPort = cfg.local_udp_port;
    g_peerAddr  = cfg.peer_addr;
    g_peerPort  = cfg.peer_port;

    // Host = player 1 (slot 0), client = player 2 (slot 1).
    g_localPlayerSlot  = g_isHost ? 0 : 1;
    g_remotePlayerSlot = g_isHost ? 1 : 0;

    caster::common::logger::info(
        "netplay: starting (host={} local=P{} remote=P{} bind_port={} peer={}:{})",
        g_isHost, g_localPlayerSlot + 1, g_remotePlayerSlot + 1,
        g_localPort, g_peerAddr, g_peerPort);

    if (enet_initialize() != 0) {
        caster::common::logger::err("netplay: enet_initialize failed");
        g_isNetplay = false;
        return;
    }

    // Bind our ENet host. If local_udp_port is 0 the OS picks a port; the
    // launcher normally provides the hole-punched port.
    ENetAddress bindAddr;
    enet_address_set_host(&bindAddr, "0.0.0.0");
    bindAddr.port = g_localPort;
    g_host = enet_host_create(&bindAddr, 2 /* peers */, 2 /* channels */, 0, 0);
    if (!g_host) {
        caster::common::logger::err("netplay: enet_host_create failed (port {})", g_localPort);
        g_isNetplay = false;
        return;
    }
    caster::common::logger::info("netplay: ENet host bound on port {}", g_localPort);

    if (!g_isHost) {
        // Client: initiate the connection to the peer.
        ENetAddress peerAddr;
        if (enet_address_set_host(&peerAddr, g_peerAddr.c_str()) < 0) {
            caster::common::logger::err("netplay: enet_address_set_host('{}') failed", g_peerAddr);
            return;
        }
        peerAddr.port = g_peerPort;
        g_peer = enet_host_connect(g_host, &peerAddr, 2, 0);
        if (!g_peer) {
            caster::common::logger::err("netplay: enet_host_connect failed");
            return;
        }
        caster::common::logger::info("netplay: connecting to {}:{} ...", g_peerAddr, g_peerPort);
        // The CONNECT event arrives on a subsequent poll(); we don't block here.
    } else {
        caster::common::logger::info("netplay: waiting for peer to connect...");
    }
}

void poll() {
    if (!g_isNetplay || !g_host) return;

    // Drain all pending events this frame (non-blocking).
    ENetEvent ev;
    while (enet_host_service(g_host, &ev, 0) > 0) {
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT:
                g_peer = ev.peer;
                g_connected.store(true);
                caster::common::logger::info(
                    "netplay: peer CONNECTED from {}:{}",
                    ev.peer->address.host, ev.peer->address.port);
                break;

            case ENET_EVENT_TYPE_RECEIVE: {
                const uint8_t* data = ev.packet->data;
                const size_t   len  = ev.packet->dataLength;
                DecodedMessage msg;
                if (DecodedMessage::decode(data, len, msg)) {
                    if (msg.type == MsgType::BothInputs) {
                        // The peer sent both players' input history. Extract
                        // the remote player's slot and store each frame.
                        const auto& bi = msg.bothInputs;
                        const uint32_t startFrame = bi.getStartFrame();
                        for (uint32_t i = 0; i < NUM_INPUTS; ++i) {
                            const uint16_t v = bi.inputs[g_remotePlayerSlot][i];
                            const uint32_t f = startFrame + i;
                            g_remoteInputs.set(0, f, v);
                            if (f == bi.getFrame()) {
                                g_lastRemoteInput = v;
                                g_haveRemoteInput = true;
                            }
                        }
                    }
                }
                enet_packet_destroy(ev.packet);
                break;
            }

            case ENET_EVENT_TYPE_DISCONNECT:
                caster::common::logger::info("netplay: peer DISCONNECTED");
                g_peer = nullptr;
                g_connected.store(false);
                break;

            default:
                break;
        }
    }
}

void send_local_input(uint16_t direction, uint16_t buttons, uint32_t frame) {
    if (!g_isNetplay || !g_connected.load() || !g_peer) return;

    BothInputs bi;
    bi.indexedFrame.parts.frame = frame;
    bi.indexedFrame.parts.index = 0;  // no rollback indexing in this slice

    // Fill the local slot's history: we only know the current frame's input,
    // so we place it at the end of the window (index NUM_INPUTS-1) and leave
    // the rest zero. The peer treats the latest non-zero entry as current.
    const uint16_t packed = pack_input(direction, buttons);
    bi.inputs[g_localPlayerSlot][NUM_INPUTS - 1] = packed;
    // Mirror the same input into the earlier slots so getStartFrame()/getFrame()
    // math yields the expected window for a peer that reads the whole array.
    for (int i = 0; i < NUM_INPUTS - 1; ++i)
        bi.inputs[g_localPlayerSlot][i] = packed;

    const auto bytes = bi.serialize();
    ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(),
                                            ENET_PACKET_FLAG_UNSEQUENCED);
    if (packet) {
        enet_peer_send(g_peer, 0, packet);
        enet_host_flush(g_host);
    }
}

bool apply_remote_input(uint8_t remote_player, uint32_t frame) {
    if (!g_isNetplay) return false;

    uint16_t combined = 0;
    if (g_haveRemoteInput) {
        // Prefer the exact frame if we have it; otherwise reuse the last
        // known remote input (acceptable without delay/rollback handling).
        const uint16_t exact = g_remoteInputs.get(0, frame);
        combined = (exact != 0) ? exact : g_lastRemoteInput;
    }
    const auto u = unpack_input(combined);
    caster::dll::process_manager::writeGameInput(remote_player, u.direction, u.buttons);
    return true;
}

bool connected() {
    return g_connected.load();
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
    caster::common::logger::info("netplay: shut down");
}

} // namespace caster::dll::netplay
