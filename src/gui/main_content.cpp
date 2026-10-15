// MainContentRenderer - Control panel, three-panel layout, right panel, bottom buttons

#include "skywave_link/main_content.hpp"
#include "skywave_link/modem_client.hpp"
#include "imgui.h"
#include <cstring>
#include <ctime>
#include <chrono>
#include <cstdio>

namespace skylink {

namespace {

VoiceMode configuredVoiceMode(const std::string& value) {
    std::string norm = value;
    size_t start = norm.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return VoiceMode::Off;
    }
    size_t end = norm.find_last_not_of(" \t\r\n");
    norm = norm.substr(start, end - start + 1);
    for (auto& c : norm) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (norm == "CONNECTED") return VoiceMode::Connected;
    if (norm == "BROADCAST") return VoiceMode::Broadcast;
    return VoiceMode::Off;
}

std::string normalizeCallsign(const std::string& input) {
    std::string out = input;
    // Trim whitespace
    size_t start = out.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = out.find_last_not_of(" \t\r\n");
    out = out.substr(start, end - start + 1);
    // Uppercase
    for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// Endian-safe serialization
void writeDoubleLE(std::vector<uint8_t>& vec, double value) {
    uint8_t bytes[sizeof(double)];
    std::memcpy(bytes, &value, sizeof(double));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    for (size_t i = 0; i < sizeof(double) / 2; ++i)
        std::swap(bytes[i], bytes[sizeof(double) - 1 - i]);
#endif
    vec.insert(vec.end(), bytes, bytes + sizeof(double));
}

void renderVoiceActivityIndicator(const VoiceStatus& voice) {
    const ImVec4 base_button = ImGui::GetStyleColorVec4(ImGuiCol_Button);

    const bool tx_active = (voice.state == VoiceState::TxActive);
    const bool rx_active = (voice.state == VoiceState::RxActive);
    const bool flash_on = (static_cast<int>(ImGui::GetTime() * 4.0) % 2) == 0;

    ImVec4 button_color = base_button;
    const char* label = "VOICE";

    if (tx_active) {
        label = "VOICE TX";
        if (flash_on) {
            button_color = ImVec4(0.84f, 0.26f, 0.24f, 1.0f);
        }
    } else if (rx_active) {
        label = "VOICE RX";
        if (flash_on) {
            button_color = ImVec4(0.20f, 0.70f, 0.28f, 1.0f);
        }
    }

    ImGui::PushStyleColor(ImGuiCol_Button, button_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, button_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, button_color);
    ImGui::Button(label, ImVec2(-1, 0));
    ImGui::PopStyleColor(3);
}

} // anonymous namespace

MainContentRenderer::MainContentRenderer(SharedAppState& state) : WindowBase(state) {}

void MainContentRenderer::render() {
    if (!state_.modem || !state_.config) return;

    const auto& status = state_.modem->getStatus();
    bool station_connected = state_.modem->isConnected() &&
                             status.link_state == LinkState::Connected;
    static char connect_target[32] = "";

    // Band/frequency options
    struct BandFreq {
        const char* band;
        uint64_t freq_hz;
        const char* display;
    };
    static const BandFreq band_freqs[] = {
        {"80m", 3590000,  "3.590.000 (80m)"},
        {"40m", 7055000,  "7.055.000 (40m)"},
        {"30m", 10130000, "10.130.000 (30m)"},
        {"20m", 14105000, "14.105.000 (20m)"},
        {"17m", 18105000, "18.105.000 (17m)"},
        {"15m", 21105000, "21.105.000 (15m)"},
        {"12m", 24925000, "24.925.000 (12m)"},
        {"10m", 28120000, "28.120.000 (10m)"},
    };

    // ========================================================================
    // CONTROL PANEL
    // ========================================================================
    ImGui::BeginChild("ControlPanel", ImVec2(-1, 85), true);

    ImGui::Text("FREQUENCY");
    ImGui::SameLine(100);

    int current_idx = 1;
    for (int i = 0; i < 8; i++) {
        if (state_.current_band && *state_.current_band == band_freqs[i].band) {
            current_idx = i;
            break;
        }
    }

    ImGui::SetNextItemWidth(170);
    if (ImGui::BeginCombo("##Freq", band_freqs[current_idx].display)) {
        for (int i = 0; i < 8; i++) {
            if (ImGui::Selectable(band_freqs[i].display, current_idx == i)) {
                if (state_.current_band) *state_.current_band = band_freqs[i].band;
                if (onSetFrequency) onSetFrequency(band_freqs[i].freq_hz);
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine(300);
    ImGui::Text("Connect");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::InputText("##ConnectTarget", connect_target, sizeof(connect_target),
                     ImGuiInputTextFlags_CharsUppercase);

    ImGui::Separator();

    // Row 2: Control buttons
    if (ImGui::Button("CONNECT MODEM")) {
        if (onConnectModem) onConnectModem();
    }
    ImGui::SameLine();
    if (ImGui::Button("DISCONNECT MODEM")) {
        if (onDisconnectModem) onDisconnectModem();
    }
    ImGui::SameLine();
    ImGui::Spacing(); ImGui::SameLine();

    if (ImGui::Button("CONNECT")) {
        std::string target = normalizeCallsign(connect_target);
        if (!target.empty()) {
            std::strncpy(connect_target, target.c_str(), sizeof(connect_target) - 1);
            connect_target[sizeof(connect_target) - 1] = '\0';
            if (onConnectToStation) onConnectToStation(target);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("PING")) {
        std::string target = normalizeCallsign(connect_target);
        if (!target.empty()) {
            std::strncpy(connect_target, target.c_str(), sizeof(connect_target) - 1);
            connect_target[sizeof(connect_target) - 1] = '\0';
            if (onSendPing) onSendPing(target);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("DISCONNECT")) {
        state_.modem->disconnectStation();
    }
    ImGui::SameLine();
    if (ImGui::Button("ABORT")) {
        if (onAbort) onAbort();
    }

    // Row 3
    if (status.ptt_active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        ImGui::Button("TX");
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
        ImGui::Button("RX");
        ImGui::PopStyleColor();
    }
    ImGui::SameLine();

    int pending = state_.store ? state_.store->countPendingMessages() : 0;
    int mail_count = state_.mail_inbox ? static_cast<int>(state_.mail_inbox->size()) : 0;
    ImGui::Text("%d/%d", pending, mail_count);

    ImGui::SameLine();
    ImGui::Spacing(); ImGui::SameLine();

    // CQ controls
    bool cq_active = state_.cq_active && *state_.cq_active;
    if (cq_active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.4f, 0.2f, 1.0f));
        if (ImGui::Button("END CQ")) {
            if (onStopCQ) onStopCQ();
        }
        ImGui::PopStyleColor();
    } else {
        if (ImGui::Button("CALL CQ")) {
            if (onStartCQ) onStartCQ();
        }
    }
    ImGui::SameLine();

    bool beacon_enabled = state_.beacon_enabled && *state_.beacon_enabled;
    if (beacon_enabled) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.8f, 1.0f));
        if (ImGui::Button("STOP BEACONS")) {
            if (state_.config) state_.config->beacon.enabled = false;
            if (state_.beacon_enabled) *state_.beacon_enabled = false;
        }
        ImGui::PopStyleColor();
    } else {
        if (ImGui::Button("SEND BEACONS")) {
            if (state_.config) state_.config->beacon.enabled = true;
            if (state_.beacon_enabled) *state_.beacon_enabled = true;
            if (onSendBeacon) onSendBeacon();
        }
    }

    // Status indicators on the right
    ImGui::SameLine(ImGui::GetWindowWidth() - 350);

    int unread_mail = 0;
    if (state_.mail_inbox) {
        for (const auto& m : *state_.mail_inbox) {
            if (!m.read) unread_mail++;
        }
    }
    if (unread_mail > 0) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.6f, 0.0f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        char mail_btn[32];
        std::snprintf(mail_btn, sizeof(mail_btn), "%d NEW MAIL%s", unread_mail, unread_mail > 1 ? "S" : "");
        ImGui::Button(mail_btn);
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
    }

