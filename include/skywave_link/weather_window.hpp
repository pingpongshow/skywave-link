#pragma once

#include "window_base.hpp"
#include <functional>
#include <string>
#include <chrono>

namespace skylink {

class WeatherWindow : public WindowBase {
public:
    explicit WeatherWindow(SharedAppState& state);
    void render() override;
    void update() override;
    const char* name() const override { return "Weather"; }

    // Action callbacks (set by App during initialization)
    std::function<void(const std::string&)> onRequestForecast;
    std::function<void(const std::string&)> onRequestMETAR;
    std::function<void(const std::string&)> onRequestTAF;
    std::function<void(const std::string&)> onRequestMarine;
    std::function<bool(bool)> onCheckStationLink;
    std::function<void()> onCancelWeatherRequest;

private:
    static constexpr float TIMEOUT_SECONDS = 30.0f;
    static constexpr int MAX_RETRIES = 2;

    int retry_count_ = 0;
    std::string last_request_type_;   // "forecast", "metar", "taf", "marine"
    std::string last_request_param_;
    bool timed_out_ = false;

    void requestWeather(const std::string& type, const std::string& param);
    void retryLastRequest();
    void cancelRequest();
};

} // namespace skylink
