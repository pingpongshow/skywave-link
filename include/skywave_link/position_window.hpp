#pragma once
#include "window_base.hpp"
#include <functional>

namespace skylink {
class PositionWindow : public WindowBase {
public:
    explicit PositionWindow(SharedAppState& state);
    void render() override;
    const char* name() const override { return "Position"; }

    std::function<void(double, double)> onUpdatePosition;
    std::function<void()> onAPRSConnect;
    std::function<void()> onAPRSDisconnect;
};
}