    int unread_files = state_.unread_file_notifications ? *state_.unread_file_notifications : 0;
    if (unread_files > 0) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.4f, 0.9f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        char file_btn[36];
        std::snprintf(file_btn,
                      sizeof(file_btn),
                      "%d NEW FILE%s",
                      unread_files,
                      unread_files > 1 ? "S" : "");
        if (ImGui::Button(file_btn)) {
            if (state_.current_tab) *state_.current_tab = AppTab::Files;
            if (state_.unread_file_notifications) *state_.unread_file_notifications = 0;
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
    }

    ImGui::Text("Outbox: %d", pending);
    ImGui::SameLine();

    size_t beacon_count = state_.heard_beacons ? state_.heard_beacons->size() : 0;
    ImGui::Text("Parking: %zu", beacon_count);

    ImGui::EndChild();

    // ========================================================================
    // THREE-PANEL LAYOUT
    // ========================================================================
    float content_width = ImGui::GetContentRegionAvail().x;
    float panel_row_height = 120;
    float panel_width = (content_width - 20) / 3.0f;
    float right_panel_width = 140;
    float main_area_height = ImGui::GetContentRegionAvail().y - panel_row_height - 40;

    // Log panel (left)
    ImGui::BeginChild("LogPanel", ImVec2(panel_width, panel_row_height), true);
    ImGui::Text("Skywave Link Log");
    ImGui::Separator();
    if (state_.chat_history) {
        int count = 0;
        for (auto it = state_.chat_history->rbegin();
             it != state_.chat_history->rend() && count < 8; ++it) {
            if (it->from == "SYSTEM") {
                auto time_t_val = static_cast<time_t>(it->timestamp);
                char time_str[16];
                std::strftime(time_str, sizeof(time_str), "%H:%M:%S", std::localtime(&time_t_val));
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s - %s", time_str, it->text.c_str());
                count++;
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Last heard beacons panel (center)
    ImGui::BeginChild("HeardBeaconsPanel", ImVec2(panel_width, panel_row_height), true);
    ImGui::Text("Last heard beacons");
    ImGui::Separator();
    ImGui::Columns(5, "BeaconCols", false);
    ImGui::SetColumnWidth(0, 30); ImGui::SetColumnWidth(1, 40);
    ImGui::SetColumnWidth(2, 60); ImGui::SetColumnWidth(3, 30); ImGui::SetColumnWidth(4, 35);
    ImGui::Text("Bnd"); ImGui::NextColumn();
    ImGui::Text("Time"); ImGui::NextColumn();
    ImGui::Text("Call"); ImGui::NextColumn();
    ImGui::Text("BW"); ImGui::NextColumn();
    ImGui::Text("SNR"); ImGui::NextColumn();
    if (state_.heard_beacons) {
        int count = 0;
        for (const auto& beacon : *state_.heard_beacons) {
            if (count++ > 5) break;
            std::string band = state_.current_band ? *state_.current_band : "20m";
            ImGui::Text("%s", band.c_str()); ImGui::NextColumn();
            auto time_t_val = static_cast<time_t>(beacon.timestamp);
            char time_str[16];
            std::strftime(time_str, sizeof(time_str), "%H:%M", std::localtime(&time_t_val));
            ImGui::Text("%s", time_str); ImGui::NextColumn();
            if (beacon.has_pending_messages) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", beacon.callsign.c_str());
            } else if (beacon.email_gateway) {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s @", beacon.callsign.c_str());
            } else {
                ImGui::Text("%s", beacon.callsign.c_str());
            }
            if (beacon.email_gateway && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Email gateway capable station");
            }
            ImGui::NextColumn();
            ImGui::Text("2300"); ImGui::NextColumn();
            ImGui::Text("%.0f", beacon.snr_db); ImGui::NextColumn();
        }
    }
    ImGui::Columns(1);
    ImGui::EndChild();

    ImGui::SameLine();

    // Last heard CQ calls panel (right)
    ImGui::BeginChild("CQCallsPanel", ImVec2(panel_width, panel_row_height), true);
    ImGui::Text("Last heard CQ calls");
    ImGui::Separator();
    ImGui::Columns(5, "CQCols", false);
    ImGui::SetColumnWidth(0, 30); ImGui::SetColumnWidth(1, 40);
    ImGui::SetColumnWidth(2, 60); ImGui::SetColumnWidth(3, 30); ImGui::SetColumnWidth(4, 35);
    ImGui::Text("Bnd"); ImGui::NextColumn();
    ImGui::Text("Time"); ImGui::NextColumn();
    ImGui::Text("Call"); ImGui::NextColumn();
    ImGui::Text("BW"); ImGui::NextColumn();
    ImGui::Text("SNR"); ImGui::NextColumn();
    if (state_.heard_stations) {
        int count = 0;
        for (const auto& [callsign, timestamp] : *state_.heard_stations) {
            if (count++ > 5) break;
            std::string band = state_.current_band ? *state_.current_band : "20m";
            ImGui::Text("%s", band.c_str()); ImGui::NextColumn();
            ImGui::Text("%s", timestamp.c_str()); ImGui::NextColumn();
            if (ImGui::Selectable(callsign.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                std::strncpy(connect_target, callsign.c_str(), sizeof(connect_target) - 1);
                connect_target[sizeof(connect_target) - 1] = '\0';
            }
            ImGui::NextColumn();
            ImGui::Text("2300"); ImGui::NextColumn();
            ImGui::Text("-"); ImGui::NextColumn();
        }
    }
    ImGui::Columns(1);
    ImGui::EndChild();

    // ========================================================================
    // ROW 2: Main message area + Right control panel
    // ========================================================================
    ImGui::Text("In QSO with");
    ImGui::SameLine(90);
    ImGui::SetNextItemWidth(100);
    char qso_partner[32] = "";
    if (station_connected && state_.current_chat_partner) {
        std::strncpy(qso_partner, state_.current_chat_partner->c_str(), sizeof(qso_partner) - 1);
    }
    ImGui::InputText("##QSOPartner", qso_partner, sizeof(qso_partner), ImGuiInputTextFlags_ReadOnly);

    ImGui::SameLine(210);
    ImGui::Text("Message");
    ImGui::SameLine(280);
    ImGui::Text(station_connected ? "CONNECTED" : "IDLE");

    ImGui::SameLine(380);
    ImGui::Text("File");
    ImGui::SameLine(420);
    bool has_transfers = state_.transfers && !state_.transfers->empty();
    ImGui::Text(has_transfers ? "ACTIVE" : "IDLE");

    ImGui::SameLine(520);
    if (state_.away_mode_active) {
        ImGui::Checkbox("I'm away (Auto)", state_.away_mode_active);
    }

    // Main chat area
    float main_chat_width = content_width - right_panel_width - 10;
    ImGui::BeginChild("MainChatArea", ImVec2(main_chat_width, main_area_height), false);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.7f, 0.95f, 0.95f, 1.0f));
    float chat_height = ImGui::GetContentRegionAvail().y - 35;
    if (ImGui::BeginChild("MainChat", ImVec2(-1, chat_height), true)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));

