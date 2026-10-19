#pragma once

#include "window_base.hpp"
#include <chrono>
#include <functional>
#include <string>

namespace skylink {

// Weather requests go to the linked station. App owns the one request
// timer (weather_request_pending / weather_request_time, cleared on the
// reply or after WEATHER_TIMEOUT_MS); this window only watches that state:
// a request that ends without a new report is shown as timed out with a
// Retry button.
class WeatherWindow : public WindowBase {
public:
    explicit WeatherWindow(SharedAppState& state);
    void render() override;
    const char* name() const override { return "Weather"; }

    // Action callbacks (set by App during initialization). A request was
    // sent iff it left *state_.weather_request_pending set.
    std::function<void(const std::string&)> onRequestForecast;
    std::function<void(const std::string&)> onRequestMETAR;
    std::function<void(const std::string&)> onRequestTAF;
    std::function<void(const std::string&)> onRequestMarine;
    // Active weather alerts (type 0x05) for a US state code, grid square,
    // "lat,lon" or place name.
    std::function<void(const std::string&)> onRequestAlerts;
    std::function<bool(bool)> onCheckStationLink;
    // Drops the pending request in App so a late reply is ignored (G4).
    std::function<void()> onCancelWeatherRequest;

    // Must match App's WEATHER_TIMEOUT_MS (shown as the countdown).
    int timeout_seconds = 30;

private:
    static constexpr int MAX_RETRIES = 2;

    int retry_count_ = 0;
    std::string last_request_type_;   // "forecast", "metar", "taf", "marine", "alerts"
    std::string last_request_param_;
    bool awaiting_ = false;           // a request we sent is outstanding
    bool timed_out_ = false;
    size_t reports_at_request_ = 0;
    int64_t requested_unix_ = 0;
    std::chrono::steady_clock::time_point requested_at_{};

    bool sendRequest(const std::string& type, const std::string& param);
    void requestWeather(const std::string& type, const std::string& param);
    void retryLastRequest();
    void cancelRequest();
    void watchPendingRequest();
};

} // namespace skylink
