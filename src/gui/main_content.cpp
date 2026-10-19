// MainContentRenderer - Control panel, three-panel layout, right panel, bottom buttons

#include "skywave_link/main_content.hpp"
#include "skywave_link/gui_util.hpp"
#include "skywave_link/modem_client.hpp"
#include "skywave_link/modern_style.hpp"
#include "skywave_link/radio_controller.hpp"
#include "imgui.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace skylink {

namespace {

// The VOICE button: shows the voice state and opens / closes a session.
// Returns true when clicked.
bool renderVoiceButton(const VoiceButtonView& view) {
    const ImVec4 base_button = ImGui::GetStyleColorVec4(ImGuiCol_Button);
    const bool flash_on = (static_cast<int>(ImGui::GetTime() * 4.0) % 2) == 0;
    ImVec4 button_color = base_button;
    switch (view.kind) {
        case VoiceIndicator::Tx:
            if (flash_on) button_color = ui::palette().danger;
            break;
        case VoiceIndicator::Requesting:
            button_color = ui::palette().warning;
            break;
        case VoiceIndicator::Rx:
            if (flash_on) button_color = ui::palette().success;
            break;
        case VoiceIndicator::On:
            button_color = ui::palette().success;
            break;
        case VoiceIndicator::Starting:
            if (flash_on) button_color = ui::palette().warning;
            break;
        case VoiceIndicator::Unavailable:
        case VoiceIndicator::Idle:
        default:
            break;
    }
    ImGui::PushStyleColor(ImGuiCol_Button, button_color);
    if (view.kind != VoiceIndicator::Idle && view.kind != VoiceIndicator::Unavailable) {
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, button_color);
    } else {
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
    }
    const std::string label = view.label + "##voice_button";
    const bool clicked = ImGui::Button(label.c_str(), ImVec2(-1, 0));
    ImGui::PopStyleColor(2);
    if (!view.tooltip.empty()) {
        ImGui::SetItemTooltip("%s", view.tooltip.c_str());
    }
    return clicked;
}

std::string formatClock(int64_t unix_seconds, bool with_seconds) {
    const auto t = static_cast<time_t>(unix_seconds);
    char buf[16];
    std::strftime(buf, sizeof(buf), with_seconds ? "%H:%M:%S" : "%H:%M", std::localtime(&t));
    return buf;
}

// "-" for an unrecorded table value.
const char* orDash(const std::string& value) {
    return value.empty() ? "-" : value.c_str();
}

}  // anonymous namespace

MainContentRenderer::MainContentRenderer(SharedAppState& state) : WindowBase(state) {}

bool MainContentRenderer::tuneBand(const std::string& band, uint64_t freq_hz) {
    bool ok = false;
    std::string why;
    if (onTuneFrequency) {
        ok = onTuneFrequency(freq_hz);
    } else if (state_.radio && state_.radio->isConnected()) {
        ok = state_.radio->setFrequency(freq_hz);
    } else {
        why = "the radio is not connected (Settings > Radio)";
    }
    if (!ok) {
        if (why.empty() && state_.radio) why = state_.radio->getStatus().last_error;
        if (why.empty()) why = "the radio did not accept the frequency";
        postStatus("Not tuned to " + band + " " + gui::formatDialFrequency(freq_hz) + ": " + why,
                   StatusKind::Error);
        return false;
    }
    // Record the band only once the rig is there.
    if (state_.current_band) *state_.current_band = band;
    if (state_.current_slot) *state_.current_slot = 0;
    if (state_.current_frequency_hz) *state_.current_frequency_hz = static_cast<uint32_t>(freq_hz);
    if (state_.radio && state_.radio->isDryRun()) {
        postStatus("Dry-run radio: " + band + " recorded, but no rig was tuned", StatusKind::Warning);
    } else {
        clearStatus();
    }
    return true;
}

void MainContentRenderer::sendChatInput() {
    if (!state_.chat_input) return;
    const std::string text = state_.chat_input;
    if (gui::trimCopy(text).empty()) return;
    if (!onSendChatMessage) {
        postStatus("Chat is not available", StatusKind::Error);
        return;
    }
    const size_t mark = state_.chatHistorySize();
    onSendChatMessage(text);
    if (state_.outgoingMessageSince(mark)) {
        state_.chat_input[0] = '\0';
        if (status_.kind == StatusKind::Error) clearStatus();
        return;
    }
    // Not sent: keep the text for a retry and say why.
    std::string why = state_.systemMessageSince(mark);
    if (why.empty()) {
        why = gui::isStationLinked(state_.modem) ? "Message not sent"
                                                 : "Message not sent: no station link (CONNECT first)";
        state_.addSystemMessage(why);
    }
    postStatus(why, StatusKind::Error);
}

