// ChatWindow - Real-time chat with typing indicators and CQ

#include "skywave_link/chat_window.hpp"
#include "skywave_link/modem_client.hpp"
#include "imgui.h"
#include <cstring>
#include <chrono>

namespace skylink {

static constexpr int TYPING_INDICATOR_TIMEOUT_MS = 3000;

ChatWindow::ChatWindow(SharedAppState& state) : WindowBase(state) {}

void ChatWindow::render() {
    if (!state_.chat_history || !state_.current_chat_partner) return;
    auto& chat_history = *state_.chat_history;
    auto& current_partner = *state_.current_chat_partner;

    // Check typing indicator timeout
    auto now = std::chrono::steady_clock::now();
    if (state_.remote_is_typing && *state_.remote_is_typing && state_.last_typing_indicator) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - *state_.last_typing_indicator);
        if (elapsed.count() > TYPING_INDICATOR_TIMEOUT_MS) {
            *state_.remote_is_typing = false;
        }
    }

    // Two-column layout: Chat on left, Heard Stations on right
    ImGui::Columns(2, "ChatColumns", true);
    ImGui::SetColumnWidth(0, ImGui::GetWindowWidth() * 0.7f);

    // === LEFT COLUMN: Chat ===
    float footer_height = ImGui::GetFrameHeightWithSpacing() * 2 + 40;
    ImVec2 size = ImVec2(-1, -footer_height);

    if (ImGui::BeginChild("ChatHistory", size, true)) {
        for (const auto& msg : chat_history) {
            if (msg.is_outgoing) {
                ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "[%s] ", msg.from.c_str());
            } else {
                ImGui::TextColored(ImVec4(0.7f, 1.0f, 0.7f, 1.0f), "[%s] ", msg.from.c_str());
            }
            ImGui::SameLine();
            ImGui::TextWrapped("%s", msg.text.c_str());
        }

        if (state_.remote_is_typing && *state_.remote_is_typing) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s is typing...",
                               current_partner.c_str());
        }

        if (state_.chat_scroll_to_bottom && *state_.chat_scroll_to_bottom) {
            ImGui::SetScrollHereY(1.0f);
            *state_.chat_scroll_to_bottom = false;
        }
    }
    ImGui::EndChild();

    ImGui::Separator();

    // Connection status and CQ buttons
    bool station_connected = state_.modem &&
        state_.modem->getStatus().link_state == LinkState::Connected;
    if (station_connected) {
        ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Connected to %s",
                           current_partner.c_str());
    } else {
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "Not connected");
        ImGui::SameLine(200);
        if (state_.cq_active && *state_.cq_active) {
            if (ImGui::Button("Stop CQ")) {
                if (onStopCQ) onStopCQ();
            }
            ImGui::SameLine();
            ImGui::Text("Repeat %d/%d",
                        state_.cq_current_repeat ? *state_.cq_current_repeat : 0,
                        state_.cq_repeat_count ? *state_.cq_repeat_count : 0);
        } else {
            if (ImGui::Button("Call CQ")) {
                if (onStartCQ) onStartCQ();
            }
        }
    }

    bool send_pressed = false;
    ImGui::PushItemWidth(-80);
    if (ImGui::InputText("##ChatInput", state_.chat_input, state_.chat_input_size,
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        send_pressed = true;
    }

    // Send typing indicator if input changed
    bool input_empty = (std::strlen(state_.chat_input) == 0);
    if (!input_empty && state_.last_input_was_empty && *state_.last_input_was_empty) {
        if (onSendTypingIndicator) onSendTypingIndicator();
    }
    if (state_.last_input_was_empty) *state_.last_input_was_empty = input_empty;

    ImGui::PopItemWidth();

    ImGui::SameLine();
    if (ImGui::Button("Send", ImVec2(70, 0)) || send_pressed) {
        if (std::strlen(state_.chat_input) > 0) {
            if (onSendMessage) onSendMessage(state_.chat_input);
            state_.chat_input[0] = '\0';
            if (state_.last_input_was_empty) *state_.last_input_was_empty = true;
        }
    }

    // === RIGHT COLUMN: Heard Stations & Broadcast ===
    ImGui::NextColumn();

    ImGui::Text("Heard Stations");
    ImGui::Separator();

    if (ImGui::BeginChild("HeardStations", ImVec2(-1, 200), true)) {
        if (!state_.heard_stations || state_.heard_stations->empty()) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No stations heard yet");
        } else {
            for (const auto& [callsign, timestamp] : *state_.heard_stations) {
                if (ImGui::Selectable(callsign.c_str())) {
                    if (onConnectToStation) onConnectToStation(callsign);
                }
                ImGui::SameLine(120);
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", timestamp.c_str());
            }
        }
    }
    ImGui::EndChild();

    if (ImGui::Button("Clear List")) {
        if (state_.heard_stations) state_.heard_stations->clear();
    }

    ImGui::Separator();
    ImGui::Text("Broadcast");

    ImGui::PushItemWidth(-1);
    if (state_.broadcast_message) {
        ImGui::InputText("##BroadcastMsg", state_.broadcast_message, state_.broadcast_message_size);
    }
    ImGui::PopItemWidth();

    if (ImGui::Button("Send Broadcast", ImVec2(-1, 0))) {
        if (state_.broadcast_message && std::strlen(state_.broadcast_message) > 0) {
            if (onSendBroadcast) onSendBroadcast(state_.broadcast_message);
            state_.broadcast_message[0] = '\0';
        }
    }

    // Ping section
    ImGui::Separator();
    ImGui::Text("Status Check (Ping)");

    static char ping_callsign_input[32] = "";
    ImGui::PushItemWidth(-70);
    ImGui::InputText("##PingCall", ping_callsign_input, sizeof(ping_callsign_input));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Ping", ImVec2(-1, 0))) {
        if (std::strlen(ping_callsign_input) > 0 && onSendPing) {
            onSendPing(ping_callsign_input);
        }
    }
    if (state_.ping_in_progress && *state_.ping_in_progress) {
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "Waiting for response...");
    }

    // CQ settings
    ImGui::Separator();
    ImGui::Text("CQ Settings");
    if (state_.cq_repeat_count) {
        ImGui::SliderInt("Repeat Count", state_.cq_repeat_count, 1, 10);
    }

    ImGui::Columns(1);
}

} // namespace skylink
