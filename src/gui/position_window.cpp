#include "skywave_link/position_window.hpp"
#include "skywave_link/aprs_client.hpp"
#include "skywave_link/config.hpp"
#include "skywave_link/gui_util.hpp"
#include "skywave_link/modem_client.hpp"
#include "skywave_link/modern_style.hpp"

#include "imgui.h"
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace skylink {

PositionWindow::PositionWindow(SharedAppState& state)
    : WindowBase(state)
{
}

bool PositionWindow::stationLinked(bool refresh) const {
    if (onCheckStationLink) {
        return onCheckStationLink(refresh);
    }
    return gui::isStationLinked(state_.modem);
}

bool PositionWindow::checkPositionUsable(const char* what) {
    const double lat = state_.my_lat ? *state_.my_lat : 0.0;
    const double lon = state_.my_lon ? *state_.my_lon : 0.0;
    if (gui::isUsablePosition(lat, lon)) {
        return true;
    }
    postStatus(std::string(what) + " not sent: no position yet (enter it above or wait for a "
                                   "GPS fix)",
               StatusKind::Error);
    return false;
}

// A line in the chat as this station's own transmission.
void PositionWindow::addOwnLine(const std::string& text) {
    if (!state_.chat_history || !state_.config) return;
    ChatMessage msg;
    msg.from = state_.config->station.callsign;
    msg.text = text;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    msg.is_outgoing = true;
    msg.band = state_.current_band ? *state_.current_band : std::string();
    state_.chat_history->push_back(msg);
    if (state_.chat_scroll_to_bottom) *state_.chat_scroll_to_bottom = true;
}

void PositionWindow::sendRelayRequest(const std::string& remote_callsign) {
    if (!checkPositionUsable("Position relay request")) return;
    if (!stationLinked(true)) {
        postStatus("Position relay request not sent: no station link", StatusKind::Error);
        return;
    }
    const auto packet = gui::buildPositionRelayPacket(*state_.my_lat, *state_.my_lon,
                                                      state_.config->station.grid_square,
                                                      state_.position_comment);
    bool sent = false;
    const size_t log_mark = state_.chatHistorySize();
    if (onSendLinkPayload) {
        sent = onSendLinkPayload(packet);
    } else if (onSendBroadcastPayload && gui::isStationLinked(state_.modem)) {
        // App::sendBroadcastPayload sends session data while linked.
        sent = onSendBroadcastPayload(packet);
    }
    if (sent) {
        postStatus("Relay request sent to " + remote_callsign + "; it reports when the position "
                   "reaches APRS-IS",
                   StatusKind::Success);
        addOwnLine("[Position relay request sent via " + remote_callsign + "]");
    } else {
        const std::string reason = state_.systemMessageSince(log_mark);
        postStatus(reason.empty() ? "Position relay request not sent: the modem refused the data"
                                  : reason,
                   StatusKind::Error);
    }
}

void PositionWindow::sendPositionBeacon() {
    if (!checkPositionUsable("Position beacon")) return;
    if (!onSendBroadcastPayload) {
        postStatus("Position beacon not available", StatusKind::Error);
        return;
    }
    if (!state_.modem || !state_.modem->isConnected()) {
        postStatus("Position beacon not sent: the modem is not connected", StatusKind::Error);
        return;
    }
    const auto packet = gui::buildPositionBeaconPacket(*state_.my_lat, *state_.my_lon,
                                                       state_.config->station.grid_square,
                                                       state_.config->station.callsign,
                                                       state_.position_comment);
    const size_t log_mark = state_.chatHistorySize();
    if (onSendBroadcastPayload(packet)) {
        std::string grid = gui::trimCopy(state_.config->station.grid_square);
        if (grid.size() > gui::kMaxPositionGridLen) grid.resize(gui::kMaxPositionGridLen);
        postStatus("Position beacon queued for transmission", StatusKind::Success);
        addOwnLine("[Position beacon sent: " + (grid.empty() ? std::string("no grid") : grid) + "]");
    } else {
        const std::string reason = state_.systemMessageSince(log_mark);
        postStatus(reason.empty() ? "Position beacon not sent: the modem refused it" : reason,
                   StatusKind::Error);
    }
}

void PositionWindow::sendToAprsIs() {
    if (!state_.aprs) {
        postStatus("APRS-IS send failed: APRS client unavailable", StatusKind::Error);
        return;
    }
    if (!checkPositionUsable("APRS-IS position")) return;
    if (!state_.config->aprs.enabled) {
        postStatus("APRS-IS send failed: APRS is disabled in Settings", StatusKind::Error);
        return;
    }
    if (state_.config->station.callsign.empty()) {
        postStatus("APRS-IS send failed: set your callsign in Settings", StatusKind::Error);
        return;
    }

    state_.aprs->setMyPosition(*state_.my_lat, *state_.my_lon, state_.config->station.altitude_m);
    state_.aprs->setMyComment(std::strlen(state_.position_comment) > 0
                                  ? std::string(state_.position_comment)
                                  : state_.config->aprs.comment);

    if (state_.aprs->getState() == APRSState::Connected) {
        if (state_.aprs->sendPositionNow()) {
            postStatus("APRS-IS position beacon sent", StatusKind::Success);
            state_.addSystemMessage("APRS-IS position beacon sent");
        } else {
            postStatus("APRS-IS send failed: the server connection dropped", StatusKind::Error);
            state_.addSystemMessage("APRS-IS send failed: the server connection dropped");
        }
        return;
    }

    // Log in in the background (never on the GUI thread); App reports when
    // the queued beacon goes out.
    if (!state_.aprs->isConnecting() &&
        !state_.aprs->beginConnect(state_.config->aprs.server, state_.config->aprs.port,
                                   state_.config->station.callsign,
                                   APRSClient::effectivePasscode(state_.config->aprs.passcode,
                                                                   state_.config->station.callsign))) {
        const std::string why = state_.aprs->lastConnectError();
        postStatus("APRS-IS connect failed" + (why.empty() ? std::string() : ": " + why),
                   StatusKind::Error);
        return;
    }
    state_.aprs->sendPositionWhenConnected();
    postStatus("Connecting to APRS-IS; the position is sent once logged in", StatusKind::Info);
}

void PositionWindow::render() {
    const ui::Palette& pal = ui::palette();
    const ModemStatus status = state_.modem ? state_.modem->getStatus() : ModemStatus{};
    const bool station_connected = stationLinked(false);
    std::string remote_callsign = status.remote_callsign.empty()
        ? (state_.current_chat_partner ? *state_.current_chat_partner : std::string{})
        : status.remote_callsign;
    if (remote_callsign.empty()) {
        remote_callsign = "(unknown)";
    }

    ImGui::Text("Position Reporting");
    ImGui::Separator();
    ui::StatusLine(status_);

    // Current position
    ImGui::Text("My Position:");
    ImGui::InputDouble("Latitude", state_.my_lat, 0.0001, 0.001, "%.6f");
    ImGui::InputDouble("Longitude", state_.my_lon, 0.0001, 0.001, "%.6f");
    ImGui::InputText("Comment", state_.position_comment, state_.position_comment_size);
    const bool have_position = gui::isUsablePosition(*state_.my_lat, *state_.my_lon);
    if (!have_position) {
        ImGui::TextColored(pal.warning, "No position set: position reports are disabled.");
    }

    if (ImGui::Button("Update Position")) {
        if (*state_.my_lat < -90.0 || *state_.my_lat > 90.0 ||
            *state_.my_lon < -180.0 || *state_.my_lon > 180.0) {
            postStatus("Latitude must be -90..90 and longitude -180..180", StatusKind::Error);
        } else if (onUpdatePosition) {
            onUpdatePosition(*state_.my_lat, *state_.my_lon);
            postStatus("Position updated", StatusKind::Success);
        }
    }

    ImGui::Separator();

    // ========================================================================
    // RADIO POSITION REPORTING (via beacon or connected station)
    // ========================================================================
    ImGui::Text("Radio Position Reporting:");

    if (station_connected) {
        // Connected to a remote station - offer to relay through them
        ImGui::TextColored(pal.success, "Connected to: %s", remote_callsign.c_str());
        ImGui::TextWrapped("Position can be relayed to APRS-IS through the connected station if they have internet access.");

        ImGui::BeginDisabled(!have_position);
        if (ImGui::Button("Send Position via Connected Station")) {
            sendRelayRequest(remote_callsign);
        }
        ImGui::EndDisabled();
    } else {
        // Not connected - send as radio beacon
        ImGui::TextColored(pal.warning, "Not connected to any station");
        ImGui::TextWrapped("Position will be sent as a radio beacon. Other stations can receive and optionally relay to APRS-IS.");

        ImGui::BeginDisabled(!have_position);
        if (ImGui::Button("Send Position Beacon")) {
            sendPositionBeacon();
        }
        ImGui::EndDisabled();
    }

    ImGui::Separator();

    // ========================================================================
    // APRS-IS DIRECT (if internet available locally)
    // ========================================================================
    ImGui::Text("APRS-IS Direct (requires internet):");

    ImGui::BeginDisabled(!have_position);
    if (ImGui::Button("Send to APRS-IS Now")) {
        sendToAprsIs();
    }
    ImGui::EndDisabled();

    if (state_.aprs) {
        ImGui::SameLine();
        const auto aprs_state = state_.aprs->getState();
        const bool aprs_busy = state_.aprs->isConnecting() || aprs_state == APRSState::Connecting;
        if (aprs_state == APRSState::Connected) {
            ImGui::TextColored(pal.success, "APRS-IS Connected");
        } else if (aprs_busy) {
            ImGui::TextColored(pal.warning, "Connecting...");
        } else {
            ImGui::TextColored(pal.danger, "APRS-IS Disconnected");
            const std::string& why = state_.aprs->lastConnectError();
            if (!why.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, pal.danger);
                ImGui::TextWrapped("Last connect error: %s", why.c_str());
                ImGui::PopStyleColor();
            }
        }

        if (aprs_state == APRSState::Connected || aprs_busy) {
            if (ImGui::Button("Disconnect APRS-IS")) {
                if (onAPRSDisconnect) {
                    onAPRSDisconnect();
                } else {
                    if (state_.aprs->isConnecting()) state_.aprs->cancelConnect();
                    state_.aprs->disconnect();
                }
                postStatus("APRS-IS disconnected", StatusKind::Info);
            }
        } else {
            ImGui::BeginDisabled(!state_.config->aprs.enabled);
            if (ImGui::Button("Connect APRS-IS")) {
                if (onAPRSConnect) {
                    onAPRSConnect();
                    postStatus("Connecting to APRS-IS...", StatusKind::Info);
                } else if (state_.config->station.callsign.empty()) {
                    postStatus("Set your callsign in Settings first", StatusKind::Error);
                } else if (state_.aprs->beginConnect(state_.config->aprs.server,
                                                     state_.config->aprs.port,
                                                     state_.config->station.callsign,
                                                     APRSClient::effectivePasscode(state_.config->aprs.passcode,
                                                                   state_.config->station.callsign))) {
                    postStatus("Connecting to APRS-IS...", StatusKind::Info);
                } else {
                    postStatus("APRS-IS connect failed: " + state_.aprs->lastConnectError(),
                               StatusKind::Error);
                }
            }
            ImGui::EndDisabled();
            if (!state_.config->aprs.enabled) {
                ImGui::SameLine();
                ImGui::TextDisabled("(enable APRS in Settings)");
            }
        }

        bool beaconing = state_.aprs->isBeaconingEnabled();
        if (ImGui::Checkbox("Enable Auto Beaconing", &beaconing)) {
            state_.aprs->enableBeaconing(beaconing);
            if (onAutoBeaconChanged) {
                onAutoBeaconChanged(beaconing);
            } else {
                postStatus(std::string("Auto beaconing ") + (beaconing ? "on" : "off") +
                               " for this session (not saved)",
                           StatusKind::Warning);
            }
        }
    } else {
        ImGui::SameLine();
        ImGui::TextDisabled("APRS client not initialized");
    }
}

} // namespace skylink
