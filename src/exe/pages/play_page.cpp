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

#include <cstddef>  // std::size
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
    if (runner.snapshot().is_running) {
        // Shouldn't happen — buttons are disabled when busy — but be safe.
        return;
    }

    caster::exe::launcher::LaunchOfflineParams params;
    params.training = training;
    // Launch is async — the worker does CreateProcess + inject + IPC.
    // The InGame UI shows a "Launching..." spinner while
    // snapshot().launch_in_progress is true.
    runner.launch_offline_async(cfg, params);
    menu->transition_to(UiState::InGame);
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
}

} // namespace

void draw(const caster::common::config::Config& cfg,
          MainMenu* menu,
          State& state) {
    namespace ut = caster::common::ui_theme;
    const ut::Theme& t = ut::active_theme();

    const bool busy = menu && menu->game_runner().snapshot().is_running;

    // ====================================================================
    // PLAY PAGE LAYOUT — matches the HTML reference
    //
    // ┌─────────────────────────────┬──────────────────────────────┐
    // │ ROOM CODE                   │ LOCAL MODES                  │
    // │ ┌─────────────────────────┐ │  Training            →      │
    // │ │ input                   │ │  Versus              →      │
    // │ └─────────────────────────┘ │  Combo Trial         →      │
    // │                             │  2v2                 →      │
    // │ [ HOST ] [ JOIN ] [ SPECTATE ] │                            │
    // │                             │                              │
    // │ Detected: ...              │                              │
    // └─────────────────────────────┴──────────────────────────────┘
    // ====================================================================

    // Use the full content area. We'll draw two columns separated by a
    // vertical line, each ~50% of the available width.

    const float avail_w = ImGui::GetContentRegionAvail().x;
    const float avail_h = ImGui::GetContentRegionAvail().y;
    const float col_gap = 40.0f;  // matches HTML .play-col--right padding-left
    const float col_w = (avail_w - col_gap) * 0.5f;

    // ---- LEFT COLUMN: Netplay ----------------------------------------
    {
        ImGui::BeginGroup();
        ImGui::PushID("play_left");

        ut::fieldLabel("ROOM CODE");

        // Input field — takes full column width.
        ImGui::PushItemWidth(col_w);
        ImGui::InputText("##netplay_input", state.input_buf,
                         sizeof(state.input_buf));
        ImGui::PopItemWidth();

        // Show parsed type as muted hint.
        auto parsed = cd::parse_input(state.input_buf);
        ImGui::Spacing();
        ImGui::TextDisabled("Detected: %s", cd::type_label(parsed.type));

        ImGui::Spacing();
        ImGui::Spacing();

        // 3 buttons in a row: Host / Join / Spectate.
        const float btn_gap = 8.0f;
        const float btn_w = (col_w - 2 * btn_gap) / 3.0f;
        const float btn_h = 38.0f;
        ImGui::BeginDisabled(busy);
        if (ut::actionButton("HOST", btn_w, btn_h, ut::ButtonVariant::Default)) {
            do_host(menu, state, parsed, cfg);
        }
        ImGui::SameLine(0, btn_gap);
        if (ut::actionButton("JOIN", btn_w, btn_h, ut::ButtonVariant::Default)) {
            do_join(menu, state, parsed, cfg);
        }
        ImGui::SameLine(0, btn_gap);
        if (ut::actionButton("SPECTATE", btn_w, btn_h, ut::ButtonVariant::Default)) {
            do_spectate(menu, state, parsed, cfg);
        }
        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::Spacing();

        // Inline message (red for error, muted for info).
        if (!state.message.empty()) {
            if (state.is_error) {
                ut::drawErrorText("%s", state.message.c_str());
            } else {
                ut::pushStyleColor(ImGuiCol_Text, t.text_muted);
                ImGui::PushTextWrapPos(col_w);
                ImGui::TextUnformatted(state.message.c_str());
                ImGui::PopTextWrapPos();
                ut::popStyleColor();
            }
        }

        // Quick reference (push to bottom of left column).
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Spacing();
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

        ImGui::PopID();
        ImGui::EndGroup();
    }

    // ---- Vertical separator ------------------------------------------
    {
        const ImVec2 cursor = ImGui::GetCursorPos();
        const ImVec2 root_pos = ImGui::GetWindowPos();
        const float sep_x = root_pos.x + col_w + col_gap * 0.5f;
        const float sep_y0 = root_pos.y + cursor.y - avail_h;
        const float sep_y1 = root_pos.y + cursor.y;
        ut::verticalSeparator(sep_x, sep_y0, sep_y1);
    }

    // ---- RIGHT COLUMN: Local Modes -----------------------------------
    {
        ImGui::SameLine(0, col_gap);
        ImGui::BeginGroup();
        ImGui::PushID("play_right");

        ut::fieldLabel("LOCAL MODES");

        // Behavior depends on the active theme:
        //   Default / Modern — proper buttons (matching Host/Join/Spectate)
        //   Elegant — list-style items (transparent bg, hover arrow, bottom rule)
        //
        // The Elegant theme's flat aesthetic benefits from the minimal list
        // look; the other themes look better with solid bordered buttons.

        struct Mode { const char* label; bool enabled; };
        const Mode modes[] = {
            { "Training",    true  },
            { "Versus",      true  },
            { "Combo Trial", false },  // stub
            { "2v2",         false },  // stub
        };

        const bool list_style = (t.id == ut::ThemeId::Elegant);

        if (list_style) {
            // ---- Elegant: list-style items (original behavior) ----
            const float item_h = 44.0f;
            const float item_w = col_w;
            ImDrawList* dl = ImGui::GetWindowDrawList();

            for (size_t i = 0; i < std::size(modes); ++i) {
                ImGui::PushID(static_cast<int>(i));

                const Mode& m = modes[i];

                // Push button style: transparent bg, hover shows arrow.
                ut::pushStyleColor(ImGuiCol_Text, m.enabled ? t.text : t.text_dim);
                ut::pushStyleColor(ImGuiCol_Button,        ut::COL_TRANSPARENT);
                ut::pushStyleColor(ImGuiCol_ButtonHovered, ut::COL_TRANSPARENT);
                ut::pushStyleColor(ImGuiCol_ButtonActive,  ut::COL_TRANSPARENT);
                ut::pushStyleColor(ImGuiCol_Border,        ut::COL_TRANSPARENT);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 12));

                bool clicked = ImGui::Button(m.label, ImVec2(item_w, item_h));

                ImGui::PopStyleVar(3);
                ImGui::PopStyleColor(5);

                // Draw the bottom rule (1px line).
                {
                    ImVec2 rmin = ImGui::GetItemRectMin();
                    ImVec2 rmax = ImGui::GetItemRectMax();
                    dl->AddLine(ImVec2(rmin.x, rmax.y - 1),
                                ImVec2(rmax.x, rmax.y - 1),
                                ImGui::ColorConvertFloat4ToU32(
                                    ImVec4(t.rule.x, t.rule.y, t.rule.z, t.rule.w)),
                                1.0f);

                    // Draw the right-arrow on hover (only if enabled).
                    if (m.enabled && ImGui::IsItemHovered()) {
                        const float arrow_x = rmax.x - 14.0f;
                        const float arrow_y = (rmin.y + rmax.y) * 0.5f;
                        const ImU32 arrow_col = ImGui::ColorConvertFloat4ToU32(
                            ImVec4(t.accent.x, t.accent.y, t.accent.z, t.accent.w));
                        dl->AddLine(ImVec2(arrow_x - 4, arrow_y - 4),
                                    ImVec2(arrow_x, arrow_y),
                                    arrow_col, 1.5f);
                        dl->AddLine(ImVec2(arrow_x, arrow_y),
                                    ImVec2(arrow_x - 4, arrow_y + 4),
                                    arrow_col, 1.5f);
                    }

                    if (!m.enabled && ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Coming soon");
                    }
                }

                if (clicked && m.enabled) {
                    switch (i) {
                        case 0: do_launch_offline(menu, cfg, /*training=*/true);  break;
                        case 1: do_launch_offline(menu, cfg, /*training=*/false); break;
                    }
                }

                ImGui::PopID();
            }
        } else {
            // ---- Default / Modern: proper buttons (matching Host/Join/Spectate) ----
            const float btn_h = 38.0f;
            const float btn_gap = 8.0f;
            ImGui::BeginDisabled(busy);
            for (size_t i = 0; i < std::size(modes); ++i) {
                ImGui::PushID(static_cast<int>(i));
                const Mode& m = modes[i];

                if (!m.enabled) {
                    // Disabled stub — render as a dimmed button that shows
                    // "Coming soon" on hover. We can't easily disable a single
                    // button inside a BeginDisabled group without affecting
                    // siblings, so we draw it manually with dimmed colors.
                    ut::pushStyleColor(ImGuiCol_Text, t.text_dim);
                    ut::pushStyleColor(ImGuiCol_Button,        t.bg_elevated);
                    ut::pushStyleColor(ImGuiCol_ButtonHovered, t.bg_elevated);
                    ut::pushStyleColor(ImGuiCol_ButtonActive,  t.bg_elevated);
                    ut::pushStyleColor(ImGuiCol_Border,        t.rule);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, t.btn_radius);
                    ImGui::Button(m.label, ImVec2(col_w, btn_h));
                    ImGui::PopStyleVar(2);
                    ImGui::PopStyleColor(5);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Coming soon");
                    }
                } else {
                    if (ut::actionButton(m.label, col_w, btn_h,
                                         ut::ButtonVariant::Default)) {
                        switch (i) {
                            case 0: do_launch_offline(menu, cfg, /*training=*/true);  break;
                            case 1: do_launch_offline(menu, cfg, /*training=*/false); break;
                        }
                    }
                }
                if (i + 1 < std::size(modes)) ImGui::Spacing();
                ImGui::PopID();
            }
            ImGui::EndDisabled();
        }

        // If a game is currently running, show a warning at the bottom.
        if (busy) {
            ImGui::Spacing();
            ImGui::Spacing();
            ut::drawWarnText("Game already running (PID %u) — use Force Kill first.",
                             menu ? menu->game_runner().snapshot().pid : 0u);
        }

        ImGui::PopID();
        ImGui::EndGroup();
    }
}

} // namespace caster::exe::pages::play_page
