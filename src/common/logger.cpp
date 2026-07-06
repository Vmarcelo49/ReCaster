// src/common/logger.cpp

#include "logger.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace caster::common::logger {

namespace {

struct LoggerState {
    std::mutex              mtx;
    std::ofstream           file;
    std::filesystem::path   path;
    bool                    mirror_to_stdout = false;
    bool                    initialized      = false;
};

LoggerState& state() {
    static LoggerState s;
    return s;
}

std::filesystem::path default_log_path() {
#ifdef _WIN32
    // %LOCALAPPDATA%\caster\debug.log
    char buf[MAX_PATH] = {0};
    DWORD len = GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        // Fallback: current working directory.
        return std::filesystem::current_path() / "caster-debug.log";
    }
    std::filesystem::path base = std::filesystem::path(buf) / "caster";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    return base / "debug.log";
#else
    return std::filesystem::current_path() / "caster-debug.log";
#endif
}

std::string format_timestamp() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t  = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d}",
                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                       tm.tm_hour, tm.tm_min, tm.tm_sec,
                       static_cast<unsigned>(ms.count()));
}

void log_raw(char severity, std::string_view msg) {
    LoggerState& s = state();
    std::lock_guard<std::mutex> lk(s.mtx);

    auto tid = std::this_thread::get_id();
    std::string line = std::format("[{}] [{}] [{}] {}",
                                   format_timestamp(),
                                   severity,
                                   std::hash<std::thread::id>{}(tid),
                                   msg);

    if (s.file.is_open()) {
        s.file << line << '\n';
        s.file.flush();
    }
    if (s.mirror_to_stdout || !s.file.is_open()) {
        std::fputs(line.c_str(), stdout);
        std::fputc('\n', stdout);
    }
}

} // namespace

void init(std::filesystem::path path, bool mirror_to_stdout) {
    LoggerState& s = state();
    std::lock_guard<std::mutex> lk(s.mtx);

    if (path.empty()) path = default_log_path();
    s.path = path;
    s.mirror_to_stdout = mirror_to_stdout;

    // Open in append mode so multiple sessions accumulate.
    s.file.open(path, std::ios::app | std::ios::binary);
    s.initialized = true;

    // Banner: a separator line so it's easy to find new sessions in the log.
    if (s.file.is_open()) {
        s.file << "\n========================================"
                  "========================================\n"
               << "[" << format_timestamp()
               << "] [I] [main] caster session starting\n";
        s.file.flush();
    }
}

void shutdown() {
    LoggerState& s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (s.file.is_open()) {
        s.file << "[" << format_timestamp()
               << "] [I] [main] caster session ending\n";
        s.file.close();
    }
    s.initialized = false;
}

void info(std::string_view msg) { log_raw('I', msg); }
void warn(std::string_view msg) { log_raw('W', msg); }
void err (std::string_view msg) { log_raw('E', msg); }

std::filesystem::path current_log_path() {
    LoggerState& s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    return s.path;
}

} // namespace caster::common::logger
