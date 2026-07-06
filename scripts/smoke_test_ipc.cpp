// Smoke test for IPC config_buffer: serialize → deserialize round-trip.
#include "config_buffer.hpp"
#include "logger.hpp"

#include <cstdio>
#include <cstring>

static int failures = 0;
void check(bool ok, const char* label) {
    std::printf("%s %s\n", ok ? "  OK   " : "  FAIL ", label);
    if (!ok) ++failures;
}

int main() {
    namespace cb = caster::common::ipc::config_buffer;
    namespace lg = caster::common::logger;
    lg::init({}, true);

    // 1. Empty peer_addr (offline training case)
    {
        cb::Config in{};
        in.flags = cb::kFlagTraining;
        in.delay = 4;
        in.rollback = 6;
        in.win_count = 3;
        in.host_player = 1;
        in.peer_port = 0;
        in.local_udp_port = 0;
        in.match_seed = 0xDEADBEEF;
        in.peer_addr = "";

        std::string buf = cb::serialize_to_string(in);
        check(buf.size() == 14, "empty peer_addr: size==14");

        cb::Config out;
        bool ok = cb::deserialize(reinterpret_cast<const std::uint8_t*>(buf.data()),
                                  buf.size(), out);
        check(ok, "empty peer_addr: deserialize ok");
        check(out.flags == in.flags, "empty peer_addr: flags match");
        check(out.delay == in.delay, "empty peer_addr: delay match");
        check(out.rollback == in.rollback, "empty peer_addr: rollback match");
        check(out.win_count == in.win_count, "empty peer_addr: win_count match");
        check(out.match_seed == in.match_seed, "empty peer_addr: match_seed match");
        check(out.peer_addr == "", "empty peer_addr: peer_addr empty");
    }

    // 2. With peer_addr (direct join case)
    {
        cb::Config in{};
        in.flags = cb::kFlagNetplay | cb::kFlagHost;
        in.delay = 2;
        in.rollback = 4;
        in.win_count = 2;
        in.host_player = 1;
        in.peer_port = 46318;
        in.local_udp_port = 51234;
        in.match_seed = 0x12345678;
        in.peer_addr = "192.168.1.10";

        std::string buf = cb::serialize_to_string(in);
        // 13 (header) + 12 (strlen "192.168.1.10") + 1 (NUL) = 26
        check(buf.size() == 26, "with peer_addr: size==26");

        cb::Config out;
        bool ok = cb::deserialize(reinterpret_cast<const std::uint8_t*>(buf.data()),
                                  buf.size(), out);
        check(ok, "with peer_addr: deserialize ok");
        check(out.peer_addr == "192.168.1.10", "with peer_addr: peer_addr match");
        check(out.peer_port == 46318, "with peer_addr: peer_port match");
        check(out.local_udp_port == 51234, "with peer_addr: local_udp_port match");
        check(out.is_netplay(), "with peer_addr: is_netplay() true");
        check(out.is_host(), "with peer_addr: is_host() true");
        check(!out.is_training(), "with peer_addr: is_training() false");
    }

    // 3. Long peer_addr (relay with full IPv6 + port)
    {
        cb::Config in{};
        in.peer_addr = "[2001:0db8:85a3:0000:0000:8a2e:0370:7334]:46318";
        std::string buf = cb::serialize_to_string(in);
        check(buf.size() < cb::kMaxBufferSize, "long peer_addr: within buffer");

        cb::Config out;
        cb::deserialize(reinterpret_cast<const std::uint8_t*>(buf.data()),
                        buf.size(), out);
        check(out.peer_addr == in.peer_addr, "long peer_addr: round-trip ok");
    }

    // 4. Too-short buffer should fail
    {
        cb::Config out;
        std::uint8_t tiny[5] = {1, 2, 3, 4, 5};
        bool ok = cb::deserialize(tiny, 5, out);
        check(!ok, "tiny buffer: rejected");
    }

    // 5. Manual buffer inspection (verify wire format)
    {
        cb::Config in{};
        in.flags = 0x02;  // netplay
        in.delay = 1;
        in.rollback = 4;
        in.win_count = 2;
        in.host_player = 1;
        in.peer_port = 0xB5AE;  // 46318
        in.local_udp_port = 0xC822;  // 51234
        in.match_seed = 0x12345678;

        std::string buf = cb::serialize_to_string(in);
        // Byte 0 = flags (0x02)
        check(static_cast<std::uint8_t>(buf[0]) == 0x02, "wire: byte 0 = flags");
        // Byte 1 = delay (1)
        check(static_cast<std::uint8_t>(buf[1]) == 1, "wire: byte 1 = delay");
        // Bytes 5-6 = peer_port LE (0xAE 0xB5)
        check(static_cast<std::uint8_t>(buf[5]) == 0xAE &&
              static_cast<std::uint8_t>(buf[6]) == 0xB5,
              "wire: bytes 5-6 = peer_port LE");
        // Bytes 9-12 = match_seed LE (0x78 0x56 0x34 0x12)
        check(static_cast<std::uint8_t>(buf[9])  == 0x78 &&
              static_cast<std::uint8_t>(buf[10]) == 0x56 &&
              static_cast<std::uint8_t>(buf[11]) == 0x34 &&
              static_cast<std::uint8_t>(buf[12]) == 0x12,
              "wire: bytes 9-12 = match_seed LE");
    }

    lg::shutdown();
    std::printf("\n");
    if (failures == 0) {
        std::printf("ALL IPC TESTS PASSED\n");
        return 0;
    } else {
        std::printf("%d TESTS FAILED\n", failures);
        return 1;
    }
}
