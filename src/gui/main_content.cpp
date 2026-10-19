// MainContentRenderer - Control panel, three-panel layout, right panel, bottom buttons

#include "skywave_link/main_content.hpp"
#include "skywave_link/modem_client.hpp"
#include "skywave_link/modern_style.hpp"
#include "imgui.h"
#include <cstring>
#include <ctime>
#include <chrono>
#include <cstdio>
#include <algorithm>

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
            button_color = ui::palette().danger;
        }
    } else if (rx_active) {
        label = "VOICE RX";
        if (flash_on) {
            button_color = ui::palette().success;
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

    const ui::Palette& pal = ui::palette();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float em = ImGui::GetFontSize();

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

    // The pending-message count is a database query; once a second is
    // plenty for a status counter (it used to run every frame).
    static int pending = 0;
    static double pending_refreshed_at = -1.0;
    if (pending_refreshed_at < 0.0 || ImGui::GetTime() - pending_refreshed_at >= 1.0) {
        pending = state_.store ? state_.store->countPendingMessages() : 0;
        pending_refreshed_at = ImGui::GetTime();
    }

    // ========================================================================
    // CONTROL PANEL
    // ========================================================================
    ImGui::BeginChild("ControlPanel", ImVec2(-1, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(pal.heading, "FREQUENCY");
    ImGui::SameLine();

    int current_idx = 1;
    for (int i = 0; i < 8; i++) {
        if (state_.current_band && *state_.current_band == band_freqs[i].band) {
            current_idx = i;
            break;
        }
    }

    ImGui::SetNextItemWidth(em * 11.0f);
    if (ImGui::BeginCombo("##Freq", band_freqs[current_idx].display)) {
        for (int i = 0; i < 8; i++) {
            if (ImGui::Selectable(band_freqs[i].display, current_idx == i)) {
                if (state_.current_band) *state_.current_band = band_freqs[i].band;
                if (onSetFrequency) onSetFrequency(band_freqs[i].freq_hz);
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine(0.0f, em * 2.0f);
    ImGui::TextColored(pal.heading, "CONNECT");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 8.0f);
    ImGui::InputTextWithHint("##ConnectTarget", "Callsign", connect_target, sizeof(connect_target),
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
    ui::PushButtonColor(status.ptt_active ? pal.danger : pal.success);
    ImGui::Button(status.ptt_active ? " TX " : " RX ");
    ui::PopButtonColor();
    ImGui::SameLine();

    int mail_count = state_.mail_inbox ? static_cast<int>(state_.mail_inbox->size()) : 0;
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%d/%d", pending, mail_count);

    ImGui::SameLine(0.0f, em * 1.5f);

    // CQ controls
    bool cq_active = state_.cq_active && *state_.cq_active;
    if (cq_active) {
        ui::PushButtonColor(pal.warning);
        if (ImGui::Button("END CQ")) {
            if (onStopCQ) onStopCQ();
        }
        ui::PopButtonColor();
    } else {
        if (ImGui::Button("CALL CQ")) {
            if (onStartCQ) onStartCQ();
        }
    }
    ImGui::SameLine();

    bool beacon_enabled = state_.beacon_enabled && *state_.beacon_enabled;
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
    int unread_files = state_.unread_file_notifications ? *state_.unread_file_notifications : 0;
    size_t beacon_count = state_.heard_beacons ? state_.heard_beacons->size() : 0;

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
    std::snprintf(outbox_txt, sizeof(outbox_txt), "Outbox: %d", pending);
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
        ImGui::Button(mail_btn);
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

    // ========================================================================
    // THREE-PANEL LAYOUT
    // ========================================================================
    float content_width = ImGui::GetContentRegionAvail().x;
    // The heard lists get up to ~6 rows but give way to the chat area and
    // the right-hand controls on short windows.
    const float row_padding = style.WindowPadding.y * 2.0f;
    float panel_row_height = ImGui::GetTextLineHeightWithSpacing() * 6.5f + row_padding;
    const float panel_row_cap = ImGui::GetContentRegionAvail().y * 0.24f;
    panel_row_height = std::max(ImGui::GetTextLineHeightWithSpacing() * 4.0f + row_padding,
                                std::min(panel_row_height, panel_row_cap));
    float panel_width = (content_width - style.ItemSpacing.x * 2.0f) / 3.0f;
    float right_panel_width = em * 12.0f;

    const ImGuiTableFlags heard_table_flags =
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_NoPadOuterX;

    // Log panel (left)
    ImGui::BeginChild("LogPanel", ImVec2(panel_width, panel_row_height), ImGuiChildFlags_Borders);
    ui::SectionHeader("Skywave Link Log");
    if (state_.chat_history) {
        ImGui::PushFont(ui::monoFont(), 0.0f);
        int count = 0;
        for (auto it = state_.chat_history->rbegin();
             it != state_.chat_history->rend() && count < 8; ++it) {
            if (it->from == "SYSTEM") {
                auto time_t_val = static_cast<time_t>(it->timestamp);
                char time_str[16];
                std::strftime(time_str, sizeof(time_str), "%H:%M:%S", std::localtime(&time_t_val));
                ImGui::TextColored(pal.muted, "%s", time_str);
                ImGui::SameLine();
                ImGui::TextUnformatted(it->text.c_str());
                count++;
            }
        }
        ImGui::PopFont();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Last heard beacons panel (center)
    ImGui::BeginChild("HeardBeaconsPanel", ImVec2(panel_width, panel_row_height), ImGuiChildFlags_Borders);
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
                std::string band = state_.current_band ? *state_.current_band : "20m";
                ImGui::TableNextColumn();
                ImGui::Text("%s", band.c_str());
                auto time_t_val = static_cast<time_t>(beacon.timestamp);
                char time_str[16];
                std::strftime(time_str, sizeof(time_str), "%H:%M", std::localtime(&time_t_val));
                ImGui::TableNextColumn();
                ImGui::Text("%s", time_str);
                ImGui::TableNextColumn();
                if (beacon.has_pending_messages) {
                    ImGui::TextColored(pal.warning, "%s", beacon.callsign.c_str());
                } else if (beacon.email_gateway) {
                    ImGui::TextColored(pal.info, "%s @", beacon.callsign.c_str());
                } else {
                    ImGui::Text("%s", beacon.callsign.c_str());
                }
                if (beacon.email_gateway && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Email gateway capable station");
                }
                ImGui::TableNextColumn();
                ImGui::Text("2300");
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", beacon.snr_db);
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Last heard CQ calls panel (right)
    ImGui::BeginChild("CQCallsPanel", ImVec2(0, panel_row_height), ImGuiChildFlags_Borders);
    ui::SectionHeader("Last heard CQ calls");
    if (ImGui::BeginTable("CQCols", 5, heard_table_flags)) {
        ImGui::TableSetupColumn("Bnd", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Call", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableSetupColumn("BW", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableSetupColumn("SNR", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableHeadersRow();
        if (state_.heard_stations) {
            int count = 0;
            for (const auto& [callsign, timestamp] : *state_.heard_stations) {
                if (count++ > 5) break;
                ImGui::TableNextRow();
                std::string band = state_.current_band ? *state_.current_band : "20m";
                ImGui::TableNextColumn();
                ImGui::Text("%s", band.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%s", timestamp.c_str());
                ImGui::TableNextColumn();
                ImGui::PushID(count);
                if (ImGui::Selectable(callsign.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
                    std::strncpy(connect_target, callsign.c_str(), sizeof(connect_target) - 1);
                    connect_target[sizeof(connect_target) - 1] = '\0';
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::Text("2300");
                ImGui::TableNextColumn();
                ImGui::Text("-");
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    // ========================================================================
    // ROW 2: Main message area + Right control panel
    // ========================================================================
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
            if (!t.completed && !t.failed) has_transfers = true;
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

    // Main chat area
    float main_chat_width = content_width - right_panel_width - style.ItemSpacing.x;
    ImGui::BeginChild("MainChatArea", ImVec2(main_chat_width, main_area_height), ImGuiChildFlags_None);

    float chat_height = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginChild("MainChat", ImVec2(-1, chat_height), ImGuiChildFlags_Borders)) {
        const ImGuiTableFlags chat_flags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                                           ImGuiTableFlags_NoPadOuterX;
        if (ImGui::BeginTable("ChatTable", 5, chat_flags)) {
            ImGui::TableSetupColumn("Bnd", ImGuiTableColumnFlags_WidthFixed, em * 2.6f);
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, em * 3.0f);
            ImGui::TableSetupColumn("From", ImGuiTableColumnFlags_WidthFixed, em * 5.5f);
            ImGui::TableSetupColumn("To", ImGuiTableColumnFlags_WidthFixed, em * 5.5f);
            ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            if (state_.chat_history) {
                std::string band = state_.current_band ? *state_.current_band : "20m";
                std::string my_call = state_.config ? state_.config->station.callsign : "";
                std::string partner = state_.current_chat_partner ? *state_.current_chat_partner : "";

                for (const auto& msg : *state_.chat_history) {
                    if (msg.from == "SYSTEM") continue;

                    auto time_t_val = static_cast<time_t>(msg.timestamp);
                    char time_str[16];
                    std::strftime(time_str, sizeof(time_str), "%H:%M", std::localtime(&time_t_val));

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(pal.muted, "%s", band.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextColored(pal.muted, "%s", time_str);
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
                    ImGui::Text("%s", to_call.substr(0, 10).c_str());
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
    // No scrollbar (it would only show for a few pixels of overflow on short
    // windows); the wheel still scrolls if the controls ever do not fit.
    ImGui::BeginChild("RightPanel", ImVec2(right_panel_width, main_area_height), ImGuiChildFlags_Borders,
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
    static double snr_sum = 0.0;
    static int snr_samples = 0;
    static float snr_last_sampled = -1000.0f;
    const bool linked = status.link_state == LinkState::Connected;
    if (!linked) {
        snr_sum = 0.0;
        snr_samples = 0;
        snr_last_sampled = -1000.0f;
    } else if (status.snr_db != snr_last_sampled) {
        // Sample on change (EVT:SNR / STATUS), not per frame.
        snr_sum += status.snr_db;
        ++snr_samples;
        snr_last_sampled = status.snr_db;
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
        if (snr_samples > 0) {
            ImGui::Text("%.0f", snr_sum / snr_samples);
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

    ui::SectionHeader("Actions");

    if (ImGui::Button("SEND FILE", ImVec2(-1, 0))) {
        if (state_.current_tab) *state_.current_tab = AppTab::Files;
    }
    if (ImGui::Button("SEND MAIL", ImVec2(-1, 0))) {
        if (state_.current_tab) *state_.current_tab = AppTab::Mail;
    }
    static bool open_broadcast_popup = false;
    ui::PushButtonColor(pal.danger);
    if (ImGui::Button("BROADCAST", ImVec2(-1, 0))) {
        open_broadcast_popup = true;
    }
    ui::PopButtonColor();

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
    const bool ptt_lit = voice.ptt || ptt_button_held;
    if (ptt_lit) {
        ui::PushButtonColor(pal.danger);
    }
    ImGui::Button("PTT", ImVec2(-1, 0));
    const bool button_held_now = ptt_available && ImGui::IsItemActive();
    if (button_held_now != ptt_button_held) {
        ptt_button_held = button_held_now;
        if (onVoicePtt) onVoicePtt(ptt_button_held);
    }
    if (ptt_lit) {
        ui::PopButtonColor();
    }
    if (!ptt_available) {
        ImGui::EndDisabled();
    }
    const float indicator_height = ImGui::GetFrameHeight();
    const float remaining_height = ImGui::GetContentRegionAvail().y;
    if (remaining_height > indicator_height + style.ItemSpacing.y) {
        ImGui::Dummy(ImVec2(0.0f, remaining_height - indicator_height - style.ItemSpacing.y));
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
                                       state_.broadcast_message_size, ImVec2(em * 22.0f, em * 6.0f));
        }

        ImGui::Separator();
        static bool include_position = false;
        ImGui::Checkbox("Include Position Report", &include_position);
        if (include_position && state_.config) {
            ImGui::TextColored(pal.muted,
                "Position: %.4f, %.4f (%s)",
                state_.my_lat ? *state_.my_lat : 0.0,
                state_.my_lon ? *state_.my_lon : 0.0,
                state_.config->station.grid_square.c_str());
            ImGui::TextWrapped("Receiving stations will relay your position to APRS-IS");
        }

        ImGui::Separator();
        ui::PushButtonColor(pal.danger);
        const bool send_broadcast = ImGui::Button("Send", ImVec2(em * 7.0f, 0));
        ui::PopButtonColor();
        if (send_broadcast) {
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

                if (onSendBroadcastPayload && onSendBroadcastPayload(packet)) {
                    state_.addSystemMessage("[POSITION BROADCAST] " + grid + " - " +
                        (state_.broadcast_message ? state_.broadcast_message : ""));
                } else {
                    state_.addSystemMessage("Position broadcast not sent: the modem is not connected");
                }
            } else if (state_.broadcast_message && std::strlen(state_.broadcast_message) > 0) {
                if (onSendBroadcast) onSendBroadcast(state_.broadcast_message);
            }
            if (state_.broadcast_message) state_.broadcast_message[0] = '\0';
            include_position = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(em * 7.0f, 0))) {
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
