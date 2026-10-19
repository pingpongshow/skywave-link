#pragma once
#include "window_base.hpp"
#include <cstdint>
#include <functional>
#include <vector>

namespace skylink {
class PositionWindow : public WindowBase {
public:
    explicit PositionWindow(SharedAppState& state);
    void render() override;
    const char* name() const override { return "Position"; }

    std::function<void(double, double)> onUpdatePosition;
    // APRS-IS session control. Unset, the window drives the APRSClient
    // directly (background login, polled by App).
    std::function<void()> onAPRSConnect;
    std::function<void()> onAPRSDisconnect;
    // "Enable Auto Beaconing" changed: App stores it in config (G14/K).
    std::function<void(bool)> onAutoBeaconChanged;
    // BCAST while idle, session data while linked (App::sendBroadcastPayload).
    std::function<bool(const std::vector<uint8_t>&)> onSendBroadcastPayload;
    // App's link check (App::isStationLinkConnected). Unset: the modem's
    // link state.
    std::function<bool(bool refresh)> onCheckStationLink;
    // Session data to the linked station only; false if not sent.
    // Unset: onSendBroadcastPayload while the modem reports a link.
    std::function<bool(const std::vector<uint8_t>&)> onSendLinkPayload;

private:
    bool stationLinked(bool refresh) const;
    bool checkPositionUsable(const char* what);
    void sendRelayRequest(const std::string& remote_callsign);
    void sendPositionBeacon();
    void sendToAprsIs();
    void addOwnLine(const std::string& text);
};
}
