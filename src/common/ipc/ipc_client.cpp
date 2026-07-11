// src/common/ipc/ipc_client.cpp

#include "ipc_client.hpp"
#include "../logger.hpp"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

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

IpcClient::~IpcClient() {
    close();
}

bool IpcClient::connect(const std::string& pipe_path, std::uint32_t timeout_ms) {
    if (pipe_handle_) {
        logger::warn("IpcClient::connect: already connected");
        return true;
    }

    std::wstring wide = utf8_to_wide(pipe_path);

    // Try to open the pipe. If it doesn't exist yet (launcher hasn't called
    // listen()), WaitNamedPipe gives us a bounded retry loop.
    DWORD deadline = GetTickCount() + timeout_ms;
    for (;;) {
        HANDLE h = CreateFileW(
            wide.c_str(),
            GENERIC_READ | GENERIC_WRITE,  // duplex — DLL sends status back
            0,                  // no sharing
            nullptr,            // default security
            OPEN_EXISTING,
            0,                  // no overlapped — we use blocking ReadFile
            nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            pipe_handle_ = h;
            logger::info("IpcClient: connected to {}", pipe_path);
            return true;
        }

        DWORD err = GetLastError();
        if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND) {
            // Genuine error.
            logger::err("IpcClient: CreateFileW failed (err={})", err);
            return false;
        }

        // Pipe busy or not yet created — wait and retry.
        DWORD remaining = 0;
        DWORD now = GetTickCount();
        if (now >= deadline) {
            logger::warn("IpcClient: connect timed out ({} ms)", timeout_ms);
            return false;
        }
        remaining = deadline - now;

        // WaitNamedPipe caps at NMPWAIT_USE_DEFAULT_WAIT (~2s) per call.
        if (!WaitNamedPipeW(wide.c_str(),
                            remaining < 2000 ? remaining : 2000)) {
            // Wait failed or timed out — try CreateFile again to re-check.
            if (GetTickCount() >= deadline) {
                logger::warn("IpcClient: connect timed out ({} ms)", timeout_ms);
                return false;
            }
        }
    }
}

std::size_t IpcClient::recv(void* out, std::size_t size) {
    if (!pipe_handle_) {
        logger::err("IpcClient::recv: not connected");
        return 0;
    }
    DWORD got = 0;
    if (!ReadFile(pipe_handle_, out, static_cast<DWORD>(size),
                  &got, nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_BROKEN_PIPE) {
            logger::err("IpcClient: ReadFile failed (err={})", err);
        }
        // ERROR_BROKEN_PIPE = launcher closed the pipe after sending —
        // this is the normal end-of-message signal for our protocol.
        return 0;
    }
    return got;
}

void IpcClient::close() {
    if (pipe_handle_) {
        CloseHandle(pipe_handle_);
        pipe_handle_ = nullptr;
        logger::info("IpcClient: closed");
    }
}

bool IpcClient::send(const void* data, std::size_t size) {
    if (!pipe_handle_) return false;
    DWORD written = 0;
    if (!WriteFile(pipe_handle_, data, static_cast<DWORD>(size),
                   &written, nullptr)) {
        return false;
    }
    FlushFileBuffers(pipe_handle_);
    return written == size;
}

void* IpcClient::steal_handle() {
    void* h = pipe_handle_;
    pipe_handle_ = nullptr;
    return h;
}

} // namespace caster::common::ipc
