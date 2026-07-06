// Smoke test for Phase 1 common modules (ini, logger, config).
// Compiles with native g++ (no SDL2/ImGui needed).
// Just checks that the code parses + links against a tiny main.

#include "ini.hpp"
#include "logger.hpp"
#include "config.hpp"

#include <cstdio>
#include <filesystem>

int main() {
    namespace cc = caster::common;

    // Init logger to a temp file.
    auto tmp = std::filesystem::temp_directory_path() / "caster-smoke.log";
    cc::logger::init(tmp, true);
    cc::logger::info("smoke test starting");
    cc::logger::warn("warning test: x={}", 42);
    cc::logger::err("error test: {}", "msg");

    // INI round-trip.
    cc::ini::Document doc;
    doc.parse(R"(
# comment
[player]
display_name = TestPlayer

[match]
versus_win_count = 3
default_rollback = 5
high_cpu_priority = true

[network]
relays = relay1.example.com:46320\
relay2.example.com:46320
)");
    auto dumped = doc.dump();
    cc::logger::info("INI dump:\n{}", dumped);

    cc::ini::Document doc2;
    doc2.parse(dumped);
    if (doc2.getString("player", "display_name", "") != "TestPlayer") {
        cc::logger::err("INI round-trip failed: display_name mismatch");
        return 1;
    }
    if (doc2.getInt("match", "versus_win_count", -1) != 3) {
        cc::logger::err("INI round-trip failed: versus_win_count mismatch");
        return 1;
    }
    if (!doc2.getBool("match", "high_cpu_priority", false)) {
        cc::logger::err("INI round-trip failed: high_cpu_priority mismatch");
        return 1;
    }
    cc::logger::info("INI round-trip OK");

    // Config load/save.
    auto cfg_path = std::filesystem::temp_directory_path() / "caster-smoke.ini";
    if (std::filesystem::exists(cfg_path)) std::filesystem::remove(cfg_path);

    auto cfg = cc::config::load(cfg_path.string());
    cc::logger::info("default display_name: '{}'", cfg.display_name);
    cc::logger::info("default rollback    : {}", cfg.default_rollback);
    cc::logger::info("default win count    : {}", cfg.versus_win_count);

    cfg.display_name = "SmokeTester";
    cfg.default_rollback = 6;
    if (!cc::config::save(cfg)) {
        cc::logger::err("config save failed");
        return 1;
    }

    auto cfg2 = cc::config::load(cfg_path.string());
    if (cfg2.display_name != "SmokeTester" || cfg2.default_rollback != 6) {
        cc::logger::err("config round-trip failed");
        return 1;
    }
    cc::logger::info("config round-trip OK");

    cc::logger::info("smoke test PASSED");
    cc::logger::shutdown();
    return 0;
}
