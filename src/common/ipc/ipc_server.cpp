// src/common/ipc/ipc_server.cpp

#include "ipc_server.hpp"
#include "../logger.hpp"
#include "../win32/process.hpp"  // for close_handle

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>

namespace caster::common::ipc {

namespace {

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                  static_cast<int>(s.size()),
                                  nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                        static_cast<int>(s.size()),
                        out.data(), len);
    return out;
}

} // namespace

IpcServer::~IpcServer() {
    close();
}

bool IpcServer::listen(const std::string& pipe_path) {
    if (pipe_handle_) {
        logger::warn("IpcServer::listen: already listening");
        return true;
    }

    std::wstring wide = utf8_to_wide(pipe_path);

    // Create the named pipe. We use:
    //   PIPE_ACCESS_DUPLEX    - both read and write (we only send, but
    //                           duplex is harmless and lets us read acks
    //                           in the future)
    //   FILE_FLAG_OVERLAPPED  - so wait_for_connection can use a timeout
    //                           via WaitForSingleObject
    //   PIPE_TYPE_BYTE        - stream semantics (no message framing)
    //   PIPE_WAIT             - blocking I/O for send()
    HANDLE h = CreateNamedPipeW(
        wide.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,                       // max instances (only one launcher per PID)
        4096,                    // out buffer
        4096,                    // in buffer
        0,                       // default timeout (ignored for server)
        nullptr);                // default security (no inherit)
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        logger::err("IpcServer: CreateNamedPipeW failed (err={})", err);
        return false;
    }

    pipe_handle_ = h;
    logger::info("IpcServer: listening on {}", pipe_path);
    return true;
}

bool IpcServer::wait_for_connection(std::uint32_t timeout_ms) {
    if (!pipe_handle_) {
        logger::err("IpcServer::wait_for_connection: not listening");
        return false;
    }

    // ConnectNamedPipe blocks until a client connects. To support a timeout,
    // we use an overlapped I/O + WaitForSingleObject.
    OVERLAPPED ov{};
    ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        DWORD err = GetLastError();
        logger::err("IpcServer: CreateEvent failed (err={})", err);
        return false;
    }

    BOOL connected = ConnectNamedPipe(pipe_handle_, &ov);
    DWORD last_err = GetLastError();

    if (connected) {
        // ConnectNamedPipe returning TRUE on an overlapped pipe is unusual
        // but possible if a client was already waiting.
        CloseHandle(ov.hEvent);
        logger::info("IpcServer: client connected");
        return true;
    }
    if (last_err == ERROR_PIPE_CONNECTED) {
        // Client connected between CreateNamedPipe and ConnectNamedPipe.
        CloseHandle(ov.hEvent);
        logger::info("IpcServer: client already connected");
        return true;
    }
    if (last_err != ERROR_IO_PENDING) {
        // Genuine failure.
        logger::err("IpcServer: ConnectNamedPipe failed (err={})", last_err);
        CloseHandle(ov.hEvent);
        return false;
    }

    // ERROR_IO_PENDING — wait for the event with timeout.
    DWORD wait = WaitForSingleObject(ov.hEvent, timeout_ms);
    CloseHandle(ov.hEvent);

    if (wait == WAIT_TIMEOUT) {
        logger::warn("IpcServer: wait_for_connection timed out ({} ms)",
                     timeout_ms);
        return false;
    }
    if (wait != WAIT_OBJECT_0) {
        DWORD err = GetLastError();
        logger::err("IpcServer: WaitForSingleObject failed (err={})", err);
        return false;
    }

    // Check the overlapped result to confirm the connect succeeded.
    DWORD bytes_transferred = 0;
    if (!GetOverlappedResult(pipe_handle_, &ov, &bytes_transferred, FALSE)) {
        DWORD err = GetLastError();
        logger::err("IpcServer: GetOverlappedResult failed (err={})", err);
        return false;
    }

    logger::info("IpcServer: client connected");
    return true;
}

bool IpcServer::send(const void* data, std::size_t size) {
    if (!pipe_handle_) {
        logger::err("IpcServer::send: not connected");
        return false;
    }
    DWORD written = 0;
    if (!WriteFile(pipe_handle_, data, static_cast<DWORD>(size),
                   &written, nullptr)) {
        DWORD err = GetLastError();
        logger::err("IpcServer: WriteFile failed (err={})", err);
        return false;
    }
    if (written != size) {
        logger::err("IpcServer: short write ({} of {} bytes)", written, size);
        return false;
    }
    FlushFileBuffers(pipe_handle_);
    return true;
}

void IpcServer::close() {
    if (pipe_handle_) {
        // DisconnectNamedPipe is needed if a client is connected; CloseHandle
        // alone leaves the client hanging.
        DisconnectNamedPipe(pipe_handle_);
        CloseHandle(pipe_handle_);
        pipe_handle_ = nullptr;
        logger::info("IpcServer: closed");
    }
}

} // namespace caster::common::ipc
