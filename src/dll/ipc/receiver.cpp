// src/dll/ipc/receiver.cpp

#include "receiver.hpp"
#include "ipc/ipc_client.hpp"
#include "ipc/pipe_name.hpp"
#include "logger.hpp"

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

    // 3. Receive the config buffer (one shot — launcher sends once and
    //    closes the pipe, so we read up to kMaxBufferSize and treat
    //    whatever we get as the message).
    std::uint8_t buf[common::ipc::config_buffer::kMaxBufferSize];
    std::size_t got = client.recv(buf, sizeof(buf));
    client.close();

    if (got == 0) {
        g_status = "Error: empty message";
        g_error_message = "IPC connected but received 0 bytes";
        common::logger::err("ipc_receiver: {}", g_error_message);
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

    // 5. Store + mark ready.
    g_config = cfg;
    g_ready.store(true);
    g_status = "Received: " + summarize(cfg);
    g_receiving.store(false);

    common::logger::info("ipc_receiver: config received ({} bytes)", got);
    common::logger::info("ipc_receiver: flags=0x{:02x} delay={} rollback={} win={} "
                 "host_player={} peer_port={} local_udp_port={} match_seed=0x{:08x} "
                 "peer_addr='{}'",
                 cfg.flags, cfg.delay, cfg.rollback, cfg.win_count,
                 cfg.host_player, cfg.peer_port, cfg.local_udp_port,
                 cfg.match_seed, cfg.peer_addr);
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

} // namespace caster::dll::ipc_receiver
