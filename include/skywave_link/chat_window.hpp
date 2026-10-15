// ChatWindow - Real-time chat with typing indicators and CQ
#pragma once

#include "window_base.hpp"
#include <functional>
#include <string>

namespace skylink {

class ChatWindow : public WindowBase {
public:
    explicit ChatWindow(SharedAppState& state);

    void render() override;
    const char* name() const override { return "Chat"; }

    // Action callbacks
    std::function<void(const std::string&)> onSendMessage;
    std::function<void()> onSendTypingIndicator;
    std::function<void()> onStartCQ;
    std::function<void()> onStopCQ;
    std::function<void(const std::string&)> onSendBroadcast;
    std::function<void(const std::string&)> onConnectToStation;
    std::function<void(const std::string&)> onSendPing;
};

} // namespace skylink
