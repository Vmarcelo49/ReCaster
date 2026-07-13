// src/exe/pages/waiting_for_peer.cpp
//
// WaitingForPeer UI — reads session state via snapshot() (no step() call).
// All actions (host_confirm, cancel) are enqueued as async commands.
//
// Visual layout (matches HTML .host-page):
//   - Centered card with vertical stack of:
//     * "AWAITING OPPONENT" kicker (small uppercase dim)
//     * Spinner (animated vertical bar in accent color)
//     * Room code with copy button (when hosting via relay)
//     * Connection window countdown
//     * Action buttons (Start Training / Cancel)

#include "waiting_for_peer.hpp"
#include "../../common/logger.hpp"
#include "../../common/ui_theme.hpp"
#include "../../common/net/relay/relay_client.hpp"

#include <imgui.h>

#include <algorithm>  // std::min
#include <cmath>    // std::fmod, std::sqrt
#include <cstdio>   // std::snprintf
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
void draw_relay_phase(const std::string& status) {
    if (status.empty()) return;
    const ut::Theme& t = ut::active_theme();

    if (status.find("Hole-punching") != std::string::npos) {
        ut::pushStyleColor(ImGuiCol_Text, t.warn);
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(status.c_str());
        ImGui::PopTextWrapPos();
        ut::popStyleColor();
    } else if (status.find("Retrying") != std::string::npos) {
        ut::pushStyleColor(ImGuiCol_Text, t.info);
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(status.c_str());
        ImGui::PopTextWrapPos();
        ut::popStyleColor();
    } else if (status.find("failed") != std::string::npos ||
               status.find("error") != std::string::npos ||
               status.find("Error") != std::string::npos) {
        ut::pushStyleColor(ImGuiCol_Text, t.error);
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
void draw_room_validation_error(const std::optional<rclient::RoomValidationResult>& rv) {
    if (!rv) return;
    const ut::Theme& t = ut::active_theme();

    const char* label = rclient::room_validation_label(*rv);
    const char* suggestion = rclient::room_validation_suggestion(*rv);

    ImGui::Spacing();
    ut::pushStyleColor(ImGuiCol_Text, t.error);
    ut::cardTitle("ROOM CODE ERROR");
    ut::popStyleColor();

    ImGui::TextUnformatted(label);
    ImGui::Spacing();

    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
    ImGui::TextDisabled("%s", suggestion);
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
}

// Draw an animated spinner — a circle with a "wiper" that sweeps horizontally.
// Inspired by the CSS loader:
//   .loader { width: 144px; height: 144px; border-radius: 50%; }
//   @keyframes animloader {
//     0%   { box-shadow: -216px 0 #fff inset; }
//     100% { box-shadow:  144px 0 #fff inset; }
//   }
//
// The inset box-shadow creates a half-moon shape that sweeps across the
// circle. Since ImGui has no circular clipping, we simulate it by drawing
// thin vertical slices: for each column x inside the circle, we compute
// the visible height (intersection of the circle and the shadow) and
// draw a 1px-wide line.
//
// Color is the active theme's accent (red / blue / bronze-gold).
void draw_spinner(float center_x, float center_y) {
    const ut::Theme& t = ut::active_theme();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const ImU32 accent_col = ImGui::ColorConvertFloat4ToU32(
        ImVec4(t.accent.x, t.accent.y, t.accent.z, t.accent.w));
    const ImU32 border_col = ImGui::ColorConvertFloat4ToU32(
        ImVec4(t.rule_strong.x, t.rule_strong.y, t.rule_strong.z, t.rule_strong.w));

    // Geometry — matches the CSS proportions.
    // CSS: size = 48 * 3px = 144px → radius R = 72. We scale down to R = 24
    // (48px diameter) to fit nicely in the card.
    constexpr float kR = 24.0f;
    // Slower than the CSS (1s) so the wiper is visible longer and the
    // wrap-around feels more continuous.
    constexpr float kPeriod = 1.8f;

    // Extended offset range: -2R to +2R (symmetric). At the endpoints the
    // shadow circle is fully outside the element, so the circle appears
    // empty for a brief moment before the wiper re-enters from the
    // opposite side — giving the illusion that it "comes back around".
    constexpr float kOffsetStart = -2.0f * kR;
    constexpr float kOffsetEnd   =  2.0f * kR;

    const float time = static_cast<float>(ImGui::GetTime());
    const float phase = std::fmod(time, kPeriod) / kPeriod;  // 0..1
    const float offset = kOffsetStart + phase * (kOffsetEnd - kOffsetStart);

    // Draw the circle outline (the "container").
    dl->AddCircle(ImVec2(center_x, center_y), kR, border_col, 0, 2.0f);

    // Draw the filled "wiper" by iterating vertical slices.
    // For each column x within the circle, the visible height is the
    // intersection of:
    //   - the element circle (center=center_x, radius=kR)
    //   - the shadow circle  (center=center_x+offset, radius=kR)
    // Both circles have the same radius, so the intersection at column x
    // is min(h_elem, h_shadow) where h = sqrt(R² - x²).
    for (float x = -kR; x <= kR; x += 1.0f) {
        const float sx = x - offset;  // x relative to shadow center
        const float sx2 = sx * sx;
        if (sx2 >= kR * kR) continue;  // this column is outside the shadow

        const float h_elem    = std::sqrt(kR * kR - x * x);
        const float h_shadow  = std::sqrt(kR * kR - sx2);
        const float h = std::min(h_elem, h_shadow);  // intersection height

        dl->AddLine(
            ImVec2(center_x + x, center_y - h),
            ImVec2(center_x + x, center_y + h),
            accent_col, 1.0f);
    }
}

} // namespace

DrawResult draw(ss::NetplaySession& session) {
    DrawResult r;
    const ut::Theme& t = ut::active_theme();

    // Read a consistent snapshot — no step() call, the worker thread is
    // driving the state machine in the background.
    auto snap = session.snapshot();

    // Check terminal states.
    if (snap.state == ss::SessionState::Launching) {
        r.launching = true;
        return r;
    }
    if (snap.state == ss::SessionState::Failed) {
        r.error_message = snap.error_message;
        return r;
    }
    if (snap.state == ss::SessionState::Cancelled) {
        r.cancelled = true;
        return r;
    }

    // ---- Centered card (host-page layout) -----------------------------
    constexpr float card_w = 520.0f;
    constexpr float card_h = 500.0f;
    if (ut::beginCenteredCard("##waiting", card_w, card_h, false)) {
        const float inner_w = card_w - 2 * ut::CARD_PAD;

        // ---- Kicker: "AWAITING OPPONENT" (host) or "CONNECTING" (client) -
        {
            const char* kicker = snap.config.is_host
                ? "AWAITING OPPONENT"
                : "CONNECTING TO HOST";
            ut::fieldLabel(kicker);
        }

        // ---- Animated spinner (centered) ---------------------------------
        {
            const ImVec2 cursor = ImGui::GetCursorPos();
            ImGui::Dummy(ImVec2(inner_w, 56.0f));
            const ImVec2 item_min = ImGui::GetItemRectMin();
            const ImVec2 item_max = ImGui::GetItemRectMax();
            const float cx = (item_min.x + item_max.x) * 0.5f;
            const float cy = (item_min.y + item_max.y) * 0.5f;
            draw_spinner(cx, cy);
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // ---- Room code section (host via relay) -------------------------
        // Layout:
        //   ROOM CODE              ← title (muted, small uppercase)
        //          #ABCD [COPY]     ← code centered, button right next to it
        if (snap.config.is_host && snap.room_code) {
            // Title "Room Code"
            ut::fieldLabel("ROOM CODE");

            // Center just the "#XXXX" text, then place COPY right after it.
            // Uses font_body_lg() — a separate font instance loaded at 38px
            // (2× body) so the text is crisp, not pixelated.
            if (t.font_body_lg()) ImGui::PushFont(t.font_body_lg());
            ut::pushStyleColor(ImGuiCol_Text, t.text);
            const std::string code_label = "#" + *snap.room_code;
            const ImVec2 code_size = ImGui::CalcTextSize(code_label.c_str());
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner_w - code_size.x) * 0.5f);
            ImGui::TextUnformatted(code_label.c_str());
            ut::popStyleColor();
            if (t.font_body_lg()) ImGui::PopFont();

            // COPY button immediately to the right of the code (not centered).
            ImGui::SameLine(0, 8);
            if (ut::actionButton("COPY", 60.0f, 24.0f, ut::ButtonVariant::Default)) {
                ImGui::SetClipboardText(("#" + *snap.room_code).c_str());
            }
        } else if (!snap.config.is_host) {
            // Client: show what we're connecting to.
            if (!snap.config.peer_addr.empty()) {
                draw_info_row("Connecting to",
                              snap.config.peer_addr + ":" +
                              std::to_string(snap.config.peer_port));
            }
            if (snap.room_code) {
                draw_info_row("Room code", "#" + *snap.room_code);
            }
        }

        ImGui::Spacing();

        // ---- Timeout timer -----------------------------------------------
        if (snap.remaining_seconds) {
            const unsigned int total = *snap.remaining_seconds;
            const unsigned int min = total / 60;
            const unsigned int sec = total % 60;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%umin %02us", min, sec);

            ut::pushStyleColor(ImGuiCol_Text, t.text_dim);
            if (t.font_body_sm()) ImGui::PushFont(t.font_body_sm());
            const char* label = "TIMEOUT IN";
            const ImVec2 label_size = ImGui::CalcTextSize(label);
            const ImVec2 value_size = ImGui::CalcTextSize(buf);
            const float gap = 8.0f;
            const float total_w = label_size.x + gap + value_size.x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner_w - total_w) * 0.5f);
            ImGui::TextUnformatted(label);
            ImGui::SameLine(0, gap);
            ImGui::PopFont();
            if (t.font_mono()) ImGui::PushFont(t.font_mono());
            ut::pushStyleColor(ImGuiCol_Text, t.text);
            ImGui::TextUnformatted(buf);
            ut::popStyleColor();
            if (t.font_mono()) ImGui::PopFont();
            ut::popStyleColor();
        }

        ImGui::Spacing();

        // ---- Room validation error (if any) ------------------------------
        draw_room_validation_error(snap.room_validation);

        // ---- Connection type info ----------------------------------------
        const auto& ct = snap.config.local_connection_type;
        if (ct == "Wireless") {
            ut::drawErrorText("Wi-fi detected. A wired connection is recommended "
                               "for lowest latency.");
        } else if (!ct.empty()) {
            ut::pushStyleColor(ImGuiCol_Text, t.text_dim);
            if (t.font_body_sm()) ImGui::PushFont(t.font_body_sm());
            const std::string conn_label = "Connection: " + ct;
            const ImVec2 conn_size = ImGui::CalcTextSize(conn_label.c_str());
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner_w - conn_size.x) * 0.5f);
            ImGui::TextUnformatted(conn_label.c_str());
            if (t.font_body_sm()) ImGui::PopFont();
            ut::popStyleColor();
        }

        // ---- Handshake info (visible during WaitingConfirmation) --------
        if (snap.state == ss::SessionState::WaitingConfirmation) {
            // Static buffers for the editable Delay / Rollback fields.
            // Initialized once when entering WaitingConfirmation, then the
            // user can override before clicking START MATCH.
            static char delay_buf[8]    = {0};
            static char rollback_buf[8] = {0};
            static bool handshake_init = false;

            // Initialize from the suggested values when we first enter
            // the handshake state. Reset when we leave it.
            if (!handshake_init) {
                std::snprintf(delay_buf, sizeof(delay_buf), "%d",
                              static_cast<int>(snap.config.delay));
                std::snprintf(rollback_buf, sizeof(rollback_buf), "%d",
                              static_cast<int>(snap.config.rollback));
                handshake_init = true;
            }

            if (!snap.config.remote_name.empty()) {
                draw_info_row("User Name", snap.config.remote_name);
            }
            if (!snap.config.remote_connection_type.empty()) {
                draw_info_row("Opponent connection", snap.config.remote_connection_type);
            }
            ImGui::BulletText("Ping: %.0f ms", snap.stats.avg_ms);
            ImGui::BulletText("Packet loss: %u%%", snap.stats.packet_loss);
            ImGui::BulletText("Suggested input delay: %d frames",
                              snap.config.delay);
            ImGui::BulletText("Suggested rollback: %d frames",
                              static_cast<int>(snap.config.rollback));

            // ---- Editable Delay / Rollback fields -------------------------
            // These are the actual values sent to the game when the match
            // starts. The user can override the suggested values.
            ImGui::Spacing();
            const float field_w = 60.0f;

            // Delay: [input]
            ImGui::AlignTextToFramePadding();
            ut::pushStyleColor(ImGuiCol_Text, t.text_muted);
            if (t.font_body_sm()) ImGui::PushFont(t.font_body_sm());
            ImGui::TextUnformatted("DELAY");
            if (t.font_body_sm()) ImGui::PopFont();
            ut::popStyleColor();
            ImGui::SameLine(0, 8);
            ImGui::PushItemWidth(field_w);
            ImGui::InputText("##delay_edit", delay_buf, sizeof(delay_buf),
                             ImGuiInputTextFlags_CharsDecimal);
            ImGui::PopItemWidth();

            // Rollback: [input]  (same line)
            ImGui::SameLine(0, 24);
            ImGui::AlignTextToFramePadding();
            ut::pushStyleColor(ImGuiCol_Text, t.text_muted);
            if (t.font_body_sm()) ImGui::PushFont(t.font_body_sm());
            ImGui::TextUnformatted("ROLLBACK");
            if (t.font_body_sm()) ImGui::PopFont();
            ut::popStyleColor();
            ImGui::SameLine(0, 8);
            ImGui::PushItemWidth(field_w);
            ImGui::InputText("##rollback_edit", rollback_buf, sizeof(rollback_buf),
                             ImGuiInputTextFlags_CharsDecimal);
            ImGui::PopItemWidth();

            ImGui::Spacing();
            const float start_match_w = 200.0f;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner_w - start_match_w) * 0.5f);
            if (ut::primaryButton("START MATCH", start_match_w, 40.0f)) {
                // Parse the user's input and apply before confirming.
                try {
                    int d = std::stoi(delay_buf);
                    int rb = std::stoi(rollback_buf);
                    if (d >= 0 && d <= 254) {
                        session.set_manual_delay_async(
                            static_cast<std::uint8_t>(d));
                    }
                    if (rb >= 0 && rb <= 20) {
                        session.set_rollback_async(
                            static_cast<std::uint8_t>(rb));
                    }
                } catch (...) {
                    // If parse fails, fall back to the suggested values.
                }
                session.host_confirm_async();
                handshake_init = false;  // reset for next time
            }
        }

        // ---- Action row: Launch Training + Cancel (centered) ------------
        const float action_gap = 10.0f;
        const float training_w = 160.0f;
        const float cancel_w   = 120.0f;
        const bool show_training = snap.config.is_host &&
            snap.state != ss::SessionState::WaitingConfirmation;
        const float action_total = (show_training ? training_w + action_gap : 0.0f) + cancel_w;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner_w - action_total) * 0.5f);

        if (show_training) {
            if (ut::actionButton("START TRAINING", training_w, 32.0f,
                                 ut::ButtonVariant::Default)) {
                r.launch_training = true;
                return r;
            }
            ImGui::SameLine(0, action_gap);
        }

        if (ut::actionButton("CANCEL", cancel_w, 32.0f, ut::ButtonVariant::Default)) {
            session.cancel_async();
            r.cancelled = true;
        }

        // ---- Footer: status message -------------------------------------
        // "Listening for direct connection" etc. — shown as a muted footer
        // at the bottom of the card.
        if (!snap.status_message.empty()) {
            ImGui::Spacing();
            ImGui::Spacing();
            ut::pushStyleColor(ImGuiCol_Text, t.text_dim);
            if (t.font_body_sm()) ImGui::PushFont(t.font_body_sm());
            const ImVec2 status_size = ImGui::CalcTextSize(snap.status_message.c_str());
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner_w - status_size.x) * 0.5f);
            ImGui::TextUnformatted(snap.status_message.c_str());
            if (t.font_body_sm()) ImGui::PopFont();
            ut::popStyleColor();
        }

        ut::endCard();
    }

    return r;
}

} // namespace caster::exe::pages::waiting_for_peer
