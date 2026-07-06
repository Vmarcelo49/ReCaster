// src/exe/cli_args.cpp

#include "cli_args.hpp"

#include "config.hpp"  // for kMaxNameLen — provided by caster_common's PUBLIC include dir

#include <stdexcept>
#include <string>
#include <string_view>

namespace caster::exe::cli {

namespace {

// Try to parse `--flag=value` or `--flag value` for a flag that takes a value.
// Returns the value string, or empty if `arg` doesn't match `flag`.
// If `arg == flag` exactly, the value is taken from `next_arg` (advanced).
std::optional<std::string> extractValue(std::string_view arg,
                                        std::string_view flag,
                                        std::string_view next_arg,
                                        bool& consumed_next) {
    consumed_next = false;
    // --flag=value
    if (arg.starts_with(flag) && arg.size() > flag.size() && arg[flag.size()] == '=') {
        return std::string(arg.substr(flag.size() + 1));
    }
    // --flag value (only if next_arg doesn't look like another flag)
    if (arg == flag) {
        if (next_arg.empty() || next_arg[0] == '-') {
            return std::string{};  // signal "missing value" via empty
        }
        consumed_next = true;
        return std::string(next_arg);
    }
    return std::nullopt;
}

int parseInt(std::string_view s, std::string_view flag_name) {
    if (s.empty()) {
        throw std::runtime_error(
            std::string(flag_name) + " requires an integer argument");
    }
    try {
        size_t pos = 0;
        int v = std::stoi(std::string(s), &pos);
        if (pos != s.size()) {
            throw std::runtime_error(
                std::string(flag_name) + ": trailing garbage in '" + std::string(s) + "'");
        }
        return v;
    } catch (const std::exception&) {
        throw std::runtime_error(
            std::string(flag_name) + ": invalid integer '" + std::string(s) + "'");
    }
}

void validatePort(int p) {
    if (p < 1 || p > 65535) {
        throw std::runtime_error("--port must be between 1 and 65535");
    }
}
void validateDelay(int d) {
    if (d < 0 || d > 8) {
        throw std::runtime_error("--delay must be between 0 and 8 (frames)");
    }
}
void validateRollback(int r) {
    if (r < 0 || r > 20) {
        throw std::runtime_error("--rollback must be between 0 and 20 (frames)");
    }
}

} // namespace

std::string helpText() {
    return
        "caster — rollback netplay launcher (C++23)\n"
        "\n"
        "USAGE:\n"
        "    caster.exe [MODE] [OPTIONS]\n"
        "\n"
        "MODES (mutually exclusive; last one wins):\n"
        "    --training           Launch in Training mode (offline)\n"
        "    --versus             Launch in Versus mode   (offline)\n"
        "    --host               Host a netplay session\n"
        "    --join=PEER          Join a peer (host:port or #room)\n"
        "    --spec=PEER          Spectate a peer (host:port; relay not yet supported)\n"
        "\n"
        "    (no mode flag)       Open interactive menu (default)\n"
        "\n"
        "OPTIONS:\n"
        "    --port=N             UDP port for hosting / local bind (default: 46318)\n"
        "    --delay=N            Input delay override in frames (0..8; default: auto)\n"
        "    --rollback=N         Rollback window in frames (default: 4)\n"
        "    --name=NAME          Display name override (max 31 chars)\n"
        "    -h, --help           Show this help and exit\n"
        "\n"
        "PEER FORMAT for --join and --spec:\n"
        "    host:port            Direct ENet connection (e.g. 192.168.1.10:46318)\n"
        "    #ABCD                Relay room code (4-letter; --join only for now)\n"
        "\n"
        "EXAMPLES:\n"
        "    caster.exe                              # open GUI menu\n"
        "    caster.exe --training                   # offline training\n"
        "    caster.exe --host --port=46318          # host on port 46318\n"
        "    caster.exe --join=192.168.1.10:46318    # direct join\n"
        "    caster.exe --join=#ABCD                 # relay join\n"
        "    caster.exe --spec=192.168.1.10:46318    # spectate direct\n"
        "\n"
        "CONFIG:\n"
        "    caster/config.ini next to the .exe (created on first save).\n"
        "    Log file: %LOCALAPPDATA%\\caster\\debug.log\n";
}

Args parse(int argc, char** argv) {
    Args args;
    args.help_message = helpText();

    // We process args in order, recording the LAST mode flag seen (zzcaster
    // semantics: later overrides earlier).
    for (int i = 1; i < argc; ++i) {
        std::string_view a  = argv[i];
        std::string_view na = (i + 1 < argc) ? std::string_view(argv[i + 1])
                                             : std::string_view{};

        if (a == "-h" || a == "--help") {
            args.help_requested = true;
            continue;
        }
        if (a == "--training") { args.mode = Mode::Training; continue; }
        if (a == "--versus")   { args.mode = Mode::Versus;   continue; }
        if (a == "--host")     { args.mode = Mode::Host;     continue; }

        // --join=PEER / --spec=PEER
        bool consumed = false;
        if (auto v = extractValue(a, "--join", na, consumed)) {
            if (v->empty()) {
                throw std::runtime_error("--join requires a peer argument "
                                         "(host:port or #room)");
            }
            args.mode = Mode::Join;
            args.peer = *v;
            if (consumed) ++i;
            continue;
        }
        if (auto v = extractValue(a, "--spec", na, consumed)) {
            if (v->empty()) {
                throw std::runtime_error("--spec requires a peer argument "
                                         "(host:port or #room)");
            }
            args.mode = Mode::Spectate;
            args.peer = *v;
            if (consumed) ++i;
            continue;
        }
        if (auto v = extractValue(a, "--port", na, consumed)) {
            args.port = parseInt(*v, "--port");
            validatePort(args.port);
            if (consumed) ++i;
            continue;
        }
        if (auto v = extractValue(a, "--delay", na, consumed)) {
            args.delay = parseInt(*v, "--delay");
            validateDelay(args.delay);
            if (consumed) ++i;
            continue;
        }
        if (auto v = extractValue(a, "--rollback", na, consumed)) {
            args.rollback = parseInt(*v, "--rollback");
            validateRollback(args.rollback);
            if (consumed) ++i;
            continue;
        }
        if (auto v = extractValue(a, "--name", na, consumed)) {
            if (v->empty()) {
                throw std::runtime_error("--name requires a value");
            }
            if (v->size() > common::config::kMaxNameLen) {
                throw std::runtime_error(
                    "--name too long (max " +
                    std::to_string(common::config::kMaxNameLen) + " chars)");
            }
            args.name = *v;
            if (consumed) ++i;
            continue;
        }

        throw std::runtime_error("unknown argument: " + std::string(a));
    }

    return args;
}

} // namespace caster::exe::cli