        ImGui::TextColored(ImVec4(0.5f, 0.0f, 0.0f, 1.0f), "Bnd   Time   From       To         Message");
        ImGui::Separator();

        if (state_.chat_history) {
            std::string band = state_.current_band ? *state_.current_band : "20m";
            std::string my_call = state_.config ? state_.config->station.callsign : "";
            std::string partner = state_.current_chat_partner ? *state_.current_chat_partner : "";

            for (const auto& msg : *state_.chat_history) {
                if (msg.from == "SYSTEM") continue;

                auto time_t_val = static_cast<time_t>(msg.timestamp);
                char time_str[16];
                std::strftime(time_str, sizeof(time_str), "%H:%M", std::localtime(&time_t_val));

                ImGui::Text("%-4s", band.c_str());
                ImGui::SameLine(45);
                ImGui::Text("%-6s", time_str);
                ImGui::SameLine(95);

                if (msg.is_outgoing) {
                    ImGui::TextColored(ImVec4(0.0f, 0.0f, 0.6f, 1.0f), "%-10s",
                        my_call.substr(0, 10).c_str());
                } else {
                    ImGui::TextColored(ImVec4(0.6f, 0.0f, 0.0f, 1.0f), "%-10s",
                        msg.from.substr(0, 10).c_str());
                }
                ImGui::SameLine(175);

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
                ImGui::Text("%-10s", to_call.substr(0, 10).c_str());
                ImGui::SameLine(255);

                ImGui::TextWrapped("%s", msg.text.c_str());
            }
        }

