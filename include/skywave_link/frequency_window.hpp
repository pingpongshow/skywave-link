#pragma once
#include "window_base.hpp"
#include <functional>

namespace skylink {

class FrequencyWindow : public WindowBase {
public:
    explicit FrequencyWindow(SharedAppState& state);
    void render() override;
    const char* name() const override { return "Frequency Management"; }

    std::function<void()> onStartSniffer;
    std::function<void()> onStopSniffer;
    std::function<void(const std::string&, int, uint32_t)> onSendQsySuggestion;
    std::function<void(bool)> onRespondQsySuggestion;

private:
    bool requestSlotChange(const std::string& band, int slot);
    void renderSnifferResults();
    void renderRecommendations();
};

} // namespace skylink
