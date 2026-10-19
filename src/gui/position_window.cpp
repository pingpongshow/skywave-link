#include "skywave_link/position_window.hpp"
#include "skywave_link/modem_client.hpp"
#include "skywave_link/aprs_client.hpp"
#include "skywave_link/config.hpp"

#include "imgui.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <string>
#include <vector>

namespace skylink {

// ---------------------------------------------------------------------------
// Local helpers (mirrored from app.cpp so position_window is self-contained)
// ---------------------------------------------------------------------------
static void writeDoubleLE(std::vector<uint8_t>& vec, double value) {
    uint8_t bytes[sizeof(double)];
    std::memcpy(bytes, &value, sizeof(double));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    for (size_t i = 0; i < sizeof(double) / 2; ++i) {
        std::swap(bytes[i], bytes[sizeof(double) - 1 - i]);
    }
#endif
    vec.insert(vec.end(), bytes, bytes + sizeof(double));
}

static std::string aprsPasscodeForConfig(const AppConfig& config) {
    if (!config.aprs.passcode.empty()) {
        return config.aprs.passcode;
    }
    if (config.station.callsign.empty()) {
        return "-1";
    }
    return std::to_string(APRSClient::calculatePasscode(config.station.callsign));
}

static bool isStationLinkConnected(const SharedAppState& state) {
    if (!state.modem || !state.modem->isConnected()) {
        return false;
    }
    if (state.modem->getStatus().link_state == LinkState::Connected) {
        return true;
    }
    if (state.current_chat_partner && !state.current_chat_partner->empty()) {
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// PositionWindow
// ---------------------------------------------------------------------------
PositionWindow::PositionWindow(SharedAppState& state)
    : WindowBase(state)
{
}

void PositionWindow::render() {
    const auto& status = state_.modem ? state_.modem->getStatus() : ModemStatus{};
    bool station_connected = isStationLinkConnected(state_);
    std::string remote_callsign = status.remote_callsign.empty()
        ? (state_.current_chat_partner ? *state_.current_chat_partner : std::string{})
        : status.remote_callsign;
    if (remote_callsign.empty()) {
        remote_callsign = "(unknown)";
    }

    ImGui::Text("Position Reporting");
    ImGui::Separator();

    // Current position
    ImGui::Text("My Position:");
    ImGui::InputDouble("Latitude", state_.my_lat, 0.0001, 0.001, "%.6f");
    ImGui::InputDouble("Longitude", state_.my_lon, 0.0001, 0.001, "%.6f");
    ImGui::InputText("Comment", state_.position_comment, state_.position_comment_size);

    if (ImGui::Button("Update Position")) {
        if (onUpdatePosition) {
            onUpdatePosition(*state_.my_lat, *state_.my_lon);
        }
    }

    ImGui::Separator();

    // ========================================================================
    // RADIO POSITION REPORTING (via beacon or connected station)
    // ========================================================================
    ImGui::Text("Radio Position Reporting:");

    if (station_connected) {
        // Connected to a remote station - offer to relay through them
        ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Connected to: %s",
                           remote_callsign.c_str());
        ImGui::TextWrapped("Position can be relayed to APRS-IS through the connected station if they have internet access.");

        if (ImGui::Button("Send Position via Connected Station")) {
            // Send position relay request: 0xF5 | lat(8 bytes double) | lon(8 bytes double) | comment
            std::vector<uint8_t> packet;
            packet.push_back(0xF5);  // Position relay request marker

            // Pack lat/lon as little-endian doubles
            writeDoubleLE(packet, *state_.my_lat);
            writeDoubleLE(packet, *state_.my_lon);

            // Add grid square and comment
            std::string grid = state_.config->station.grid_square;
            packet.push_back(static_cast<uint8_t>(grid.size()));
            packet.insert(packet.end(), grid.begin(), grid.end());

            std::string comment = state_.position_comment;
            packet.insert(packet.end(), comment.begin(), comment.end());

            if (state_.modem) {
                state_.modem->sendData(packet);

                // Add to chat history
                ChatMessage msg;
                msg.from = state_.config->station.callsign;
                msg.text = "[Position relay request sent via " + remote_callsign + " - waiting for relay ACK]";
                msg.timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                msg.is_outgoing = true;
                msg.delivered = true;
                state_.chat_history->push_back(msg);
            }
        }
    } else {
        // Not connected - send as radio beacon
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "Not connected to any station");
        ImGui::TextWrapped("Position will be sent as a radio beacon. Other stations can receive and optionally relay to APRS-IS.");

        if (ImGui::Button("Send Position Beacon")) {
            // Send position beacon: 0xF4 | lat(8) | lon(8) | grid_len | grid | comment
            std::vector<uint8_t> packet;
            packet.push_back(0xF4);  // Position beacon marker

            // Pack lat/lon as little-endian doubles
            writeDoubleLE(packet, *state_.my_lat);
            writeDoubleLE(packet, *state_.my_lon);

            // Add grid square
            std::string grid = state_.config->station.grid_square;
            if (grid.size() > 8) grid = grid.substr(0, 8);
            packet.push_back(static_cast<uint8_t>(grid.size()));
            packet.insert(packet.end(), grid.begin(), grid.end());

            // Add callsign and comment
            std::string msg_text = state_.config->station.callsign;
            if (std::strlen(state_.position_comment) > 0) {
                msg_text += " ";
                msg_text += state_.position_comment;
            }
            packet.insert(packet.end(), msg_text.begin(), msg_text.end());

            if (state_.modem) {
                state_.modem->sendBeacon();
                state_.modem->sendData(packet);

                // Add to chat history
                ChatMessage chat_msg;
                chat_msg.from = state_.config->station.callsign;
                chat_msg.text = "[Position beacon sent: " + grid + "]";
                chat_msg.timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                chat_msg.is_outgoing = true;
                chat_msg.delivered = true;
                state_.chat_history->push_back(chat_msg);
            }
        }
    }

    ImGui::Separator();

    // ========================================================================
    // APRS-IS DIRECT (if internet available locally)
    // ========================================================================
    ImGui::Text("APRS-IS Direct (requires internet):");

    if (ImGui::Button("Send to APRS-IS Now")) {
        if (!state_.aprs) {
            ChatMessage msg;
            msg.from = "SYSTEM";
            msg.text = "APRS-IS send failed: APRS client unavailable";
            msg.is_outgoing = false;
            msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            state_.chat_history->push_back(msg);
            if (state_.chat_scroll_to_bottom) *state_.chat_scroll_to_bottom = true;
        } else {
            bool queued_for_login = false;
            if (state_.aprs->getState() != APRSState::Connected) {
                if (!state_.config->aprs.enabled) {
                    ChatMessage msg;
                    msg.from = "SYSTEM";
                    msg.text = "APRS-IS send failed: APRS is disabled in Settings";
                    msg.is_outgoing = false;
                    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    state_.chat_history->push_back(msg);
                    if (state_.chat_scroll_to_bottom) *state_.chat_scroll_to_bottom = true;
                } else {
                    // Log in in the background (never on the GUI thread);
                    // the App reports when the queued beacon goes out.
                    state_.aprs->setMyPosition(*state_.my_lat, *state_.my_lon,
                                               state_.config->station.altitude_m);
                    state_.aprs->setMyComment(std::strlen(state_.position_comment) > 0
                                                  ? std::string(state_.position_comment)
                                                  : state_.config->aprs.comment);
                    if (!state_.aprs->isConnecting()) {
                        state_.aprs->beginConnect(state_.config->aprs.server,
                                                  state_.config->aprs.port,
                                                  state_.config->station.callsign,
                                                  aprsPasscodeForConfig(*state_.config));
                    }
                    state_.aprs->sendPositionWhenConnected();
                    state_.addSystemMessage("APRS-IS connecting; the position beacon is sent once logged in");
                    queued_for_login = true;
                }
            }

            if (queued_for_login) {
                // Sent by the App once the background login completes.
            } else if (state_.aprs->getState() == APRSState::Connected) {
                state_.aprs->setMyPosition(*state_.my_lat, *state_.my_lon,
                                           state_.config->station.altitude_m);
                if (std::strlen(state_.position_comment) > 0) {
                    state_.aprs->setMyComment(state_.position_comment);
                } else {
                    state_.aprs->setMyComment(state_.config->aprs.comment);
                }
                state_.aprs->sendPositionNow();

                ChatMessage msg;
                msg.from = "SYSTEM";
                msg.text = "APRS-IS position beacon sent";
                msg.is_outgoing = false;
                msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                state_.chat_history->push_back(msg);
                if (state_.chat_scroll_to_bottom) *state_.chat_scroll_to_bottom = true;
            } else {
                ChatMessage msg;
                msg.from = "SYSTEM";
                msg.text = "APRS-IS send failed: Not connected/authenticated";
                msg.is_outgoing = false;
                msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                state_.chat_history->push_back(msg);
                if (state_.chat_scroll_to_bottom) *state_.chat_scroll_to_bottom = true;
            }
        }
    }

    if (state_.aprs) {
        ImGui::SameLine();
        auto aprs_state = state_.aprs->getState();
        if (aprs_state == APRSState::Connected) {
            ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "APRS-IS Connected");
        } else if (aprs_state == APRSState::Connecting) {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "Connecting...");
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.2f, 1.0f), "APRS-IS Disconnected");
        }

        bool beaconing = state_.aprs->isBeaconingEnabled();
        if (ImGui::Checkbox("Enable Auto Beaconing", &beaconing)) {
            state_.aprs->enableBeaconing(beaconing);
        }
    } else {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "APRS client not initialized");
    }
}

} // namespace skylink
