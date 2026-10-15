#include "skywave_link/weather_window.hpp"
#include "skywave_link/config.hpp"

#include "imgui.h"
#include <cstring>
#include <ctime>
#include <chrono>

namespace skylink {

namespace {

std::string formatUtcTime(int64_t unix_seconds) {
    std::time_t ts = static_cast<std::time_t>(unix_seconds);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &ts);
#else
    gmtime_r(&ts, &tm_utc);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%SZ", &tm_utc);
    return std::string(buf);
}

}

WeatherWindow::WeatherWindow(SharedAppState& state)
    : WindowBase(state)
{
}

void WeatherWindow::update() {
    // Check for timeout on pending weather request
    if (state_.weather_request_pending && *state_.weather_request_pending &&
        state_.weather_request_time && !timed_out_) {
        auto elapsed = std::chrono::steady_clock::now() - *state_.weather_request_time;
        auto seconds = std::chrono::duration_cast<std::chrono::duration<float>>(elapsed).count();
        if (seconds >= TIMEOUT_SECONDS) {
            timed_out_ = true;
            *state_.weather_request_pending = false;

            // Add error report
            if (state_.weather_reports) {
                WeatherReport err;
                err.type = "ERROR";
                err.location = last_request_param_;
                err.report = "Request timed out after " +
                             std::to_string(static_cast<int>(TIMEOUT_SECONDS)) +
                             " seconds. The remote station may not have internet access.";
                err.source_station = "SYSTEM";
                err.severity = "N/A";
                err.expires_utc = "N/A";
                err.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                state_.weather_reports->push_back(err);
            }
        }
    }
}

void WeatherWindow::requestWeather(const std::string& type, const std::string& param) {
    last_request_type_ = type;
    last_request_param_ = param;
    retry_count_ = 0;
    timed_out_ = false;

    if (state_.weather_request_time) {
        *state_.weather_request_time = std::chrono::steady_clock::now();
    }

    if (type == "forecast" && onRequestForecast) onRequestForecast(param);
    else if (type == "metar" && onRequestMETAR) onRequestMETAR(param);
    else if (type == "taf" && onRequestTAF) onRequestTAF(param);
    else if (type == "marine" && onRequestMarine) onRequestMarine(param);
}

void WeatherWindow::retryLastRequest() {
    if (retry_count_ >= MAX_RETRIES || last_request_type_.empty()) return;
    retry_count_++;
    timed_out_ = false;

    if (state_.weather_request_time) {
        *state_.weather_request_time = std::chrono::steady_clock::now();
    }

    if (last_request_type_ == "forecast" && onRequestForecast) onRequestForecast(last_request_param_);
    else if (last_request_type_ == "metar" && onRequestMETAR) onRequestMETAR(last_request_param_);
    else if (last_request_type_ == "taf" && onRequestTAF) onRequestTAF(last_request_param_);
    else if (last_request_type_ == "marine" && onRequestMarine) onRequestMarine(last_request_param_);
}

void WeatherWindow::cancelRequest() {
    timed_out_ = false;
    if (state_.weather_request_pending) {
        *state_.weather_request_pending = false;
    }
    if (onCancelWeatherRequest) {
        onCancelWeatherRequest();
    }
}