void MainContentRenderer::render() {
    if (!state_.modem || !state_.config) return;

    const ModemStatus status = state_.modem->getStatus();
    const bool station_connected = state_.modem->isConnected() &&
                                   status.link_state == LinkState::Connected;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float em = ImGui::GetFontSize();

    // The pending-message count is a database query; once a second is
    // plenty for a status counter.
    if (pending_refreshed_at_ < 0.0 || ImGui::GetTime() - pending_refreshed_at_ >= 1.0) {
        pending_count_ = state_.store ? state_.store->countPendingMessages() : 0;
        pending_refreshed_at_ = ImGui::GetTime();
    }

    renderControlPanel(station_connected);
    ui::StatusLine(status_);

    // ========================================================================
    // THREE-PANEL LAYOUT
    // ========================================================================
    const float content_width = ImGui::GetContentRegionAvail().x;
    // The heard lists get up to ~6 rows but give way to the chat area and
    // the right-hand controls on short windows.
    const float row_padding = style.WindowPadding.y * 2.0f;
    float panel_row_height = ImGui::GetTextLineHeightWithSpacing() * 6.5f + row_padding;
    const float panel_row_cap = ImGui::GetContentRegionAvail().y * 0.24f;
    panel_row_height = std::max(ImGui::GetTextLineHeightWithSpacing() * 4.0f + row_padding,
                                std::min(panel_row_height, panel_row_cap));
    const float panel_width = (content_width - style.ItemSpacing.x * 2.0f) / 3.0f;
    const float right_panel_width = em * 12.0f;

    renderHeardPanels(panel_width, panel_row_height);

    // ========================================================================
    // ROW 2: Main message area + Right control panel
    // ========================================================================
    const ui::Palette& pal = ui::palette();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(pal.heading, "IN QSO WITH");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 8.0f);
    char qso_partner[32] = "";
    if (station_connected && state_.current_chat_partner) {
        std::strncpy(qso_partner, state_.current_chat_partner->c_str(), sizeof(qso_partner) - 1);
    }
    ImGui::InputText("##QSOPartner", qso_partner, sizeof(qso_partner), ImGuiInputTextFlags_ReadOnly);

    ImGui::SameLine(0.0f, em * 2.0f);
    ImGui::TextColored(pal.heading, "MESSAGE");
    ImGui::SameLine();
    if (station_connected) {
        ImGui::TextColored(pal.success, "CONNECTED");
    } else {
        ImGui::TextColored(pal.muted, "IDLE");
    }

    ImGui::SameLine(0.0f, em * 2.0f);
    ImGui::TextColored(pal.heading, "FILE");
    ImGui::SameLine();
    // Finished (completed or failed) transfers stay listed; only running ones count.
    bool has_transfers = false;
    if (state_.transfers) {
        for (const auto& t : *state_.transfers) {
            if (!t.completed && !t.failed && !t.cancelled) has_transfers = true;
        }
    }
    if (has_transfers) {
        ImGui::TextColored(pal.success, "ACTIVE");
    } else {
        ImGui::TextColored(pal.muted, "IDLE");
    }

    if (state_.away_mode_active) {
        ImGui::SameLine(0.0f, em * 2.0f);
        ImGui::Checkbox("I'm away (Auto)", state_.away_mode_active);
    }

    // Everything below the main row: the separator, the window buttons and
    // the status bar drawn by the App after this renderer returns.
    const float bottom_reserve = style.ItemSpacing.y * 3.0f + 1.0f +
                                 ImGui::GetFrameHeight() + ui::statusBarHeight() + 1.0f;
    float main_area_height = ImGui::GetContentRegionAvail().y - bottom_reserve;
    if (main_area_height < em * 10.0f) main_area_height = em * 10.0f;

    renderChatArea(content_width - right_panel_width - style.ItemSpacing.x, main_area_height);
    ImGui::SameLine();
    renderRightPanel(right_panel_width, main_area_height);

    renderBroadcastPopup();
    renderBottomButtons();
}

