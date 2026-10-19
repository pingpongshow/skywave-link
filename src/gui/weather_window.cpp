#include "skywave_link/weather_window.hpp"
#include "skywave_link/config.hpp"
#include "skywave_link/gui_util.hpp"
#include "skywave_link/modern_style.hpp"

#include "imgui.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>

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

const char* requestLabel(const std::string& type) {
    if (type == "forecast") return "forecast";
    if (type == "metar") return "METAR";
    if (type == "taf") return "TAF";
    if (type == "marine") return "marine forecast";
    if (type == "alerts") return "weather alerts";
    return "weather";
}

}  // namespace

WeatherWindow::WeatherWindow(SharedAppState& state)
    : WindowBase(state)
{
}

bool WeatherWindow::sendRequest(const std::string& type, const std::string& param) {
    const std::string label = requestLabel(type);
    if (!onCheckStationLink || !onCheckStationLink(true)) {
        postStatus("Weather " + label + " not requested: connect to a remote station first",
                   StatusKind::Error);
        return false;
    }

    std::function<void(const std::string&)>* request = nullptr;
    if (type == "forecast") request = &onRequestForecast;
    else if (type == "metar") request = &onRequestMETAR;
    else if (type == "taf") request = &onRequestTAF;
    else if (type == "marine") request = &onRequestMarine;
    else if (type == "alerts") request = &onRequestAlerts;
    if (!request || !*request) {
        postStatus("Weather " + label + " requests are not available", StatusKind::Error);
        return false;
    }

    const size_t log_mark = state_.chatHistorySize();
    reports_at_request_ = state_.weather_reports ? state_.weather_reports->size() : 0;
    (*request)(param);

    if (state_.weather_request_pending && *state_.weather_request_pending) {
        awaiting_ = true;
        timed_out_ = false;
        requested_at_ = std::chrono::steady_clock::now();
        requested_unix_ = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const std::string peer = state_.modem ? state_.modem->getStatus().remote_callsign : "";
        postStatus("Requested " + label + " for " + param +
                       (peer.empty() ? std::string() : " from " + peer) + "; waiting for the reply",
                   StatusKind::Info);
        return true;
    }
    const std::string reason = state_.systemMessageSince(log_mark);
    postStatus(reason.empty() ? "Weather " + label + " request was not sent" : reason,
               StatusKind::Error);
    return false;
}

void WeatherWindow::requestWeather(const std::string& type, const std::string& param) {
    last_request_type_ = type;
    last_request_param_ = param;
    retry_count_ = 0;
    (void)sendRequest(type, param);
}

void WeatherWindow::retryLastRequest() {
    if (retry_count_ >= MAX_RETRIES || last_request_type_.empty()) return;
    retry_count_++;
    (void)sendRequest(last_request_type_, last_request_param_);
}

void WeatherWindow::cancelRequest() {
    awaiting_ = false;
    timed_out_ = false;
    if (state_.weather_request_pending) {
        *state_.weather_request_pending = false;
    }
    if (onCancelWeatherRequest) {
        onCancelWeatherRequest();
        postStatus("Weather request cancelled", StatusKind::Info);
    } else {
        postStatus("Weather request cancelled here; a late reply may still be shown",
                   StatusKind::Warning);
    }
}

// The App clears weather_request_pending on a reply or on its timeout.
void WeatherWindow::watchPendingRequest() {
    const bool pending = state_.weather_request_pending && *state_.weather_request_pending;
    if (!awaiting_ || pending) {
        return;
    }
    awaiting_ = false;
    // A reply is inserted at the front (the list is capped, so its size may
    // not change): a non-error report newer than the request.
    const bool got_reply = state_.weather_reports && !state_.weather_reports->empty() &&
                           state_.weather_reports->front().type != "ERROR" &&
                           (state_.weather_reports->size() != reports_at_request_ ||
                            state_.weather_reports->front().timestamp >= requested_unix_);
    if (got_reply) {
        const WeatherReport& newest = state_.weather_reports->front();
        postStatus("Received " + newest.type + " for " + newest.location + " from " +
                       newest.source_station,
                   StatusKind::Success);
        return;
    }

    timed_out_ = true;
    const auto waited = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - requested_at_).count();
    postStatus("No weather reply after " + std::to_string(waited) +
                   " s; the remote station may not have internet access",
               StatusKind::Error);
    if (state_.weather_reports) {
        WeatherReport err;
        err.type = "ERROR";
        err.location = last_request_param_;
        err.report = "Request timed out after " + std::to_string(waited) +
                     " seconds. The remote station may not have internet access.";
        err.source_station = "SYSTEM";
        err.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        // Newest first, like App's received reports.
        state_.weather_reports->insert(state_.weather_reports->begin(), err);
    }
}

