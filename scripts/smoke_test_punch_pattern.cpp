// Pattern-level validation for the host-side NAT re-punch (connectivity
// Stage 1). It does NOT link the real NetworkThread (that drags in the whole
// rollback engine); it reproduces the exact socket mechanics the punch relies
// on, to de-risk the platform assumptions under Wine + mingw:
//
//   1. ENet's bound host exposes a usable Winsock SOCKET at host->socket.
//   2. A raw sendto() of a 1-byte 0x00 on that socket is delivered to the
//      peer, carrying the BOUND game port as its source port (the NAT key).
//   3. A live peer ENet host receiving the 1-byte non-ENet packet drops it
//      silently (no error, no surfaced RECEIVE) — the safety property that
//      lets the punch share the port with live ENet traffic.
//
// Note: two UDP sockets cannot share a port on Windows without SO_REUSEADDR
// (ENet does not set it), which is precisely why the production punch sends on
// ENet's own socket rather than a second one. The test therefore uses a
// distinct receiver port per sub-check.
//
// Run: wine build/bin/smoke_test_punch_pattern.exe   (exit 0 = OK)

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <enet/enet.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr std::uint16_t kSenderPort  = 46318;  // "game" port (bound)
constexpr std::uint16_t kPlainPort   = 47318;  // receiver 1 (plain UDP)
constexpr std::uint16_t kEnetPeerPort = 47319; // receiver 2 (peer ENet)

bool g_ws2init = false;

void cleanup() {
    if (g_ws2init) {
        WSACleanup();
        g_ws2init = false;
    }
    enet_deinitialize();
}

// Bind a fresh UDP socket to 127.0.0.1:port, non-blocking. Returns INVALID
// socket on failure.
SOCKET bind_plain(std::uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    return s;
}

sockaddr_in loopback(std::uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}

} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::atexit(cleanup);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "FAIL: WSAStartup\n");
        return 1;
    }
    g_ws2init = true;

    if (enet_initialize() != 0) {
        std::fprintf(stderr, "FAIL: enet_initialize\n");
        return 1;
    }

    // --- Sender: an ENet host bound to the game port (mirrors NetworkThread).
    ENetAddress bindAddr;
    bindAddr.host = ENET_HOST_ANY;
    bindAddr.port = kSenderPort;
    ENetHost* sender = enet_host_create(&bindAddr, 1, 2, 0, 0);
    if (!sender || sender->socket == ENET_SOCKET_NULL) {
        std::fprintf(stderr, "FAIL: enet_host_create(sender, %u) / socket null\n",
                     (unsigned)kSenderPort);
        return 1;
    }

    int rc = 0;

    // --- Check 1: raw sendto on sender->socket delivers a 1-byte 0x00 to a
    // plain UDP socket, with the bound game port as the source port. ---
    {
        SOCKET plain = bind_plain(kPlainPort);
        if (plain == INVALID_SOCKET) {
            std::fprintf(stderr, "FAIL: bind_plain(%u) — WSA=%d\n",
                         (unsigned)kPlainPort, WSAGetLastError());
            rc = 1;
        } else {
            const char nul = 0;
            sockaddr_in dstPlain = loopback(kPlainPort);
            int n = sendto(sender->socket, &nul, 1, 0,
                           (sockaddr*)&dstPlain, sizeof(dstPlain));
            bool ok = false;
            std::uint16_t srcPort = 0;
            if (n == 1) {
                char buf[64];
                sockaddr_in from{};
                int flen = sizeof(from);
                for (int i = 0; i < 50; ++i) {
                    int r = recvfrom(plain, buf, sizeof(buf), 0,
                                     (sockaddr*)&from, &flen);
                    if (r == 1 && buf[0] == 0) {
                        ok = true;
                        srcPort = ntohs(from.sin_port);
                        break;
                    }
                    if (r < 0) {
                        int e = WSAGetLastError();
                        if (e == WSAEWOULDBLOCK) { Sleep(10); continue; }
                        break;
                    }
                    Sleep(10);
                }
            }
            if (!ok) {
                std::fprintf(stderr, "FAIL: punch byte not delivered (sendto n=%d)\n", n);
                rc = 1;
            } else if (srcPort != kSenderPort) {
                std::fprintf(stderr, "FAIL: punch source port %u != bound game port %u\n",
                             (unsigned)srcPort, (unsigned)kSenderPort);
                rc = 1;
            } else {
                std::printf("OK: 1-byte 0x00 delivered, source port == game port (%u)\n",
                            (unsigned)kSenderPort);
            }
            closesocket(plain);
        }
    }

    // --- Check 2: a live peer ENet host receiving the 1-byte non-ENet packet
    // drops it silently (no error, no surfaced RECEIVE). ---
    {
        ENetAddress peerBind;
        peerBind.host = ENET_HOST_ANY;
        peerBind.port = kEnetPeerPort;
        ENetHost* peer = enet_host_create(&peerBind, 1, 2, 0, 0);
        if (!peer) {
            std::fprintf(stderr, "FAIL: enet_host_create(peer, %u)\n",
                         (unsigned)kEnetPeerPort);
            rc = 1;
        } else {
            const char nul = 0;
            sockaddr_in dstPeer = loopback(kEnetPeerPort);
            sendto(sender->socket, &nul, 1, 0,
                   (sockaddr*)&dstPeer, sizeof(dstPeer));
            bool surfaced = false;
            for (int i = 0; i < 30; ++i) {
                ENetEvent ev;
                int s = enet_host_service(peer, &ev, 20);
                if (s < 0) {
                    std::fprintf(stderr, "FAIL: peer enet_host_service error (rc=%d)\n", s);
                    rc = 1;
                    break;
                }
                if (s == 0) break;
                if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(ev.packet);
                    surfaced = true;
                    break;
                }
            }
            if (rc == 0 && surfaced) {
                std::fprintf(stderr, "FAIL: peer ENet host surfaced the 1-byte packet (not dropped)\n");
                rc = 1;
            } else if (rc == 0) {
                std::printf("OK: peer ENet host dropped the 1-byte packet silently\n");
            }
            enet_host_destroy(peer);
        }
    }

    enet_host_destroy(sender);

    if (rc == 0) std::printf("OK: punch pattern validated\n");
    return rc;
}
