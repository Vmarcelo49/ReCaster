// Smoke test for relay_protocol: encode/decode round-trip.
#include "relay/relay_protocol.hpp"
#include "logger.hpp"

#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;
void check(bool ok, const std::string& label) {
    std::printf("%s %s\n", ok ? "  OK   " : "  FAIL ", label.c_str());
    if (!ok) ++failures;
}

int main() {
    namespace rp = caster::common::net::relay_protocol;
    namespace lg = caster::common::logger;
    lg::init({}, true);

    // 1. encode_host_register
    {
        char buf[64];
        std::string_view code("ABCD");
        std::size_t n = rp::encode_host_register(buf, sizeof(buf),
                                                  rp::kTypeUdp, 46318, code);
        check(n == 8, "encode_host_register: size==8");
        check(static_cast<std::uint8_t>(buf[0]) == 'U', "  type='U'");
        // Port 46318 = 0xB4EE, LE = 0xEE 0xB4
        check(static_cast<std::uint8_t>(buf[1]) == 0xEE &&
              static_cast<std::uint8_t>(buf[2]) == 0xB4,
              "  port LE");
        check(static_cast<std::uint8_t>(buf[3]) == 4, "  code_len=4");
        check(std::memcmp(buf + 4, "ABCD", 4) == 0, "  code='ABCD'");
    }

    // 2. encode_host_register with empty code (server assigns)
    {
        char buf[64];
        std::string_view code("");
        std::size_t n = rp::encode_host_register(buf, sizeof(buf),
                                                  rp::kTypeUdp, 3939, code);
        check(n == 4, "encode_host_register empty code: size==4");
        check(static_cast<std::uint8_t>(buf[3]) == 0, "  code_len=0");
    }

    // 3. encode_client_join
    {
        char buf[64];
        std::string_view code("WXYZ");
        std::size_t n = rp::encode_client_join(buf, sizeof(buf),
                                                rp::kTypeUdp, code);
        check(n == 6, "encode_client_join: size==6");
        check(static_cast<std::uint8_t>(buf[0]) == 'U', "  type='U'");
        check(static_cast<std::uint8_t>(buf[1]) == 4, "  code_len=4");
        check(std::memcmp(buf + 2, "WXYZ", 4) == 0, "  code='WXYZ'");
    }

    // 4. encode_client_join rejects bad code length
    {
        char buf[64];
        std::string_view code("ABC");  // too short
        std::size_t n = rp::encode_client_join(buf, sizeof(buf),
                                                rp::kTypeUdp, code);
        check(n == 0, "encode_client_join bad code len -> 0");
    }

    // 5. encode_udp_data
    {
        char buf[8];
        std::size_t n = rp::encode_udp_data(buf, sizeof(buf), true, 0x12345678);
        check(n == 5, "encode_udp_data: size==5");
        check(static_cast<std::uint8_t>(buf[0]) == 1, "  isClient=1");
        // matchId LE = 0x78 0x56 0x34 0x12
        check(static_cast<std::uint8_t>(buf[1]) == 0x78 &&
              static_cast<std::uint8_t>(buf[2]) == 0x56 &&
              static_cast<std::uint8_t>(buf[3]) == 0x34 &&
              static_cast<std::uint8_t>(buf[4]) == 0x12,
              "  matchId LE");
    }

    // 6. encode_udp_data rejects match_id=0
    {
        char buf[8];
        std::size_t n = rp::encode_udp_data(buf, sizeof(buf), false, 0);
        check(n == 0, "encode_udp_data match_id=0 -> 0");
    }

    // 7. encode_stun_probe
    {
        char buf[8];
        std::size_t n = rp::encode_stun_probe(buf, sizeof(buf));
        check(n == 1, "encode_stun_probe: size==1");
        check(buf[0] == 'X', "  byte='X'");
    }

    // 8. decode_stun_reply (big-endian per RFC 5389)
    {
        std::uint8_t data[8] = {192, 168, 1, 10, 0x12, 0x34, 0, 0};
        auto r = rp::decode_stun_reply(data, 8);
        check(r.has_value(), "decode_stun_reply: has value");
        check(r->ip[0] == 192 && r->ip[1] == 168 &&
              r->ip[2] == 1 && r->ip[3] == 10,
              "  ip=192.168.1.10");
        check(r->port == 0x1234, "  port=0x1234 (BE)");
    }

    // 9. decode_stun_reply rejects short input
    {
        std::uint8_t data[7] = {0};
        auto r = rp::decode_stun_reply(data, 7);
        check(!r.has_value(), "decode_stun_reply short -> nullopt");
    }

    // 10. decode MatchInfo
    {
        std::uint8_t data[13];
        std::memcpy(data, "MatchInfo", 9);
        data[9] = 0x78; data[10] = 0x56;
        data[11] = 0x34; data[12] = 0x12;
        auto msg = rp::decode_server_msg(data, 13);
        check(msg.kind == rp::ServerMsgKind::MatchInfo,
              "decode MatchInfo: kind");
        check(msg.match_info.match_id == 0x12345678,
              "  match_id LE");
        check(rp::consumed_bytes(msg, 13) == 13, "  consumed=13");
    }

    // 11. decode Hosted
    {
        std::uint8_t data[10];
        std::memcpy(data, "Hosted", 6);
        std::memcpy(data + 6, "ABCD", 4);
        auto msg = rp::decode_server_msg(data, 10);
        check(msg.kind == rp::ServerMsgKind::Hosted, "decode Hosted: kind");
        check(msg.hosted.code == "ABCD", "  code='ABCD'");
        check(rp::consumed_bytes(msg, 10) == 10, "  consumed=10");
    }

    // 12. decode TunInfo
    {
        std::string s = "TunInfo";
        s += std::string(4, '\0');  // matchId (placeholder)
        s += "203.0.113.10:54321";
        s += '\0';  // NUL terminator
        // Set matchId = 0x12345678 LE
        s[7] = 0x78; s[8] = 0x56; s[9] = 0x34; s[10] = 0x12;
        auto msg = rp::decode_server_msg(
            reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
        check(msg.kind == rp::ServerMsgKind::TunInfo, "decode TunInfo: kind");
        check(msg.tun_info.match_id == 0x12345678, "  match_id LE");
        check(msg.tun_info.addr == "203.0.113.10:54321", "  addr");
    }

    // 13. decode TunInfo fragmented (no NUL yet) -> Unknown
    {
        std::string s = "TunInfo";
        s += std::string(4, '\0');
        s += "203.0.113.10:54321";  // no NUL terminator
        auto msg = rp::decode_server_msg(
            reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
        check(msg.kind == rp::ServerMsgKind::Unknown,
              "decode TunInfo fragmented -> Unknown");
    }

    // 14. decode Error
    {
        std::uint8_t data[20];
        std::memcpy(data, "Error", 5);
        data[5] = rp::kErrRoomNotFound;
        std::memcpy(data + 6, "Room not found", 14);
        auto msg = rp::decode_server_msg(data, 20);
        check(msg.kind == rp::ServerMsgKind::Error, "decode Error: kind");
        check(msg.error.code == rp::kErrRoomNotFound, "  code");
        check(msg.error.msg == "Room not found", "  msg");
    }

    // 15. decode unknown -> Unknown
    {
        std::uint8_t data[10] = "garbage!!";
        auto msg = rp::decode_server_msg(data, 10);
        check(msg.kind == rp::ServerMsgKind::Unknown,
              "decode garbage -> Unknown");
        check(rp::consumed_bytes(msg, 10) == 0, "  consumed=0");
    }

    // 16. generate_room_code
    {
        std::string code = rp::generate_room_code(42);
        check(code.size() == 4, "generate_room_code: size==4");
        check(rp::is_valid_room_code(code), "  valid");
        // Same seed = same code (deterministic).
        std::string code2 = rp::generate_room_code(42);
        check(code == code2, "  same seed = same code");
    }

    // 17. is_valid_room_code — 4 alphanumeric chars (A-Z or 0-9)
    {
        check(rp::is_valid_room_code("ABCD"), "valid 'ABCD'");
        check(rp::is_valid_room_code("WXYZ"), "valid 'WXYZ'");
        check(rp::is_valid_room_code("2345"), "valid '2345'");
        check(rp::is_valid_room_code("A0B1"), "valid 'A0B1' (0 and 1 allowed)");
        check(rp::is_valid_room_code("ABID"), "valid 'ABID' (I allowed)");
        check(rp::is_valid_room_code("ABOD"), "valid 'ABOD' (O allowed)");
        check(!rp::is_valid_room_code("ABC"), "invalid 'ABC' (too short)");
        check(!rp::is_valid_room_code("ABCDE"), "invalid 'ABCDE' (too long)");
        check(!rp::is_valid_room_code("AbCD"), "invalid 'AbCD' (lowercase)");
        check(!rp::is_valid_room_code("AB!D"), "invalid 'AB!D' (special char)");
    }

    // 18. Room code alphabet is A-Z + 0-9 (36 chars)
    {
        std::string alpha = rp::kRoomCodeAlphabet;
        check(alpha.size() == 36, "alphabet size == 36 (A-Z + 0-9)");
        check(alpha.find('I') != std::string::npos, "alphabet includes I");
        check(alpha.find('O') != std::string::npos, "alphabet includes O");
        check(alpha.find('0') != std::string::npos, "alphabet includes 0");
        check(alpha.find('1') != std::string::npos, "alphabet includes 1");
    }

    lg::shutdown();
    std::printf("\n");
    if (failures == 0) {
        std::printf("ALL RELAY PROTOCOL TESTS PASSED\n");
        return 0;
    } else {
        std::printf("%d TESTS FAILED\n", failures);
        return 1;
    }
}
