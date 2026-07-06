// Full link test for Phase 2 common code (no SDL/ImGui deps).
#include "embedded_font.hpp"
#include "ini.hpp"
#include "logger.hpp"
#include "config.hpp"

#include <cstdio>
#include <filesystem>

int main() {
    namespace cc = caster::common;

    // Verify embedded font is accessible.
    if (cc::embedded_font::size != 159108) {
        std::fprintf(stderr, "FAIL: font size mismatch (%zu != 159108)\n",
                     cc::embedded_font::size);
        return 1;
    }
    if (cc::embedded_font::data[0] != 0x00 || cc::embedded_font::data[1] != 0x01) {
        std::fprintf(stderr, "FAIL: font magic bytes wrong (0x%02x 0x%02x)\n",
                     cc::embedded_font::data[0], cc::embedded_font::data[1]);
        return 1;
    }
    std::printf("OK: embedded font (%zu bytes, magic 0x00 0x01)\n",
                cc::embedded_font::size);

    // Round-trip INI.
    cc::ini::Document doc;
    doc.parse("[test]\nkey=value\n");
    if (doc.getString("test", "key", "") != "value") {
        std::fprintf(stderr, "FAIL: INI round-trip\n");
        return 1;
    }
    std::printf("OK: INI round-trip\n");

    // Logger + config.
    auto tmp_log = std::filesystem::temp_directory_path() / "phase2-link.log";
    cc::logger::init(tmp_log, true);
    cc::logger::info("link test: all common modules linked");

    auto tmp_ini = std::filesystem::temp_directory_path() / "phase2-link.ini";
    if (std::filesystem::exists(tmp_ini)) std::filesystem::remove(tmp_ini);
    auto cfg = cc::config::load(tmp_ini.string());
    cfg.display_name = "Linker";
    cc::config::save(cfg);

    cc::logger::shutdown();
    std::printf("OK: logger + config linked\n");
    std::printf("ALL LINK TESTS PASSED\n");
    return 0;
}
