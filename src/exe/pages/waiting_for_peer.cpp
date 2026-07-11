// src/exe/pages/waiting_for_peer.cpp

#include "waiting_for_peer.hpp"
#include "../../common/logger.hpp"
#include "../../common/ui_theme.hpp"
#include "../../common/net/relay/relay_client.hpp"

#include <imgui.h>

#include <string>

namespace caster::exe::pages::waiting_for_peer {

namespace {

namespace ut = caster::common::ui_theme;
namespace ss = caster::exe::session;
namespace rclient = caster::common::net::relay_client;

void draw_info_row(const char* label, const std::string& value) {
    ImGui::BulletText("%s: %s", label, value.c_str());
}

// Draw the relay phase status with a phase-appropriate color.
void draw_relay_phase(ss::NetplaySession& session) {
    const auto& status = session.status_message();
    if (status.empty()) return;

    if (status.find("Hole-punching") != std::string::npos) {
        ut::pushStyleColor(ImGuiCol_Text, ut::COL_WARN);
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(status.c_str());
        ImGui::PopTextWrapPos();
        ut::popStyleColor();
    } else if (status.find("Retrying") != std::string::npos) {
        ut::pushStyleColor(ImGuiCol_Text, ut::COL_INFO);
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(status.c_str());
        ImGui::PopTextWrapPos();
        ut::popStyleColor();
    } else if (status.find("failed") != std::string::npos ||
               status.find("error") != std::string::npos ||
               status.find("Error") != std::string::npos) {
        ut::pushStyleColor(ImGuiCol_Text, ut::COL_ERROR);
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(status.c_str());
        ImGui::PopTextWrapPos();
        ut::popStyleColor();
    } else {
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextDisabled("%s", status.c_str());
        ImGui::PopTextWrapPos();
    }
}

// Draw room validation failure details (when start_relay_join rejected the code).
void draw_room_validation_error(ss::NetplaySession& session) {
    auto rv = session.room_validation();
    if (!rv) return;

    const char* label = rclient::room_validation_label(*rv);
    const char* suggestion = rclient::room_validation_suggestion(*rv);

    ImGui::Spacing();
    ut::pushStyleColor(ImGuiCol_Text, ut::COL_ERROR);
    ut::cardTitle("ROOM CODE ERROR");
    ut::popStyleColor();

    ImGui::TextUnformatted(label);
    ImGui::Spacing();

    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
    ImGui::TextDisabled("%s", suggestion);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
}

} // namespace

DrawResult draw(ss::NetplaySession& session) {
    DrawResult r;

    // Drive the state machine.
    session.step();

    // Check terminal states.
    if (session.state() == ss::SessionState::Launching) {
        r.launching = true;
        return r;
    }
    if (session.state() == ss::SessionState::Failed) {
        r.error_message = session.error_message();
        return r;
    }
    if (session.state() == ss::SessionState::Cancelled) {
        r.cancelled = true;
        return r;
    }

    // ---- Render the centered card ----------------------------------------
    constexpr float card_w = 640.0f;
    constexpr float card_h = 460.0f;
    if (ut::beginCenteredCard("##waiting", card_w, card_h, false)) {
        // Title depends on role + state.
        std::string title;
        if (session.config().is_host) {
            title = "HOSTING — WAITING FOR OPPONENT";
        } else {
            title = "CONNECTING TO HOST";
        }
        ut::cardTitle(title.c_str());

        // ---- Phase / status display ---------------------------------------
        draw_relay_phase(session);

        // ---- Room validation error (if start_relay_join rejected) -------
        draw_room_validation_error(session);

        // Countdown.
        auto remaining = session.remaining_seconds();
        if (remaining) {
            ImGui::TextDisabled("Timeout in: %us", *remaining);
        }

        ImGui::Separator();

        // ---- Info to share with opponent (host: room code / IP) ----------
        if (session.config().is_host) {
            auto code = session.room_code();
            if (code) {
                ImGui::TextDisabled("Room code (share with opponent):");
                ImGui::SetWindowFontScale(1.8f);
                ImGui::TextColored(ut::COL_RED, "#%s", code->c_str());
                ImGui::SetWindowFontScale(1.0f);
                ImGui::SameLine();
                if (ut::secondaryButton("Copy", 80, 24)) {
                    ImGui::SetClipboardText(("#" + *code).c_str());
                }
            } else {
                // Direct host: show IP:port.
                auto pub = session.public_ip();
                if (pub) {
                    ImGui::TextDisabled("Share with opponent:");
                    ImGui::Text("%s:%d", pub->c_str(),
                                session.config().peer_port);
                    ImGui::SameLine();
                    if (ut::secondaryButton("Copy", 80, 24)) {
                        std::string s = *pub + ":" +
                            std::to_string(session.config().peer_port);
                        ImGui::SetClipboardText(s.c_str());
                    }
                }
            }
            if (auto loc = session.local_ip()) {
                ImGui::TextDisabled("Local IP: %s (use this on LAN)", loc->c_str());
            }
        } else {
            // Client: show what we're connecting to.
            if (!session.config().peer_addr.empty()) {
                draw_info_row("Connecting to",
                              session.config().peer_addr + ":" +
                              std::to_string(session.config().peer_port));
            }
            if (auto code = session.room_code()) {
                draw_info_row("Room code", "#" + *code);
            }
        }

        ImGui::Separator();

        // ---- Connection type warning (Wi-Fi) ------------------------------
        const auto& ct = session.local_connection_type();
        if (ct == "Wireless") {
            ut::drawErrorText("Wi-fi detected. A wired connection is recommended "
                               "for lowest latency.");
        } else if (!ct.empty()) {
            ImGui::TextDisabled("Connection: %s", ct.c_str());
        }

        // ---- Ping stats (visible during WaitingConfirmation) -------------
        if (session.state() == ss::SessionState::WaitingConfirmation) {
            ImGui::Separator();
            ut::cardTitle("HANDSHAKE COMPLETE");
            if (!session.remote_name().empty()) {
                draw_info_row("Opponent", session.remote_name());
            }
            if (!session.remote_connection_type().empty()) {
                draw_info_row("Opponent conn", session.remote_connection_type());
            }
            const auto& stats = session.stats();
            ImGui::BulletText("Ping (avg/min/max): %.0f / %.0f / %.0f ms",
                              stats.avg_ms, stats.min_ms, stats.max_ms);
            ImGui::BulletText("Packet loss: %u%%", stats.packet_loss);
            ImGui::BulletText("Auto input delay: %d frames",
                              session.config().delay);

            ImGui::Spacing();
            if (ut::primaryButton("Start Match", 200, 40)) {
                session.host_confirm();
            }
        }

        ImGui::Separator();

        // ---- Cancel button (always available) ----------------------------
        if (ut::secondaryButton("Cancel", 120, 32)) {
            session.cancel();
            r.cancelled = true;
        }

        ut::endCard();
    }

    return r;
}

} // namespace caster::exe::pages::waiting_for_peer
