// src/common/config.cpp

#include "config.hpp"
#include "ini.hpp"
#include "logger.hpp"

namespace caster::common::config {

namespace {

// These are the public relay servers used for NAT traversal when the
// player can't port-forward. Order matters: tried top-to-bottom.
const std::vector<std::string> kDefaultRelays = {
    "zzcaster.duckdns.org:3939"
    // Placeholder
};

} // namespace

Config load(const std::string& path) {
    Config cfg;
    cfg.source_path = path;

    ini::Document doc;
    if (!ini::loadFile(path, doc)) {
        // Missing file → use defaults (already set in struct).
        logger::info("config: file not found at '{}', using defaults", path);
        return cfg;
    }

    // [paths]
    cfg.game_dir = doc.getString("paths", "game_dir", "");

    // [player]
    cfg.display_name = doc.getString("player", "display_name", "");

    // [match]
    cfg.versus_win_count  = static_cast<int>(
        doc.getInt("match", "versus_win_count", 2));
    cfg.max_real_delay    = static_cast<int>(
        doc.getInt("match", "max_real_delay", 254));
    cfg.high_cpu_priority = doc.getBool("match", "high_cpu_priority", true);

    // [game]
    cfg.stage_animations_off = doc.getBool("game", "stage_animations_off", false);
    cfg.auto_replay_save     = doc.getBool("game", "auto_replay_save", true);
    cfg.dxvk_enabled         = doc.getBool("game", "dxvk_enabled", true);

    // [system]
    cfg.auto_check_updates = doc.getBool("system", "auto_check_updates", true);
    cfg.log_to_stdout      = doc.getBool("system", "log_to_stdout", false);

    // [overlay]
    cfg.playername_enabled         = doc.getBool("overlay", "playername_enabled", true);
    cfg.playername_position_bottom = doc.getBool("overlay", "playername_position_bottom", false);

    // [ui]
    cfg.theme = static_cast<int>(doc.getInt("ui", "theme", 2));  // default: Elegant
    cfg.rounded_corners = doc.getBool("ui", "rounded_corners", false);

    // [network]
    // Relay list is stored as a single multi-line value under [network]/relays.
    std::string relays_str = doc.getString("network", "relays", "");
    if (!relays_str.empty()) {
        std::string current;
        for (char c : relays_str) {
            if (c == '\n' || c == '\r') {
                if (!current.empty()) {
                    cfg.relay_servers.push_back(current);
                    current.clear();
                }
            } else {
                current += c;
            }
        }
        if (!current.empty()) cfg.relay_servers.push_back(current);
    }

    logger::info("config: loaded from '{}'", path);
    return cfg;
}

bool save(Config& cfg, const std::string& path) {
    const std::string target = path.empty() ? cfg.source_path : path;
    if (target.empty()) {
        logger::err("config: cannot save — no source_path set");
        return false;
    }
    cfg.source_path = target;

    ini::Document doc;

    // [paths]
    doc.set("paths", "game_dir", cfg.game_dir);

    // [player]
    doc.set("player", "display_name", cfg.display_name);

    // [match]
    doc.set("match", "versus_win_count",  std::to_string(cfg.versus_win_count));
    doc.set("match", "max_real_delay",    std::to_string(cfg.max_real_delay));
    doc.set("match", "high_cpu_priority", cfg.high_cpu_priority ? "true" : "false");

    // [game]
    doc.set("game", "stage_animations_off", cfg.stage_animations_off ? "true" : "false");
    doc.set("game", "auto_replay_save",     cfg.auto_replay_save     ? "true" : "false");
    doc.set("game", "dxvk_enabled",         cfg.dxvk_enabled         ? "true" : "false");

    // [system]
    doc.set("system", "auto_check_updates", cfg.auto_check_updates ? "true" : "false");
    doc.set("system", "log_to_stdout",      cfg.log_to_stdout      ? "true" : "false");

    // [overlay]
    doc.set("overlay", "playername_enabled",         cfg.playername_enabled         ? "true" : "false");
    doc.set("overlay", "playername_position_bottom", cfg.playername_position_bottom ? "true" : "false");

    // [ui]
    doc.set("ui", "theme", std::to_string(cfg.theme));
    doc.set("ui", "rounded_corners", cfg.rounded_corners ? "true" : "false");

    // [network]
    doc.set("network", "relays", relayListAsString(cfg.relay_servers));

    if (!ini::writeFile(target, doc)) {
        logger::err("config: failed to write '{}'", target);
        return false;
    }
    logger::info("config: saved to '{}'", target);
    return true;
}

std::vector<std::string> defaultRelayList() {
    return kDefaultRelays;
}

std::string relayListAsString(const std::vector<std::string>& relays) {
    std::string out;
    for (size_t i = 0; i < relays.size(); ++i) {
        if (i) out += '\n';
        out += relays[i];
    }
    return out;
}

} // namespace caster::common::config