void WeatherWindow::render() {
    ImGui::Text("Weather Information");
    ImGui::Separator();

    // Check connection status
    bool is_connected = onCheckStationLink ? onCheckStationLink(false) : false;
    if (!is_connected) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
            "Connect to a remote station to request weather data");
        ImGui::Spacing();
    } else {
        ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f),
            "Connected to: %s", state_.modem->getStatus().remote_callsign.c_str());
        ImGui::Spacing();
    }

    // Pending request indicator with cancel button and timeout info
    bool is_pending = state_.weather_request_pending && *state_.weather_request_pending;
    if (is_pending) {
        auto elapsed = std::chrono::steady_clock::now() - *state_.weather_request_time;
        auto secs = std::chrono::duration_cast<std::chrono::duration<float>>(elapsed).count();
        int remaining = static_cast<int>(TIMEOUT_SECONDS - secs);
        if (remaining < 0) remaining = 0;

        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f),
            "Requesting weather... (%ds remaining)", remaining);
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            cancelRequest();
        }
        ImGui::Spacing();
    }

    // Timeout with retry
    if (timed_out_) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Request timed out");
        if (retry_count_ < MAX_RETRIES) {
            ImGui::SameLine();
            if (ImGui::Button("Retry")) {
                retryLastRequest();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(%d/%d retries)", retry_count_, MAX_RETRIES);
        } else {
            ImGui::SameLine();
            ImGui::TextDisabled("Max retries reached");
        }
        ImGui::Spacing();
    }

    // Tabs for different weather types
    if (ImGui::BeginTabBar("WeatherTabs")) {
        // Forecast Tab
        if (ImGui::BeginTabItem("Forecast")) {
            ImGui::Text("Location-Based Weather Forecast");
            ImGui::Separator();

            ImGui::InputText("Location/Grid", state_.weather_location_input, state_.weather_location_input_size);
            ImGui::SameLine();
            bool disabled = is_pending;
            if (disabled) ImGui::BeginDisabled();
            if (ImGui::Button("Get Forecast")) {
                if (std::strlen(state_.weather_location_input) > 0) {
                    requestWeather("forecast", state_.weather_location_input);
                }
            }
            if (disabled) ImGui::EndDisabled();

            // Use grid square if available
            if (!state_.config->station.grid_square.empty()) {
                ImGui::SameLine();
                if (disabled) ImGui::BeginDisabled();
                if (ImGui::Button("Use My Grid")) {
                    strncpy(state_.weather_location_input, state_.config->station.grid_square.c_str(),
                            state_.weather_location_input_size - 1);
                    requestWeather("forecast", state_.config->station.grid_square);
                }
                if (disabled) ImGui::EndDisabled();
            }

            ImGui::EndTabItem();
        }

        // Aviation Tab (METAR/TAF)
        if (ImGui::BeginTabItem("Aviation")) {
            ImGui::Text("Aviation Weather");
            ImGui::Separator();

            ImGui::InputText("Airport Code (ICAO)", state_.weather_airport_input, state_.weather_airport_input_size);

            bool disabled = is_pending;
            if (disabled) ImGui::BeginDisabled();
            if (ImGui::Button("Get METAR")) {
                if (std::strlen(state_.weather_airport_input) >= 4) {
                    requestWeather("metar", state_.weather_airport_input);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Get TAF")) {
                if (std::strlen(state_.weather_airport_input) >= 4) {
                    requestWeather("taf", state_.weather_airport_input);
                }
            }
            if (disabled) ImGui::EndDisabled();

            ImGui::TextDisabled("Enter 4-letter ICAO code (e.g., KJFK, EGLL)");
            ImGui::EndTabItem();
        }

        // Marine Tab
        if (ImGui::BeginTabItem("Marine")) {
            ImGui::Text("Marine Weather Forecasts");
            ImGui::Separator();

            ImGui::InputText("Zone Code", state_.weather_zone_input, state_.weather_zone_input_size);
            ImGui::SameLine();
            bool disabled = is_pending;
            if (disabled) ImGui::BeginDisabled();
            if (ImGui::Button("Get Marine Wx")) {
                if (std::strlen(state_.weather_zone_input) > 0) {
                    requestWeather("marine", state_.weather_zone_input);
                }
            }
            if (disabled) ImGui::EndDisabled();

            ImGui::TextDisabled("Enter marine zone code (e.g., GMZ335, PZZ530, LEZ040)");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::Separator();

    // Display received weather reports
    ImGui::Text("Received Reports:");
    if (ImGui::BeginChild("WeatherReports", ImVec2(-1, 200), true)) {
        if (state_.weather_reports->empty()) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No weather reports received");
        } else {
            for (const auto& report : *state_.weather_reports) {
                // Header with type and location
                ImVec4 color = ImVec4(0.7f, 0.9f, 1.0f, 1.0f);  // Light blue default
                if (report.type == "METAR") color = ImVec4(0.6f, 1.0f, 0.6f, 1.0f);
                else if (report.type == "TAF") color = ImVec4(1.0f, 1.0f, 0.6f, 1.0f);
                else if (report.type == "MARINE") color = ImVec4(0.6f, 0.8f, 1.0f, 1.0f);
                else if (report.type == "ERROR") color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                else if (report.type == "ALERT") color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);

                ImGui::TextColored(color, "[%s] %s", report.type.c_str(), report.location.c_str());
                ImGui::TextWrapped("%s", report.report.c_str());
                if (!report.source_station.empty()) {
                    ImGui::TextDisabled("From: %s", report.source_station.c_str());
                }
                ImGui::TextDisabled("Received (UTC): %s", formatUtcTime(report.timestamp).c_str());
                ImGui::Separator();
            }
        }
    }
    ImGui::EndChild();
}

} // namespace skylink
