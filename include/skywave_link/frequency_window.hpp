#pragma once
#include "window_base.hpp"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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
    // Tunes the rig (App::tuneOperatingFrequency(hz, false)); false when
    // the radio did not move. Unset: RadioController::setFrequency.
    std::function<bool(uint64_t)> onTuneFrequency;

    // Band/slot ranking from the beacon and ping history (exposed for the
    // cache; recomputed at most every few seconds, not per frame).
    struct BandRecommendation {
        std::string band;
        float score = -99.0f;
        float avg_snr = -30.0f;
        float link_success = 0.0f;  // 0..1
        float occupancy = 0.0f;     // 0..1
        float propagation_score = 0.0f;
        float path_km = 0.0f;
        float luf_mhz = 0.0f;
        float muf_mhz = 0.0f;
    };

private:
    // QSY: a suggestion to the peer while linked, else tune the rig.
    // Band/slot/frequency state changes only once the rig moved.
    void requestSlotChange(const std::string& band, int slot);
    bool tuneToSlot(const std::string& band, int slot);
    void renderSnifferResults();
    void renderRecommendations();
    const std::vector<BandRecommendation>& bandRecommendations();

    std::string view_band_;     // band whose slots are shown
    std::vector<BandRecommendation> recs_cache_;
    double recs_computed_at_ = -1.0;
};

} // namespace skylink
