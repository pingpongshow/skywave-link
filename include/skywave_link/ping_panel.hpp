// PingPanel - Ping / status check display panel
#pragma once

#include "window_base.hpp"
#include <functional>
#include <string>

namespace skylink {

class PingPanel : public WindowBase {
public:
    explicit PingPanel(SharedAppState& state);

    void render() override;
    const char* name() const override { return "Ping"; }

    // Action callbacks
    std::function<void(const std::string&)> onSendPing;
};

} // namespace skylink
