// MainContentRenderer - Control panel, three-panel layout, right panel, bottom buttons
#pragma once

#include "window_base.hpp"
#include <functional>
#include <string>

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
    std::function<void(const std::string&)> onSendChatMessage;
    std::function<void()> onSendTypingIndicator;
    std::function<void(const std::string&)> onSendBroadcast;
    // BCAST while idle, session data while linked (App::sendBroadcastPayload).
    std::function<bool(const std::vector<uint8_t>&)> onSendBroadcastPayload;
    std::function<void(uint64_t)> onSetFrequency;
};

} // namespace skylink
