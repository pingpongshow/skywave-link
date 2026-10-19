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
    std::function<void()> onAPRSConnect;
    std::function<void()> onAPRSDisconnect;
    // BCAST while idle, session data while linked (App::sendBroadcastPayload).
    std::function<bool(const std::vector<uint8_t>&)> onSendBroadcastPayload;
};
}
