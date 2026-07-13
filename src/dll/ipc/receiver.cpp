// src/dll/ipc/receiver.cpp

#include "receiver.hpp"
#include "ipc/ipc_client.hpp"
#include "ipc/pipe_name.hpp"
#include "logger.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>

namespace caster::dll::ipc_receiver {

namespace {

std::mutex                                       g_mtx;
caster::common::ipc::config_buffer::Config       g_config;
std::atomic<bool>                                g_ready{false};
std::atomic<bool>                                g_receiving{false};
std::string                                      g_status = "Not received";
std::string                                      g_error_message;

// The IPC client handle is kept alive after receive() so that
// notify_launcher() can send status messages back to the launcher
// through the same pipe. The pipe is DUPLEX and the client opens
// with GENERIC_READ | GENERIC_WRITE.
void* g_ipc_pipe_handle = nullptr;  // HANDLE (INVALID_HANDLE_VALUE == not connected)

std::string summarize(const caster::common::ipc::config_buffer::Config& c) {
    std::string s;
    if (c.is_training())  s += "Training ";
    if (c.is_netplay())   s += "Netplay ";
    if (c.is_host())      s += "Host ";
    if (c.is_spectator()) s += "Spectator ";
    if (s.empty())        s = "Offline ";
    s += "| delay=" + std::to_string(c.delay);
    s += " rollback=" + std::to_string(c.rollback);
    s += " win=" + std::to_string(c.win_count);
    if (!c.peer_addr.empty()) {
        s += " peer=" + c.peer_addr + ":" + std::to_string(c.peer_port);
    }
    return s;
}

} // namespace

bool receive(std::uint32_t timeout_ms) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_receiving.store(true);
    g_status = "Receiving...";

    // 1. Find the pipe name from the env var.
    std::string pipe_name = common::ipc::pipe_name::from_env();
    if (pipe_name.empty()) {
        g_status = "No CASTER_PIPE";
        g_error_message = "env var CASTER_PIPE not set (manual injection?)";
        common::logger::warn("ipc_receiver: {}", g_error_message);
        g_receiving.store(false);
        return false;
    }

    // 2. Connect to the pipe.
    common::ipc::IpcClient client;
    if (!client.connect(pipe_name, timeout_ms)) {
        g_status = "Error: connect failed";
        g_error_message = "Failed to connect to " + pipe_name +
                          " within " + std::to_string(timeout_ms) + " ms";
        common::logger::err("ipc_receiver: {}", g_error_message);
        g_receiving.store(false);
        return false;
    }

    // 3. Receive the config buffer (one shot — launcher sends once).
    //    We do NOT close the client — it's kept alive for notify_launcher().
    std::uint8_t buf[common::ipc::config_buffer::kMaxBufferSize];
    std::size_t got = client.recv(buf, sizeof(buf));

    if (got == 0) {
        g_status = "Error: empty message";
        g_error_message = "IPC connected but received 0 bytes";
        common::logger::err("ipc_receiver: {}", g_error_message);
        client.close();
        g_receiving.store(false);
        return false;
    }

    // 4. Deserialize.
    common::ipc::config_buffer::Config cfg;
    if (!common::ipc::config_buffer::deserialize(buf, got, cfg)) {
        g_status = "Error: deserialize failed";
        g_error_message = "Failed to deserialize " + std::to_string(got) +
                          " bytes";
        common::logger::err("ipc_receiver: {}", g_error_message);
        g_receiving.store(false);
        return false;
    }

    // 5. Store + mark ready. Steal the pipe handle so we can send
    //    status notifications back to the launcher later.
    g_config = cfg;
    g_ready.store(true);
    g_status = "Received: " + summarize(cfg);
    g_receiving.store(false);
    g_ipc_pipe_handle = client.steal_handle();

    common::logger::info("ipc_receiver: config received ({} bytes)", got);
    common::logger::info("ipc_receiver: flags=0x{:02x} delay={} rollback={} win={} "
                 "host_player={} peer_port={} local_udp_port={} match_seed=0x{:08x} "
                 "peer_addr='{}' local_name='{}' remote_name='{}'",
                 cfg.flags, cfg.delay, cfg.rollback, cfg.win_count,
                 cfg.host_player, cfg.peer_port, cfg.local_udp_port,
                 cfg.match_seed, cfg.peer_addr, cfg.local_name, cfg.remote_name);
    return true;
}

bool is_ready() {
    return g_ready.load();
}

bool get_config(caster::common::ipc::config_buffer::Config& out) {
    if (!g_ready.load()) return false;
    std::lock_guard<std::mutex> lk(g_mtx);
    out = g_config;
    return true;
}

std::string status_string() {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_status;
}

void notify_launcher(const std::string& message) {
    if (!g_ipc_pipe_handle) return;

    // Protocol: newline-terminated text. The launcher reads available
    // bytes via try_recv() and splits on '\n' to recover individual
    // messages.
    std::string msg = message + "\n";
    DWORD written = 0;
    if (!WriteFile(g_ipc_pipe_handle, msg.data(),
                   static_cast<DWORD>(msg.size()), &written, nullptr)) {
        // Pipe is broken — launcher probably already closed. Not an error.
        return;
    }
    FlushFileBuffers(g_ipc_pipe_handle);
}

} // namespace caster::dll::ipc_receiver
