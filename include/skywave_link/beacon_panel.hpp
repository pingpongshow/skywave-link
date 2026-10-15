// BeaconPanel - Heard beacons display panel
#pragma once

#include "window_base.hpp"
#include <functional>
#include <string>

namespace skylink {

class BeaconPanel : public WindowBase {
public:
    explicit BeaconPanel(SharedAppState& state);

    void render() override;
    const char* name() const override { return "Beacons"; }

    // Action callbacks
    std::function<void()> onSendBeacon;
    std::function<void(const std::string&)> onConnectToStation;
};

} // namespace skylink