        if (state_.chat_scroll_to_bottom && *state_.chat_scroll_to_bottom) {
            ImGui::SetScrollHereY(1.0f);
            *state_.chat_scroll_to_bottom = false;
        }

        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Input field
    ImGui::PushItemWidth(-60);
    bool send_msg = false;
    if (state_.chat_input) {
        send_msg = ImGui::InputText("##MsgInput", state_.chat_input, state_.chat_input_size,
                                     ImGuiInputTextFlags_EnterReturnsTrue);
    }
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Send") || send_msg) {
        if (state_.chat_input && std::strlen(state_.chat_input) > 0) {
            if (onSendChatMessage) onSendChatMessage(state_.chat_input);
            state_.chat_input[0] = '\0';
        }
    }

    // Typing indicator
    if (state_.chat_input && state_.last_input_was_empty) {
        if (std::strlen(state_.chat_input) > 0 && !*state_.last_input_was_empty) {
            if (onSendTypingIndicator) onSendTypingIndicator();
        }
        *state_.last_input_was_empty = (std::strlen(state_.chat_input) == 0);
    }

    ImGui::EndChild();

    ImGui::SameLine();

    // ========================================================================
    // RIGHT PANEL
    // ========================================================================
    ImGui::BeginChild("RightPanel", ImVec2(right_panel_width, main_area_height), true);

    ImGui::Text("Duration:");
    if (state_.qso_active && *state_.qso_active) {
        ImGui::Text("%s", state_.formatQSODuration().c_str());
    } else {
        ImGui::Text("--:--:--");
    }

    ImGui::Separator();

    ImGui::Text("SNR(db)");
    ImGui::Text("Last  Avg   Mine");
    float remote_snr = state_.remote_snr_db ? *state_.remote_snr_db : 0.0f;
    ImGui::Text("%-5.0f %-5.0f %-5.0f", status.snr_db, status.snr_db, remote_snr);

    ImGui::Separator();

    ImGui::Text("QSY");
    static int qsy_slot = 0;
    ImGui::SetNextItemWidth(50);
    ImGui::InputInt("##QSYSlot", &qsy_slot, 0, 0);
    ImGui::SameLine();
    if (ImGui::Button("^")) { qsy_slot++; }
    ImGui::SameLine();
    if (ImGui::Button("#")) { /* random slot */ }

    ImGui::Separator();

    if (ImGui::Button("SEND FILE", ImVec2(-1, 0))) {
        if (state_.current_tab) *state_.current_tab = AppTab::Files;
    }
    if (ImGui::Button("SEND MAIL", ImVec2(-1, 0))) {
        if (state_.current_tab) *state_.current_tab = AppTab::Mail;
    }
    static bool open_broadcast_popup = false;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
    if (ImGui::Button("BROADCAST", ImVec2(-1, 0))) {
        open_broadcast_popup = true;
    }
    ImGui::PopStyleColor();

    const VoiceStatus voice = state_.modem ? state_.modem->getVoiceStatus() : VoiceStatus{};
    const VoiceMode desired_voice_mode =
        state_.config ? configuredVoiceMode(state_.config->voice.mode) : VoiceMode::Off;
    const bool can_start_voice =
        desired_voice_mode != VoiceMode::Off &&
        ((desired_voice_mode == VoiceMode::Connected &&
          status.link_state == LinkState::Connected) ||
         (desired_voice_mode == VoiceMode::Broadcast &&
          status.link_state == LinkState::Idle));
    const bool ptt_available =
        state_.modem &&
        state_.modem->isConnected() &&
        (voice.ptt ||
         voice.state == VoiceState::Opening ||
         voice.state == VoiceState::Ready ||
         voice.state == VoiceState::TxRequested ||
         voice.state == VoiceState::TxActive ||
         voice.state == VoiceState::RxActive ||
         can_start_voice);
    static bool ptt_button_held = false;
    if (!ptt_available && ptt_button_held) {
        ptt_button_held = false;
        if (onVoicePtt) onVoicePtt(false);
    }
    if (!ptt_available) {
        ImGui::BeginDisabled();
    }
    if (voice.ptt || ptt_button_held) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.84f, 0.26f, 0.24f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.89f, 0.32f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.76f, 0.20f, 0.18f, 1.0f));
    }
    ImGui::Button("PTT", ImVec2(-1, 0));
    const bool button_held_now = ptt_available && ImGui::IsItemActive();
    if (button_held_now != ptt_button_held) {
        ptt_button_held = button_held_now;
        if (onVoicePtt) onVoicePtt(ptt_button_held);
    }
    if (voice.ptt || ptt_button_held) {
        ImGui::PopStyleColor(3);
    }
    if (!ptt_available) {
        ImGui::EndDisabled();
    }
    const float indicator_height = ImGui::GetFrameHeight();
    const float remaining_height = ImGui::GetContentRegionAvail().y;
    if (remaining_height > indicator_height) {
        ImGui::Dummy(ImVec2(0.0f, remaining_height - indicator_height));
    }
    renderVoiceActivityIndicator(voice);

    ImGui::EndChild();

    // Broadcast popup
    if (open_broadcast_popup) {
        ImGui::OpenPopup("BroadcastPopup");
        open_broadcast_popup = false;
    }
    if (ImGui::BeginPopupModal("BroadcastPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (state_.broadcast_message) {
            ImGui::Text("Broadcast Message:");
            ImGui::InputTextMultiline("##BroadcastText", state_.broadcast_message,
                                       state_.broadcast_message_size, ImVec2(300, 80));
        }

        ImGui::Separator();
        static bool include_position = false;
        ImGui::Checkbox("Include Position Report", &include_position);
        if (include_position && state_.config) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                "Position: %.4f, %.4f (%s)",
                state_.my_lat ? *state_.my_lat : 0.0,
                state_.my_lon ? *state_.my_lon : 0.0,
                state_.config->station.grid_square.c_str());
            ImGui::TextWrapped("Receiving stations will relay your position to APRS-IS");
        }

        ImGui::Separator();
        if (ImGui::Button("Send", ImVec2(100, 0))) {
            if (include_position && state_.config && state_.my_lat && state_.my_lon) {
                // Build position beacon
                std::vector<uint8_t> packet;
                packet.push_back(0xF4);
                writeDoubleLE(packet, *state_.my_lat);
                writeDoubleLE(packet, *state_.my_lon);
                std::string grid = state_.config->station.grid_square;
                packet.push_back(static_cast<uint8_t>(grid.size()));
                packet.insert(packet.end(), grid.begin(), grid.end());
                std::string msg_text = state_.config->station.callsign;
                if (state_.broadcast_message && std::strlen(state_.broadcast_message) > 0) {
                    msg_text += " ";
                    msg_text += state_.broadcast_message;
                }
                packet.insert(packet.end(), msg_text.begin(), msg_text.end());

                state_.modem->sendBeacon();
                if (onSendData) onSendData(packet);

                state_.addSystemMessage("[POSITION BROADCAST] " + grid + " - " +
                    (state_.broadcast_message ? state_.broadcast_message : ""));
            } else if (state_.broadcast_message && std::strlen(state_.broadcast_message) > 0) {
                if (onSendBroadcast) onSendBroadcast(state_.broadcast_message);
            }
            if (state_.broadcast_message) state_.broadcast_message[0] = '\0';
            include_position = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            include_position = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ========================================================================
    // BOTTOM BUTTONS
    // ========================================================================
    ImGui::Separator();

    if (ImGui::Button("Settings")) {
        if (state_.current_tab) *state_.current_tab = AppTab::Settings;
        if (state_.show_settings) *state_.show_settings = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("BBS")) {
        if (state_.current_tab) *state_.current_tab = AppTab::BBS;
    }
    ImGui::SameLine();
    if (ImGui::Button("Frequency")) {
        if (state_.current_tab) *state_.current_tab = AppTab::Frequency;
    }
    ImGui::SameLine();
    if (ImGui::Button("Messages")) {
        if (state_.current_tab) *state_.current_tab = AppTab::Messages;
    }
    ImGui::SameLine();
    if (ImGui::Button("Position")) {
        if (state_.current_tab) *state_.current_tab = AppTab::Position;
    }
    ImGui::SameLine();
    if (ImGui::Button("Mail")) {
        if (state_.current_tab) *state_.current_tab = AppTab::Mail;
    }
    ImGui::SameLine();
    if (ImGui::Button("Weather")) {
        if (state_.current_tab) *state_.current_tab = AppTab::Weather;
    }
    ImGui::SameLine();
    if (ImGui::Button("Station Map")) {
        if (state_.current_tab) *state_.current_tab = AppTab::StationMap;
    }
}

} // namespace skylink
