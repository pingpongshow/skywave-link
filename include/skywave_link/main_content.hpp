// MainContentRenderer - Control panel, three-panel layout, right panel, bottom buttons
#pragma once

#include "window_base.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace skylink {

class MainContentRenderer : public WindowBase {
public:
    explicit MainContentRenderer(SharedAppState& state);

    void render() override;
    const char* name() const override { return "MainContent"; }

    // Action callbacks
    std::function<void()> onConnectModem;
    std::function<void()> onDisconnectModem;
    std::function<void(const std::string&)> onConnectToStation;
    std::function<void(const std::string&)> onSendPing;
    std::function<void()> onAbort;
    std::function<void()> onStartCQ;
    std::function<void()> onStopCQ;
    std::function<void()> onSendBeacon;
    std::function<void(bool)> onVoicePtt;
    // Sends a chat line. The input is cleared only when the line was sent
    // (an outgoing chat entry appeared); otherwise it stays for a retry.
    std::function<void(const std::string&)> onSendChatMessage;
    std::function<void(const std::string&)> onSendBroadcast;
    // BCAST while idle, session data while linked (App::sendBroadcastPayload).
    std::function<bool(const std::vector<uint8_t>&)> onSendBroadcastPayload;
    // Tunes the rig (App::tuneOperatingFrequency(hz, false)); false when the
    // radio did not move. Unset: RadioController::setFrequency.
    std::function<bool(uint64_t)> onTuneFrequency;

private:
    void renderControlPanel(bool station_connected);
    void renderHeardPanels(float panel_width, float panel_height);
    void renderChatArea(float width, float height);
    void renderRightPanel(float width, float height);
    void renderBroadcastPopup();
    void renderBottomButtons();
    bool tuneBand(const std::string& band, uint64_t freq_hz);
    void sendChatInput();

    char connect_target_[32] = "";
    bool open_broadcast_popup_ = false;
    bool include_position_ = false;
    bool ptt_button_held_ = false;
    // Session SNR average (sampled when the value changes).
    double snr_sum_ = 0.0;
    int snr_samples_ = 0;
    float snr_last_sampled_ = -1000.0f;
    int pending_count_ = 0;
    double pending_refreshed_at_ = -1.0;
};

} // namespace skylink
