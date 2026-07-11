// src/exe/pages/play_page.cpp

#include "play_page.hpp"
#include "main_menu.hpp"
#include "../launcher/game_runner.hpp"
#include "../ui_state.hpp"
#include "../../common/config.hpp"
#include "../../common/logger.hpp"
#include "../../common/net/connection_detector.hpp"
#include "../../common/ui_theme.hpp"

#include <imgui.h>

#include <cstdio>
#include <string>

namespace caster::exe::pages::play_page {

namespace {

namespace ut = caster::common::ui_theme;
namespace cd = caster::common::net::connection_detector;

// Set the inline message (clears is_error if msg is info).
void set_message(State& s, const std::string& msg, bool is_error) {
    s.message  = msg;
    s.is_error = is_error;
}

// Launch the game in offline mode (Training or Versus).
void do_launch_offline(MainMenu* menu,
                       const caster::common::config::Config& cfg,
                       bool training) {
    if (!menu) return;

    auto& runner = menu->game_runner();
    if (runner.is_running()) {
        // Shouldn't happen — buttons are disabled when busy — but be safe.
        return;
    }

    caster::exe::launcher::LaunchOfflineParams params;
    params.training = training;
    auto r = runner.launch_offline(cfg, params);
    if (r.success) {
        menu->transition_to(UiState::InGame);
    } else {
        caster::common::logger::err("play_page: launch failed: {}",
                                    r.error_message);
        menu->set_error(r.error_message);
    }
}

// Netplay start helpers.
// NetplaySession::start_smart_host / start_smart_join / etc.
void do_host(MainMenu* menu, State& state, const cd::ParseResult& parsed,
              const caster::common::config::Config& cfg) {
    using namespace caster::common;
    if (!menu) return;
    menu->start_session();
    auto* s = menu->session();
    if (!s) return;

    // Set local name + detect connection type before starting.
    s->set_local_name_async(cfg.display_name);
    s->detect_connection_type_async();

    std::string relay_source;
    for (const auto& r : cfg.relay_servers) {
        if (!relay_source.empty()) relay_source += '\n';
        relay_source += r;
    }

    switch (parsed.type) {
        case cd::InputType::Empty:
            logger::info("play_page: Host (smart, random port)");
            s->start_smart_host_async(relay_source,
                                      caster::common::config::kDefaultPort, false);
            break;
        case cd::InputType::Port:
            logger::info("play_page: Host (smart, port={})", parsed.port);
            s->start_smart_host_async(relay_source,
                                      static_cast<std::uint16_t>(parsed.port), false);
            break;
        default:
            set_message(state,
                        "Host needs a port number (or empty for random)",
                        /*is_error=*/true);
            menu->end_session();
            return;
    }
    // Lookup public/local IP in background (best-effort).
    s->lookup_host_addresses_async();
    menu->transition_to(UiState::WaitingForPeer);
}

void do_join(MainMenu* menu, State& state, const cd::ParseResult& parsed,
             const caster::common::config::Config& cfg) {
    using namespace caster::common;
    if (!menu) return;
    menu->start_session();
    auto* s = menu->session();
    if (!s) return;

    s->set_local_name_async(cfg.display_name);
    s->detect_connection_type_async();

    std::string relay_source;
    for (const auto& r : cfg.relay_servers) {
        if (!relay_source.empty()) relay_source += '\n';
        relay_source += r;
    }

    switch (parsed.type) {
        case cd::InputType::RoomCode:
            logger::info("play_page: Join (relay, room={})", parsed.room_code);
            s->start_relay_join_async(relay_source, parsed.room_code, false);
            break;
        case cd::InputType::IpPort:
            logger::info("play_page: Join (direct, {}:{})",
                         parsed.host, parsed.port);
            s->start_join_async(parsed.host,
                                static_cast<std::uint16_t>(parsed.port), false);
            break;
        case cd::InputType::Port:
            // Convenience: joining on a port = joining localhost:port
            logger::info("play_page: Join (localhost:{})", parsed.port);
            s->start_join_async("127.0.0.1",
                                static_cast<std::uint16_t>(parsed.port), false);
            break;
        default:
            set_message(state, "Join needs host:port or #room",
                        /*is_error=*/true);
            menu->end_session();
            return;
    }
    s->lookup_host_addresses_async();
    menu->transition_to(UiState::WaitingForPeer);
}

void do_spectate(MainMenu* menu, State& state, const cd::ParseResult& parsed,
                 const caster::common::config::Config& cfg) {
    using namespace caster::common;
    (void)cfg;
    if (!menu) return;
    switch (parsed.type) {
        case cd::InputType::IpPort:
            logger::info("play_page: Spectate (direct, {}:{}) — not yet implemented",
                         parsed.host, parsed.port);
            set_message(state,
                        "Not yet implemented: direct spectate " + parsed.host + ":" +
                        std::to_string(parsed.port),
                        /*is_error=*/false);
            break;
        case cd::InputType::RoomCode:
            set_message(state,
                        "Spectate via relay not supported yet",
                        /*is_error=*/true);
            break;
        default:
            set_message(state,
                        "Spectate needs host:port (relay spectate not supported)",
                        /*is_error=*/true);
            return;
    }
    (void)menu;
}

} // namespace

void draw(const caster::common::config::Config& cfg,
          MainMenu* menu,
          State& state) {
    namespace ut = caster::common::ui_theme;

    const bool busy = menu && menu->game_runner().is_running();

    // Two cards side-by-side: NETPLAY (left) and OFFLINE (right).
    const float card_w = 460.0f;
    const float card_h = 480.0f;
    const float gap    = 16.0f;

    // ---- NETPLAY card --------------------------------------------------
    ImGui::SetCursorPosX(0);
    if (ut::beginCard("Netplay", card_w, card_h, false)) {
        ut::cardTitle("NETPLAY");

        ImGui::TextDisabled("Port / IP:Port / #RoomCode");
        ImGui::PushItemWidth(-1);  // fill card width
        ImGui::InputText("##netplay_input", state.input_buf,
                         sizeof(state.input_buf));
        ImGui::PopItemWidth();

        // Show parsed type as muted hint.
        auto parsed = cd::parse_input(state.input_buf);
        ImGui::TextDisabled("Detected: %s", cd::type_label(parsed.type));

        ImGui::Spacing();

        // 3 buttons in a row.
        const float btn_w = (card_w - 2 * ut::CARD_PAD - 2 * 8) / 3;
        ImGui::BeginDisabled(busy);
        if (ut::primaryButton("Host", btn_w, 36)) {
            do_host(menu, state, parsed, cfg);
        }
        ImGui::SameLine(0, 8);
        if (ut::primaryButton("Join", btn_w, 36)) {
            do_join(menu, state, parsed, cfg);
        }
        ImGui::SameLine(0, 8);
        if (ut::primaryButton("Spectate", btn_w, 36)) {
            do_spectate(menu, state, parsed, cfg);
        }
        ImGui::EndDisabled();

        ImGui::Spacing();

        // Inline message (red for error, muted for info).
        if (!state.message.empty()) {
            if (state.is_error) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.55f, 1.0f));
            }
            ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
            ImGui::TextUnformatted(state.message.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Quick reference.
        ImGui::TextDisabled("Quick reference:");
        ImGui::BulletText("Empty  -> Host with random port");
        ImGui::BulletText("46318  -> Host on port 46318");
        ImGui::BulletText("192.168.1.10:46318 -> Direct join");
        ImGui::BulletText("#ABCD  -> Relay join (4 uppercase letters)");

        ImGui::Spacing();
        ImGui::TextDisabled("Default port: %d",
                            caster::common::config::kDefaultPort);
        ImGui::TextDisabled("Display name: %s",
                            cfg.display_name.empty() ? "(not set)"
                                                     : cfg.display_name.c_str());

        ut::endCard();
    }

    // ---- OFFLINE card --------------------------------------------------
    ImGui::SameLine(0, gap);
    if (ut::beginCard("Offline", card_w, card_h, false)) {
        ut::cardTitle("OFFLINE");

        ImGui::BeginDisabled(busy);
        if (ut::primaryButton("Training", 280, 44)) {
            do_launch_offline(menu, cfg, /*training=*/true);
        }
        ImGui::EndDisabled();
        ImGui::Spacing();
        ImGui::TextDisabled("Launches MBAA.exe in training mode "
                            "(hook.dll injected).");

        ImGui::Spacing();
        ImGui::Spacing();

        ImGui::BeginDisabled(busy);
        if (ut::primaryButton("Versus Mode", 280, 44)) {
            do_launch_offline(menu, cfg, /*training=*/false);
        }
        ImGui::EndDisabled();
        ImGui::Spacing();
        ImGui::TextDisabled("Launches MBAA.exe in versus mode "
                            "(hook.dll injected).");

        if (busy) {
            ImGui::Spacing();
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                               "Game already running (PID %u) — use Force Kill first.",
                               menu ? menu->game_runner().pid() : 0u);
        }

        ut::endCard();
    }
}

} // namespace caster::exe::pages::play_page
