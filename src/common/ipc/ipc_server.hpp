// src/common/ipc/ipc_server.hpp
//
// Named-pipe server. The launcher opens the server end with listen(),
// waits for the DLL to connect (the DLL is the client), then sends the
// config_buffer message and closes.
//
// Lifecycle:
//   IpcServer server;
//   server.listen("\\\\.\\pipe\\caster_1234_pipe");
//   // ... spawn game + inject DLL ...
//   server.wait_for_connection(10000);  // wait up to 10s
//   server.send(config_bytes.data(), config_bytes.size());
//   server.close();
//
// Threading: not thread-safe. The launcher calls these methods from its
// main thread only. The DLL side uses IpcClient (separate class).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace caster::common::ipc {

class IpcServer {
public:
    IpcServer() = default;
    ~IpcServer();

    IpcServer(const IpcServer&)            = delete;
    IpcServer& operator=(const IpcServer&) = delete;
    IpcServer(IpcServer&&)                 = delete;
    IpcServer& operator=(IpcServer&&)      = delete;

    // Create a named pipe and start listening. Returns false on failure
    // (e.g. name collision — extremely unlikely given PID-based naming).
    bool listen(const std::string& pipe_path);

    // Block until a client connects, or `timeout_ms` elapses. Returns
    // true on connection, false on timeout or error.
    bool wait_for_connection(std::uint32_t timeout_ms);

    // Send `size` bytes to the connected client. Returns true on success.
    // Caller must have called wait_for_connection() first.
    bool send(const void* data, std::size_t size);

    // Non-blocking read: returns up to `size` bytes into `out`. Returns
    // the number of bytes read (0 if no data available). Used to poll
    // for status messages from the DLL without blocking the UI.
    std::size_t try_recv(void* out, std::size_t size);

    // Close the pipe. Safe to call multiple times.
    void close();

    bool is_open() const { return pipe_handle_ != nullptr; }

private:
    void* pipe_handle_ = nullptr;  // HANDLE
};

} // namespace caster::common::ipc