void MainContentRenderer::renderControlPanel(bool station_connected) {
    (void)station_connected;
    const ui::Palette& pal = ui::palette();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float em = ImGui::GetFontSize();
    const ModemStatus status = state_.modem->getStatus();

    ImGui::BeginChild("ControlPanel", ImVec2(-1, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(pal.heading, "FREQUENCY");
    ImGui::SameLine();

    // One band table: the FrequencyManager's (calling frequencies).
    std::vector<BandConfig> bands;
    if (state_.freq_mgr) bands = gui::enabledBands(*state_.freq_mgr);
    const std::string current_band = state_.current_band ? *state_.current_band : std::string();
    std::string preview = current_band.empty() ? std::string("(no band)") : current_band;
    for (const auto& band : bands) {
        if (band.name == current_band) {
            preview = gui::formatDialFrequency(band.calling_freq_hz) + " (" + band.name + ")";
            break;
        }
    }
    if (state_.current_frequency_hz && *state_.current_frequency_hz != 0 && state_.current_slot &&
        *state_.current_slot != 0) {
        preview = gui::formatDialFrequency(*state_.current_frequency_hz) + " (" + current_band +
                  " " + gui::slotLabel(*state_.current_slot) + ")";
    }

    ImGui::SetNextItemWidth(em * 11.0f);
    if (ImGui::BeginCombo("##Freq", preview.c_str())) {
        for (const auto& band : bands) {
            const std::string label =
                gui::formatDialFrequency(band.calling_freq_hz) + " (" + band.name + ")";
            if (ImGui::Selectable(label.c_str(), band.name == current_band)) {
                (void)tuneBand(band.name, band.calling_freq_hz);
            }
        }
        ImGui::EndCombo();
    }
    if (state_.radio && state_.radio->isDryRun()) {
        ImGui::SameLine();
        ImGui::TextColored(pal.warning, "DRY-RUN");
        ImGui::SetItemTooltip("No rig is controlled (Settings > Radio): tuning and PTT are "
                              "simulated only.");
    }

    ImGui::SameLine(0.0f, em * 2.0f);
    ImGui::TextColored(pal.heading, "CONNECT");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 8.0f);
    ImGui::InputTextWithHint("##ConnectTarget", "Callsign", connect_target_, sizeof(connect_target_),
                             ImGuiInputTextFlags_CharsUppercase);

    ImGui::Separator();

    // Row 2: Control buttons
    if (state_.modem->isConnecting()) {
        // The TCP connect runs in the background; keep the button's width.
        ImGui::BeginDisabled();
        ImGui::Button("CONNECTING...", ImVec2(ImGui::CalcTextSize("CONNECT MODEM").x +
                                                  style.FramePadding.x * 2.0f, 0));
        ImGui::EndDisabled();
    } else if (ImGui::Button("CONNECT MODEM")) {
        if (onConnectModem) onConnectModem();
    }
    ImGui::SameLine();
    if (ImGui::Button("DISCONNECT MODEM")) {
        if (onDisconnectModem) onDisconnectModem();
    }
    ImGui::SameLine(0.0f, em * 1.5f);

    auto take_target = [&]() -> std::string {
        const std::string target = gui::normalizeCallsign(connect_target_);
        if (target.empty()) {
            postStatus("Enter a callsign in CONNECT first", StatusKind::Error);
        } else {
            std::strncpy(connect_target_, target.c_str(), sizeof(connect_target_) - 1);
            connect_target_[sizeof(connect_target_) - 1] = '\0';
        }
        return target;
    };
    if (ImGui::Button("CONNECT")) {
        const std::string target = take_target();
        if (!target.empty() && onConnectToStation) onConnectToStation(target);
    }
    ImGui::SameLine();
    if (ImGui::Button("PING")) {
        const std::string target = take_target();
        if (!target.empty() && onSendPing) onSendPing(target);
    }
    ImGui::SameLine();
    if (ImGui::Button("DISCONNECT")) {
        state_.modem->disconnectStation();
    }
    ImGui::SameLine();
    if (ImGui::Button("ABORT")) {
        if (onAbort) onAbort();
    }

    // Row 3: TX/RX state is a status, not a button.
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(status.ptt_active ? pal.danger : pal.success, "%s",
                       status.ptt_active ? "TX" : "RX");
    ImGui::SameLine();

    const int mail_count = state_.mail_inbox ? static_cast<int>(state_.mail_inbox->size()) : 0;
    ImGui::Text("%d/%d", pending_count_, mail_count);
    ImGui::SetItemTooltip("Pending outgoing messages / mails received");

    ImGui::SameLine(0.0f, em * 1.5f);

    // CQ controls: the repeat count applies to the next CALL CQ.
    const bool cq_active = state_.cq_active && *state_.cq_active;
    if (cq_active) {
        ui::PushButtonColor(pal.warning);
        if (ImGui::Button("END CQ")) {
            if (onStopCQ) onStopCQ();
        }
        ui::PopButtonColor();
        if (state_.cq_repeat_count && state_.cq_current_repeat) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(pal.muted, "%d/%d", *state_.cq_current_repeat,
                               *state_.cq_repeat_count);
        }
    } else {
        if (ImGui::Button("CALL CQ")) {
            if (onStartCQ) onStartCQ();
        }
        if (state_.cq_repeat_count) {
            ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
            ImGui::SetNextItemWidth(em * 3.2f);
            char repeat_label[8];
            std::snprintf(repeat_label, sizeof(repeat_label), "x%d", *state_.cq_repeat_count);
            if (ImGui::BeginCombo("##CqRepeat", repeat_label, ImGuiComboFlags_NoArrowButton)) {
                for (int n = 1; n <= 10; ++n) {
                    std::snprintf(repeat_label, sizeof(repeat_label), "x%d", n);
                    if (ImGui::Selectable(repeat_label, *state_.cq_repeat_count == n)) {
                        *state_.cq_repeat_count = n;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SetItemTooltip("How many CQ calls CALL CQ sends");
        }
    }
    ImGui::SameLine();

    const bool beacon_enabled = state_.beacon_enabled && *state_.beacon_enabled;
    if (beacon_enabled) {
        ui::PushButtonColor(pal.info);
        if (ImGui::Button("STOP BEACONS")) {
            if (state_.config) state_.config->beacon.enabled = false;
            if (state_.beacon_enabled) *state_.beacon_enabled = false;
        }
        ui::PopButtonColor();
    } else {
        if (ImGui::Button("SEND BEACONS")) {
            if (state_.config) state_.config->beacon.enabled = true;
            if (state_.beacon_enabled) *state_.beacon_enabled = true;
            if (onSendBeacon) onSendBeacon();
        }
    }

    // Status indicators, right-aligned
    int unread_mail = 0;
    if (state_.mail_inbox) {
        for (const auto& m : *state_.mail_inbox) {
            if (!m.read) unread_mail++;
        }
    }
    const int unread_files = state_.unread_file_notifications ? *state_.unread_file_notifications : 0;
    const size_t beacon_count = state_.heard_beacons ? state_.heard_beacons->size() : 0;

    char mail_btn[32] = "";
    char file_btn[36] = "";
    char outbox_txt[32];
    char parking_txt[32];
    if (unread_mail > 0) {
        std::snprintf(mail_btn, sizeof(mail_btn), "%d NEW MAIL%s", unread_mail, unread_mail > 1 ? "S" : "");
    }
    if (unread_files > 0) {
        std::snprintf(file_btn, sizeof(file_btn), "%d NEW FILE%s", unread_files, unread_files > 1 ? "S" : "");
    }
    std::snprintf(outbox_txt, sizeof(outbox_txt), "Outbox: %d", pending_count_);
    std::snprintf(parking_txt, sizeof(parking_txt), "Parking: %zu", beacon_count);

    float right_width = ImGui::CalcTextSize(outbox_txt).x + ImGui::CalcTextSize(parking_txt).x +
                        style.ItemSpacing.x * 2.0f + em;
    if (mail_btn[0]) {
        right_width += ImGui::CalcTextSize(mail_btn).x + style.FramePadding.x * 2.0f + style.ItemSpacing.x;
    }
    if (file_btn[0]) {
        right_width += ImGui::CalcTextSize(file_btn).x + style.FramePadding.x * 2.0f + style.ItemSpacing.x;
    }
    const float right_x = ImGui::GetContentRegionMax().x - right_width;
    ImGui::SameLine();
    if (right_x > ImGui::GetCursorPosX()) {
        ImGui::SetCursorPosX(right_x);
    }

    if (mail_btn[0]) {
        ui::PushButtonColor(pal.success);
        if (ImGui::Button(mail_btn)) {
            if (onOpenMailInbox) onOpenMailInbox();
            if (state_.current_tab) *state_.current_tab = AppTab::Mail;
        }
        ui::PopButtonColor();
        ImGui::SameLine();
    }
    if (file_btn[0]) {
        ui::PushButtonColor(pal.accent);
        if (ImGui::Button(file_btn)) {
            if (state_.current_tab) *state_.current_tab = AppTab::Files;
            if (state_.unread_file_notifications) *state_.unread_file_notifications = 0;
        }
        ui::PopButtonColor();
        ImGui::SameLine();
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(pal.muted, "%s", outbox_txt);
    ImGui::SameLine(0.0f, em);
    ImGui::TextColored(pal.muted, "%s", parking_txt);

    ImGui::EndChild();
}

void MainContentRenderer::renderHeardPanels(float panel_width, float panel_height) {
    const ui::Palette& pal = ui::palette();
    const bool show_time = state_.config->ui.show_timestamps;
    const ImGuiTableFlags heard_table_flags =
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_NoPadOuterX;

    // Log panel (left)
    ImGui::BeginChild("LogPanel", ImVec2(panel_width, panel_height), ImGuiChildFlags_Borders);
    ui::SectionHeader("Skywave Link Log");
    if (state_.chat_history) {
        ImGui::PushFont(ui::monoFont(), 0.0f);
        int count = 0;
        for (auto it = state_.chat_history->rbegin();
             it != state_.chat_history->rend() && count < 8; ++it) {
            if (it->from == "SYSTEM") {
                if (show_time) {
                    ImGui::TextColored(pal.muted, "%s", formatClock(it->timestamp, true).c_str());
                    ImGui::SameLine();
                }
                ImGui::TextUnformatted(it->text.c_str());
                count++;
            }
        }
        ImGui::PopFont();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Last heard beacons panel (center). Band, bandwidth and SNR are what
    // was recorded for that beacon; "-" when not known.
    ImGui::BeginChild("HeardBeaconsPanel", ImVec2(panel_width, panel_height), ImGuiChildFlags_Borders);
    ui::SectionHeader("Last heard beacons");
    if (ImGui::BeginTable("BeaconCols", 5, heard_table_flags)) {
        ImGui::TableSetupColumn("Bnd", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Call", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableSetupColumn("BW", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("SNR", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableHeadersRow();
        if (state_.heard_beacons) {
            int count = 0;
            for (const auto& beacon : *state_.heard_beacons) {
                if (count++ > 5) break;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(orDash(beacon.band));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(formatClock(beacon.timestamp, false).c_str());
                ImGui::TableNextColumn();
                if (beacon.has_pending_messages) {
                    ImGui::TextColored(pal.warning, "%s", beacon.callsign.c_str());
                } else if (beacon.email_gateway) {
                    ImGui::TextColored(pal.info, "%s @", beacon.callsign.c_str());
                } else {
                    ImGui::TextUnformatted(beacon.callsign.c_str());
                }
                if (beacon.email_gateway && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Email gateway capable station");
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(orDash(gui::familyBandwidthLabel(beacon.family)));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    (beacon.snr_valid ? gui::formatSnr(beacon.snr_db) : std::string("-")).c_str());
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Last heard CQ calls panel (right). Click a call to put it in CONNECT.
    ImGui::BeginChild("CQCallsPanel", ImVec2(0, panel_height), ImGuiChildFlags_Borders);
    ui::SectionHeader("Last heard CQ calls");
    if (state_.heard_calls) {
        if (ImGui::BeginTable("CQCols", 5, heard_table_flags)) {
            ImGui::TableSetupColumn("Bnd", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Call", ImGuiTableColumnFlags_WidthStretch, 1.8f);
            ImGui::TableSetupColumn("BW", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("SNR", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableHeadersRow();
            int count = 0;
            for (const auto& call : *state_.heard_calls) {
                if (count++ > 5) break;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(orDash(call.band));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(formatClock(call.timestamp, false).c_str());
                ImGui::TableNextColumn();
                ImGui::PushID(count);
                if (ImGui::Selectable(call.callsign.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                    std::snprintf(connect_target_, sizeof(connect_target_), "%s", call.callsign.c_str());
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(orDash(gui::familyBandwidthLabel(call.family)));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    (call.snr_valid ? gui::formatSnr(call.snr_db) : std::string("-")).c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}

void MainContentRenderer::renderChatArea(float width, float height) {
    const ui::Palette& pal = ui::palette();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float em = ImGui::GetFontSize();
    const bool show_time = state_.config->ui.show_timestamps;

    ImGui::BeginChild("MainChatArea", ImVec2(width, height), ImGuiChildFlags_None);

    const float chat_height = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginChild("MainChat", ImVec2(-1, chat_height), ImGuiChildFlags_Borders)) {
        const ImGuiTableFlags chat_flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                                           ImGuiTableFlags_NoPadOuterX;
        const int columns = show_time ? 5 : 4;
        if (ImGui::BeginTable("ChatTable", columns, chat_flags)) {
            ImGui::TableSetupColumn("Bnd", ImGuiTableColumnFlags_WidthFixed, em * 2.6f);
            if (show_time) {
                ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, em * 3.0f);
            }
            ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthFixed, em * 5.5f);
            ImGui::TableSetupColumn("To", ImGuiTableColumnFlags_WidthFixed, em * 5.5f);
            ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            if (state_.chat_history) {
                const std::string my_call = state_.config->station.callsign;
                const std::string partner = state_.current_chat_partner ? *state_.current_chat_partner : "";

                for (const auto& msg : *state_.chat_history) {
                    if (msg.from == "SYSTEM") continue;

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(pal.muted, "%s", msg.band.c_str());
                    if (show_time) {
                        ImGui::TableNextColumn();
                        ImGui::TextColored(pal.muted, "%s", formatClock(msg.timestamp, false).c_str());
                    }
                    ImGui::TableNextColumn();
                    if (msg.is_outgoing) {
                        ImGui::TextColored(pal.info, "%s", my_call.substr(0, 10).c_str());
                    } else {
                        ImGui::TextColored(pal.warning, "%s", msg.from.substr(0, 10).c_str());
                    }

                    std::string to_call = msg.to;
                    if (to_call.empty()) {
                        if (msg.is_broadcast) {
                            to_call = "ALL";
                        } else if (msg.is_outgoing) {
                            to_call = partner.empty() ? "UNKNOWN" : partner;
                        } else {
                            to_call = my_call;
                        }
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(to_call.substr(0, 10).c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s", msg.text.c_str());
                }
            }
            ImGui::EndTable();
        }

        if (state_.chat_scroll_to_bottom && *state_.chat_scroll_to_bottom) {
            ImGui::SetScrollHereY(1.0f);
            *state_.chat_scroll_to_bottom = false;
        }
    }
    ImGui::EndChild();

    // Input field
    const float send_width = ImGui::CalcTextSize("Send").x + style.FramePadding.x * 4.0f;
    ImGui::PushItemWidth(-(send_width + style.ItemSpacing.x));
    bool send_msg = false;
    if (state_.chat_input) {
        send_msg = ImGui::InputTextWithHint("##MsgInput", "Type a message and press Enter",
                                            state_.chat_input, state_.chat_input_size,
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        if (send_msg) {
            ImGui::SetKeyboardFocusHere(-1);  // keep typing after Enter
        }
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();
    ui::PushButtonColor(pal.accent);
    const bool send_clicked = ImGui::Button("Send", ImVec2(send_width, 0));
    ui::PopButtonColor();
    if (send_clicked || send_msg) {
        sendChatInput();
    }

    ImGui::EndChild();
}

void MainContentRenderer::renderRightPanel(float width, float height) {
    const ui::Palette& pal = ui::palette();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ModemStatus status = state_.modem->getStatus();

    // No scrollbar (it would only show for a few pixels of overflow on short
    // windows); the wheel still scrolls if the controls ever do not fit.
    ImGui::BeginChild("RightPanel", ImVec2(width, height), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar);

    ImGui::TextColored(pal.heading, "Duration");
    ImGui::SameLine();
    ImGui::PushFont(ui::monoFont(), 0.0f);
    if (state_.qso_active && *state_.qso_active) {
        ImGui::Text("%s", state_.formatQSODuration().c_str());
    } else {
        ImGui::TextColored(pal.muted, "--:--:--");
    }
    ImGui::PopFont();

    ui::SectionHeader("SNR (dB)");
    // Last: this station's RX SNR. Avg: its running mean over the current
    // session. Mine: how well the peer hears this station (modem RSNR).
    const bool linked = status.link_state == LinkState::Connected;
    if (!linked) {
        snr_sum_ = 0.0;
        snr_samples_ = 0;
        snr_last_sampled_ = -1000.0f;
    } else if (status.snr_db != snr_last_sampled_) {
        // Sample on change (EVT:SNR / STATUS), not per frame.
        snr_sum_ += status.snr_db;
        ++snr_samples_;
        snr_last_sampled_ = status.snr_db;
    }
    if (ImGui::BeginTable("SnrTable", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextColored(pal.muted, "Last");
        ImGui::TableNextColumn(); ImGui::TextColored(pal.muted, "Avg");
        ImGui::TableNextColumn(); ImGui::TextColored(pal.muted, "Mine");
        ImGui::PushFont(ui::monoFont(), 0.0f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (linked) ImGui::Text("%.0f", status.snr_db); else ImGui::TextColored(pal.muted, "--");
        ImGui::TableNextColumn();
        if (snr_samples_ > 0) {
            ImGui::Text("%.0f", snr_sum_ / snr_samples_);
        } else {
            ImGui::TextColored(pal.muted, "--");
        }
        ImGui::TableNextColumn();
        if (linked && status.remote_snr_valid) {
            ImGui::Text("%.0f", status.remote_snr_db);
        } else {
            ImGui::TextColored(pal.muted, "--");
        }
        ImGui::PopFont();
        ImGui::EndTable();
    }

    // Last PING answer (PING button in the control panel).
    ui::SectionHeader("Ping");
    if (state_.ping_in_progress && *state_.ping_in_progress) {
        ImGui::TextColored(pal.warning, "Waiting for reply...");
    } else if (state_.ping_results && !state_.ping_results->empty()) {
        const PingResult& last = state_.ping_results->front();
        if (last.online) {
            ImGui::TextColored(pal.success, "%s %s", last.callsign.c_str(),
                               gui::formatSnr(last.snr_db, true).c_str());
        } else {
            ImGui::TextColored(pal.danger, "%s no reply", last.callsign.c_str());
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            for (const auto& r : *state_.ping_results) {
                ImGui::Text("%s  %s  %s  %s", formatClock(r.timestamp, false).c_str(),
                            r.callsign.c_str(),
                            r.online ? gui::formatSnr(r.snr_db, true).c_str() : "no reply",
                            r.grid_square.c_str());
            }
            ImGui::EndTooltip();
        }
    } else {
        ImGui::TextColored(pal.muted, "--");
    }

    ui::SectionHeader("Actions");

    if (ImGui::Button("SEND FILE", ImVec2(-1, 0))) {
        if (state_.current_tab) *state_.current_tab = AppTab::Files;
    }
    {
        // Opens the Mail window on its Inbox (Compose is a tab there).
        int unread = 0;
        if (state_.mail_inbox) {
            for (const auto& m : *state_.mail_inbox) unread += m.read ? 0 : 1;
        }
        char mail_label[40];
        if (unread > 0) {
            std::snprintf(mail_label, sizeof(mail_label), "MAIL (%d new)###MailButton", unread);
        } else {
            std::snprintf(mail_label, sizeof(mail_label), "MAIL###MailButton");
        }
        if (unread > 0) ui::PushButtonColor(pal.accent);
        const bool open_mail = ImGui::Button(mail_label, ImVec2(-1, 0));
        if (unread > 0) ui::PopButtonColor();
        ImGui::SetItemTooltip("Mail: Inbox, Sent and Compose");
        if (open_mail) {
            if (onOpenMailInbox) onOpenMailInbox();
            if (state_.current_tab) *state_.current_tab = AppTab::Mail;
        }
    }
    ui::PushButtonColor(pal.danger);
    if (ImGui::Button("BROADCAST", ImVec2(-1, 0))) {
        open_broadcast_popup_ = true;
    }
    ui::PopButtonColor();

    const VoiceStatus voice = state_.modem->getVoiceStatus();
    static const VoiceUiState kNoVoiceUi;
    const VoiceUiState& vui = state_.voice_ui ? *state_.voice_ui : kNoVoiceUi;
    // PTT is clickable whenever the modem is up and voice is not OFF: a
    // press that cannot key says why on the status line (App).
    // PTT and VOICE share the bottom row (one row fits short windows).
    const float indicator_height = ImGui::GetFrameHeight();
    const float remaining_height = ImGui::GetContentRegionAvail().y;
    if (remaining_height > indicator_height + style.ItemSpacing.y) {
        ImGui::Dummy(ImVec2(0.0f, remaining_height - indicator_height - style.ItemSpacing.y));
    }
    const float half_width =
        std::max(1.0f, (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) * 0.5f);
    const bool ptt_available = vui.ptt_available;
    if (!ptt_available && ptt_button_held_) {
        ptt_button_held_ = false;
        if (onVoicePtt) onVoicePtt(false);
    }
    if (!ptt_available) {
        ImGui::BeginDisabled();
    }
    const bool ptt_lit = voice.ptt || (ptt_button_held_ && vui.ptt_wanted);
    if (ptt_lit) {
        ui::PushButtonColor(pal.danger);
    }
    ImGui::Button("PTT", ImVec2(half_width, 0));
    const bool button_held_now = ptt_available && ImGui::IsItemActive();
    if (ptt_lit) {
        ui::PopButtonColor();
    }
    if (!vui.ptt_tooltip.empty()) {
        ImGui::SetItemTooltip("%s", vui.ptt_tooltip.c_str());
    }
    if (button_held_now != ptt_button_held_) {
        ptt_button_held_ = button_held_now;
        if (onVoicePtt) onVoicePtt(ptt_button_held_);
    }
    if (!ptt_available) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (renderVoiceButton(vui.button) && onVoiceButton) {
        onVoiceButton();
    }

    ImGui::EndChild();
}

void MainContentRenderer::renderBroadcastPopup() {
    const ui::Palette& pal = ui::palette();
    const float em = ImGui::GetFontSize();

    if (open_broadcast_popup_) {
        ImGui::OpenPopup("BroadcastPopup");
        open_broadcast_popup_ = false;
        if (status_.kind == StatusKind::Error) clearStatus();  // not about this popup
    }
    if (!ImGui::BeginPopupModal("BroadcastPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    if (state_.broadcast_message) {
        ImGui::Text("Broadcast Message:");
        ImGui::InputTextMultiline("##BroadcastText", state_.broadcast_message,
                                  state_.broadcast_message_size, ImVec2(em * 22.0f, em * 6.0f));
    }

    ImGui::Separator();
    const double lat = state_.my_lat ? *state_.my_lat : 0.0;
    const double lon = state_.my_lon ? *state_.my_lon : 0.0;
    const bool have_position = gui::isUsablePosition(lat, lon);
    ImGui::BeginDisabled(!have_position);
    ImGui::Checkbox("Include Position Report", &include_position_);
    ImGui::EndDisabled();
    if (!have_position) {
        include_position_ = false;
        ImGui::SameLine();
        ImGui::TextColored(pal.muted, "(no position set)");
    } else if (include_position_) {
        ImGui::TextColored(pal.muted, "Position: %.4f, %.4f (%s)", lat, lon,
                           state_.config->station.grid_square.c_str());
        ImGui::TextWrapped("Receiving stations will relay your position to APRS-IS");
    }

    ImGui::Separator();
    ui::PushButtonColor(pal.danger);
    const bool send_broadcast = ImGui::Button("Send", ImVec2(em * 7.0f, 0));
    ui::PopButtonColor();
    if (send_broadcast) {
        const std::string text = state_.broadcast_message ? state_.broadcast_message : "";
        bool close = true;
        if (include_position_) {
            const auto packet = gui::buildPositionBeaconPacket(
                lat, lon, state_.config->station.grid_square, state_.config->station.callsign, text);
            if (onSendBroadcastPayload && onSendBroadcastPayload(packet)) {
                state_.addSystemMessage("[POSITION BROADCAST] " +
                                        gui::trimCopy(state_.config->station.grid_square) + " - " + text);
                postStatus("Position broadcast queued", StatusKind::Success);
            } else {
                state_.addSystemMessage("Position broadcast not sent: the modem is not connected");
                postStatus("Position broadcast not sent: the modem is not connected",
                           StatusKind::Error);
                close = false;  // keep the text
            }
        } else if (!gui::trimCopy(text).empty()) {
            const size_t mark = state_.chatHistorySize();
            if (onSendBroadcast) onSendBroadcast(text);
            if (!state_.outgoingMessageSince(mark)) {
                const std::string why = state_.systemMessageSince(mark);
                postStatus(why.empty() ? std::string("Broadcast not sent") : why, StatusKind::Error);
                close = false;
            }
        } else {
            postStatus("Broadcast is empty: nothing sent", StatusKind::Warning);
        }
        if (close) {
            if (state_.broadcast_message) state_.broadcast_message[0] = '\0';
            include_position_ = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(em * 7.0f, 0))) {
        include_position_ = false;
        ImGui::CloseCurrentPopup();
    }
    if (status_.kind == StatusKind::Error && !status_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, pal.danger);
        ImGui::TextWrapped("%s", status_.text.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndPopup();
}

void MainContentRenderer::renderBottomButtons() {
    ImGui::Separator();

    struct WindowButton {
        const char* label;
        AppTab tab;
    };
    static const WindowButton kButtons[] = {
        {"Settings", AppTab::Settings},   {"BBS", AppTab::BBS},
        {"Frequency", AppTab::Frequency}, {"Messages", AppTab::Messages},
        {"Position", AppTab::Position},   {"Mail", AppTab::Mail},
        {"Weather", AppTab::Weather},     {"Station Map", AppTab::StationMap},
    };
    bool first = true;
    for (const auto& button : kButtons) {
        if (!first) ImGui::SameLine();
        first = false;
        if (ImGui::Button(button.label)) {
            if (state_.current_tab) *state_.current_tab = button.tab;
            if (button.tab == AppTab::Settings && state_.show_settings) *state_.show_settings = true;
        }
    }
}

} // namespace skylink
