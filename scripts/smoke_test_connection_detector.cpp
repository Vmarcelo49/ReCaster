// Smoke test for connection_detector: parse_input() classification.
#include "net/connection_detector.hpp"
#include "logger.hpp"

#include <cstdio>
#include <string>

static int failures = 0;
void check(bool ok, const std::string& label) {
    std::printf("%s %s\n", ok ? "  OK   " : "  FAIL ", label.c_str());
    if (!ok) ++failures;
}

int main() {
    namespace cd = caster::common::net::connection_detector;
    namespace lg = caster::common::logger;
    lg::init({}, true);

    // 1. Empty input → Empty type
    {
        auto r = cd::parse_input("");
        check(r.type == cd::InputType::Empty, "empty string -> Empty");
    }
    {
        auto r = cd::parse_input("   ");
        check(r.type == cd::InputType::Empty, "whitespace-only -> Empty");
    }

    // 2. Pure port number → Port
    {
        auto r = cd::parse_input("46318");
        check(r.type == cd::InputType::Port, "'46318' -> Port");
        check(r.port == 46318, "  port == 46318");
    }
    {
        auto r = cd::parse_input("80");
        check(r.type == cd::InputType::Port, "'80' -> Port");
        check(r.port == 80, "  port == 80");
    }
    {
        auto r = cd::parse_input("65535");
        check(r.type == cd::InputType::Port, "'65535' -> Port (max)");
    }
    {
        auto r = cd::parse_input("0");
        check(r.type == cd::InputType::Invalid, "'0' -> Invalid (port must be >= 1)");
    }
    {
        auto r = cd::parse_input("65536");
        check(r.type == cd::InputType::Invalid, "'65536' -> Invalid (port must be <= 65535)");
    }
    {
        auto r = cd::parse_input("99999");
        check(r.type == cd::InputType::Invalid, "'99999' -> Invalid (port must be <= 65535)");
    }

    // 3. host:port → IpPort
    {
        auto r = cd::parse_input("192.168.1.10:46318");
        check(r.type == cd::InputType::IpPort, "'192.168.1.10:46318' -> IpPort");
        check(r.host == "192.168.1.10", "  host == 192.168.1.10");
        check(r.port == 46318, "  port == 46318");
    }
    {
        auto r = cd::parse_input("localhost:8080");
        check(r.type == cd::InputType::IpPort, "'localhost:8080' -> IpPort");
        check(r.host == "localhost", "  host == localhost");
    }
    {
        auto r = cd::parse_input("example.com:443");
        check(r.type == cd::InputType::IpPort, "'example.com:443' -> IpPort");
    }
    {
        auto r = cd::parse_input(":46318");
        check(r.type == cd::InputType::Invalid, "':46318' -> Invalid (empty host)");
    }
    {
        auto r = cd::parse_input("192.168.1.10:");
        check(r.type == cd::InputType::Invalid, "'192.168.1.10:' -> Invalid (empty port)");
    }
    {
        auto r = cd::parse_input("192.168.1.10:abc");
        check(r.type == cd::InputType::Invalid,
              "'192.168.1.10:abc' -> Invalid (non-numeric port)");
    }
    {
        auto r = cd::parse_input("192.168.1.10:99999");
        check(r.type == cd::InputType::Invalid,
              "'192.168.1.10:99999' -> Invalid (port too large)");
    }

    // 4. #ABCD → RoomCode (4 alphanumeric chars: A-Z or 0-9)
    {
        auto r = cd::parse_input("#ABCD");
        check(r.type == cd::InputType::RoomCode, "'#ABCD' -> RoomCode");
        check(r.room_code == "ABCD", "  room_code == ABCD");
    }
    {
        auto r = cd::parse_input("#WXYZ");
        check(r.type == cd::InputType::RoomCode, "'#WXYZ' -> RoomCode");
    }
    {
        auto r = cd::parse_input("#AB2D");
        check(r.type == cd::InputType::RoomCode, "'#AB2D' -> RoomCode (digit 2 allowed)");
        check(r.room_code == "AB2D", "  room_code == AB2D");
    }
    {
        auto r = cd::parse_input("#2345");
        check(r.type == cd::InputType::RoomCode, "'#2345' -> RoomCode (all digits)");
    }
    {
        auto r = cd::parse_input("#A0B1");
        check(r.type == cd::InputType::RoomCode, "'#A0B1' -> RoomCode (0 and 1 allowed)");
    }
    {
        auto r = cd::parse_input("#ABID");
        check(r.type == cd::InputType::RoomCode, "'#ABID' -> RoomCode (letter I allowed)");
    }
    {
        auto r = cd::parse_input("#ABOD");
        check(r.type == cd::InputType::RoomCode, "'#ABOD' -> RoomCode (letter O allowed)");
    }
    {
        auto r = cd::parse_input("#abc");
        check(r.type == cd::InputType::Invalid,
              "'#abc' -> Invalid (lowercase not allowed)");
    }
    {
        auto r = cd::parse_input("#ABC");
        check(r.type == cd::InputType::Invalid,
              "'#ABC' -> Invalid (too short, must be 4 chars)");
    }
    {
        auto r = cd::parse_input("#ABCDE");
        check(r.type == cd::InputType::Invalid,
              "'#ABCDE' -> Invalid (too long, must be 4 chars)");
    }
    {
        auto r = cd::parse_input("#AB!D");
        check(r.type == cd::InputType::Invalid,
              "'#AB!D' -> Invalid (special char not allowed)");
    }

    // 5. Invalid inputs
    {
        auto r = cd::parse_input("garbage");
        check(r.type == cd::InputType::Invalid, "'garbage' -> Invalid");
        check(!r.error.empty(), "  error message is non-empty");
    }
    {
        auto r = cd::parse_input("192.168.1.10");
        check(r.type == cd::InputType::Invalid,
              "'192.168.1.10' (no port) -> Invalid");
    }

    // 6. Whitespace trimming
    {
        auto r = cd::parse_input("  46318  ");
        check(r.type == cd::InputType::Port, "'  46318  ' -> Port (trimmed)");
        check(r.port == 46318, "  port == 46318");
    }
    {
        auto r = cd::parse_input("  #ABCD  ");
        check(r.type == cd::InputType::RoomCode, "'  #ABCD  ' -> RoomCode (trimmed)");
    }

    // 7. type_label() returns non-empty strings
    {
        check(std::string(cd::type_label(cd::InputType::Empty))    != "", "label(Empty) non-empty");
        check(std::string(cd::type_label(cd::InputType::Port))     != "", "label(Port) non-empty");
        check(std::string(cd::type_label(cd::InputType::IpPort))   != "", "label(IpPort) non-empty");
        check(std::string(cd::type_label(cd::InputType::RoomCode)) != "", "label(RoomCode) non-empty");
        check(std::string(cd::type_label(cd::InputType::Invalid))  != "", "label(Invalid) non-empty");
    }

    // 8. classify() convenience
    {
        check(cd::classify("") == cd::InputType::Empty, "classify('') == Empty");
        check(cd::classify("46318") == cd::InputType::Port, "classify('46318') == Port");
        check(cd::classify("#ABCD") == cd::InputType::RoomCode, "classify('#ABCD') == RoomCode");
    }

    lg::shutdown();
    std::printf("\n");
    if (failures == 0) {
        std::printf("ALL CONNECTION DETECTOR TESTS PASSED\n");
        return 0;
    } else {
        std::printf("%d TESTS FAILED\n", failures);
        return 1;
    }
}
