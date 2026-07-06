// src/dll/net_listener.cpp

#include "net_listener.hpp"

#include <enet/enet.h>

#include <chrono>
#include <mutex>
#include <thread>

#ifndef CASTER_NET_PORT
#  define CASTER_NET_PORT 7500
#endif

namespace caster::dll {

namespace {

std::thread      g_thread;
std::mutex       g_mtx;
ENetHost*        g_host             = nullptr;
bool             g_started          = false;
uint64_t         g_accepted_count   = 0;
std::atomic<bool>* g_running_flag   = nullptr;

void listener_loop(std::atomic<bool>* running) {
    ENetAddress addr;
    enet_address_set_host(&addr, "0.0.0.0");
    addr.port = CASTER_NET_PORT;

    ENetHost* host = enet_host_create(&addr,    // address to bind
                                      32,       // max peers
                                      2,        // max channels
                                      0, 0);    // unlimited up/down bandwidth
    if (!host) {
        // Bind failed (port in use, etc.) — leave g_host null so the status
        // panel can report the failure.
        return;
    }

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_host = host;
        g_started = true;
    }

    // Loop until the running flag flips to false. Use a 100 ms service timeout
    // so we wake up frequently enough to notice the stop signal.
    while (running->load()) {
        ENetEvent ev;
        int rc = 0;
        // Drain any pending events within this 100 ms window.
        while ((rc = enet_host_service(host, &ev, 100)) > 0) {
            switch (ev.type) {
                case ENET_EVENT_TYPE_CONNECT: {
                    // Placeholder: accept then immediately disconnect so the
                    // peer sees a clean close rather than a timeout.
                    enet_peer_disconnect(ev.peer, 0);
                    std::lock_guard<std::mutex> lk(g_mtx);
                    ++g_accepted_count;
                    break;
                }
                case ENET_EVENT_TYPE_RECEIVE:
                    enet_packet_destroy(ev.packet);
                    break;
                case ENET_EVENT_TYPE_DISCONNECT:
                    // Peer fully gone — nothing else to do.
                    break;
                default:
                    break;
            }
        }
        // rc < 0 would indicate a socket error; we don't act on it for the
        // skeleton — the loop will keep trying.
    }

    // Cleanup on this thread.
    enet_host_flush(host);
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        enet_host_destroy(host);
        g_host    = nullptr;
        g_started = false;
    }
}

} // namespace

bool net_listener_running() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_host != nullptr;
}

uint64_t net_listener_accepted_count() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_accepted_count;
}

void start_net_listener(std::atomic<bool>& running) {
    // Idempotent: if already started, do nothing.
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_started || g_thread.joinable()) return;
    }
    g_running_flag = &running;
    g_thread = std::thread(listener_loop, g_running_flag);
}

void stop_net_listener() {
    // The caller controls the `running` flag they passed in; we just join.
    if (g_thread.joinable()) {
        g_thread.join();
    }
    g_running_flag = nullptr;
}

} // namespace caster::dll
