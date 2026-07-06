// src/common/ipc/ipc_client.hpp
//
// Named-pipe client. Used by hook.dll to connect to the launcher's pipe
// and receive the config_buffer message.
//
// Lifecycle:
//   IpcClient client;
//   client.connect("\\\\.\\pipe\\caster_1234_pipe", 10000);
//   std::vector<uint8_t> buf(256);
//   size_t n = client.recv(buf.data(), buf.size());
//   config_buffer::Config cfg;
//   config_buffer::deserialize(buf.data(), n, cfg);
//   client.close();
//
// Threading: not thread-safe. The DLL's worker thread uses this.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace caster::common::ipc {

class IpcClient {
public:
    IpcClient() = default;
    ~IpcClient();

    IpcClient(const IpcClient&)            = delete;
    IpcClient& operator=(const IpcClient&) = delete;
    IpcClient(IpcClient&&)                 = delete;
    IpcClient& operator=(IpcClient&&)      = delete;

    // Connect to a named pipe. Returns false on failure or timeout.
    bool connect(const std::string& pipe_path, std::uint32_t timeout_ms);

    // Receive up to `size` bytes into `out`. Returns the number of bytes
    // received (0 on disconnect or error).
    std::size_t recv(void* out, std::size_t size);

    void close();
    bool is_connected() const { return pipe_handle_ != nullptr; }

private:
    void* pipe_handle_ = nullptr;  // HANDLE
};

} // namespace caster::common::ipc