void WeatherWindow::render() {
    const ui::Palette& pal = ui::palette();
    watchPendingRequest();

    ImGui::Text("Weather Information");
    ImGui::Separator();
    ui::StatusLine(status_);

    // Check connection status
    bool is_connected = onCheckStationLink ? onCheckStationLink(false) : false;
    if (!is_connected) {
        ImGui::TextColored(pal.warning, "Connect to a remote station to request weather data");
    } else {
        ImGui::TextColored(pal.success, "Connected to: %s",
                           state_.modem ? state_.modem->getStatus().remote_callsign.c_str() : "?");
    }
    ImGui::Spacing();

    // Pending request indicator with cancel button and timeout info
    bool is_pending = state_.weather_request_pending && *state_.weather_request_pending;
    if (is_pending && state_.weather_request_time) {
        auto elapsed = std::chrono::steady_clock::now() - *state_.weather_request_time;
        auto secs = std::chrono::duration_cast<std::chrono::duration<float>>(elapsed).count();
        int remaining = static_cast<int>(static_cast<float>(timeout_seconds) - secs);
        if (remaining < 0) remaining = 0;

        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(pal.warning, "Requesting weather... (%ds remaining)", remaining);
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            cancelRequest();
        }
        ImGui::Spacing();
    }

    // Timeout with retry
    if (timed_out_ && !is_pending) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(pal.danger, "Request timed out");
        ImGui::SameLine();
        if (retry_count_ < MAX_RETRIES) {
            if (ImGui::Button("Retry")) {
                retryLastRequest();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(%d/%d retries)", retry_count_, MAX_RETRIES);
        } else {
            ImGui::TextDisabled("Max retries reached");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Dismiss")) {
            timed_out_ = false;
        }
        ImGui::Spacing();
    }

    // Tabs for different weather types
    if (ImGui::BeginTabBar("WeatherTabs")) {
        // Forecast Tab
        if (ImGui::BeginTabItem("Forecast")) {
            ImGui::Text("Location-Based Weather Forecast");
            ImGui::Separator();

            {
                // Leave room on the row for the label and the buttons.
                const ImGuiStyle& style = ImGui::GetStyle();
                float trailing = ImGui::CalcTextSize("Location/Grid").x + style.ItemInnerSpacing.x +
                                 ImGui::CalcTextSize("Get Forecast").x + style.FramePadding.x * 2.0f +
                                 style.ItemSpacing.x +
                                 ImGui::CalcTextSize("Alerts").x + style.FramePadding.x * 2.0f +
                                 style.ItemSpacing.x;
                if (!state_.config->station.grid_square.empty()) {
                    trailing += ImGui::CalcTextSize("Use My Grid").x + style.FramePadding.x * 2.0f +
                                style.ItemSpacing.x;
                }
                ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 6.0f,
                                                 ImGui::GetContentRegionAvail().x - trailing));
            }
            ImGui::InputText("Location/Grid", state_.weather_location_input, state_.weather_location_input_size);
            ImGui::SameLine();
            ImGui::BeginDisabled(is_pending);
            if (ImGui::Button("Get Forecast")) {
                const std::string location = gui::trimCopy(state_.weather_location_input);
                if (location.empty()) {
                    postStatus("Enter a location or grid square", StatusKind::Error);
                } else {
                    requestWeather("forecast", location);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Alerts")) {
                const std::string location = gui::trimCopy(state_.weather_location_input);
                if (location.empty()) {
                    postStatus("Enter a US state code (e.g. TX), grid square or location for alerts",
                               StatusKind::Error);
                } else {
                    requestWeather("alerts", location);
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Active weather alerts (US, api.weather.gov) fetched by the linked station");
            }

            // Use grid square if available
            if (!state_.config->station.grid_square.empty()) {
                ImGui::SameLine();
                if (ImGui::Button("Use My Grid")) {
                    std::strncpy(state_.weather_location_input,
                                 state_.config->station.grid_square.c_str(),
                                 state_.weather_location_input_size - 1);
                    state_.weather_location_input[state_.weather_location_input_size - 1] = '\0';
                    requestWeather("forecast", state_.config->station.grid_square);
                }
            }
            ImGui::EndDisabled();

            ImGui::EndTabItem();
        }

        // Aviation Tab (METAR/TAF)
        if (ImGui::BeginTabItem("Aviation")) {
            ImGui::Text("Aviation Weather");
            ImGui::Separator();

            ImGui::InputText("Airport Code (ICAO)", state_.weather_airport_input,
                             state_.weather_airport_input_size, ImGuiInputTextFlags_CharsUppercase);

            const std::string icao = gui::trimCopy(state_.weather_airport_input);
            auto request_aviation = [&](const char* type) {
                if (icao.size() != 4) {
                    postStatus("Enter a 4-letter ICAO airport code (e.g. KJFK)", StatusKind::Error);
                } else {
                    requestWeather(type, icao);
                }
            };
            ImGui::BeginDisabled(is_pending);
            if (ImGui::Button("Get METAR")) {
                request_aviation("metar");
            }
            ImGui::SameLine();
            if (ImGui::Button("Get TAF")) {
                request_aviation("taf");
            }
            ImGui::EndDisabled();

            ImGui::TextDisabled("Enter 4-letter ICAO code (e.g., KJFK, EGLL)");
            ImGui::EndTabItem();
        }

        // Marine Tab
        if (ImGui::BeginTabItem("Marine")) {
            ImGui::Text("Marine Weather Forecasts");
            ImGui::Separator();

            {
                const ImGuiStyle& style = ImGui::GetStyle();
                const float trailing = ImGui::CalcTextSize("Zone Code").x + style.ItemInnerSpacing.x +
                                       ImGui::CalcTextSize("Get Marine Wx").x +
                                       style.FramePadding.x * 2.0f + style.ItemSpacing.x;
                ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 6.0f,
                                                 ImGui::GetContentRegionAvail().x - trailing));
            }
            ImGui::InputText("Zone Code", state_.weather_zone_input, state_.weather_zone_input_size,
                             ImGuiInputTextFlags_CharsUppercase);
            ImGui::SameLine();
            ImGui::BeginDisabled(is_pending);
            if (ImGui::Button("Get Marine Wx")) {
                const std::string zone = gui::trimCopy(state_.weather_zone_input);
                if (zone.empty()) {
                    postStatus("Enter a marine zone code (e.g. GMZ335)", StatusKind::Error);
                } else {
                    requestWeather("marine", zone);
                }
            }
            ImGui::EndDisabled();

            ImGui::TextDisabled("Enter marine zone code (e.g., GMZ335, PZZ530, LEZ040)");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::Separator();

    // Display received weather reports (newest first)
    ImGui::Text("Received Reports:");
    if (ImGui::BeginChild("WeatherReports", ImVec2(-1, -1), ImGuiChildFlags_Borders)) {
        if (!state_.weather_reports || state_.weather_reports->empty()) {
            ImGui::TextDisabled("No weather reports received");
        } else {
            for (const auto& report : *state_.weather_reports) {
                ImVec4 color = pal.info;
                if (report.type == "METAR") color = pal.success;
                else if (report.type == "TAF") color = pal.accent;
                else if (report.type == "MARINE") color = pal.info;
                else if (report.type == "ERROR" || report.type == "ALERT") color = pal.danger;

                ImGui::TextColored(color, "[%s] %s", report.type.c_str(), report.location.c_str());
                if (report.type == "ALERT") {
                    ImGui::TextColored(pal.warning, "Severity: %s   Expires (UTC): %s",
                                       report.severity.empty() ? "unknown" : report.severity.c_str(),
                                       report.expires_utc.empty() ? "unknown" : report.expires_utc.c_str());
                }
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
