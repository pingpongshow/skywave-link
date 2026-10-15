#include "skywave_link/station_map_window.hpp"
#include "skywave_link/image_preview.hpp"

#include <SDL.h>
#include <SDL_opengl.h>
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

namespace skylink {

namespace {

#ifndef SKYWAVE_LINK_SOURCE_DIR
#define SKYWAVE_LINK_SOURCE_DIR ""
#endif

struct GeoPoint {
    double lat = 0.0;
    double lon = 0.0;
};

constexpr double kPi = 3.14159265358979323846;
constexpr const char* kBasemapRelativePath = "assets/maps/natural_earth_gray_50m.png";

double degToRad(double degrees) {
    return degrees * kPi / 180.0;
}

template <typename T>
T clampValue(T value, T min_value, T max_value) {
    return std::max(min_value, std::min(value, max_value));
}

template <typename TextureId = ImTextureID>
TextureId toImTextureId(uint32_t texture_id) {
    if constexpr (std::is_pointer_v<TextureId>) {
        return (TextureId)(uintptr_t)texture_id;
    } else {
        return static_cast<TextureId>(texture_id);
    }
}

double haversineKm(double lat1, double lon1, double lat2, double lon2) {
    constexpr double kEarthRadiusKm = 6371.0;
    const double dlat = degToRad(lat2 - lat1);
    const double dlon = degToRad(lon2 - lon1);
    const double a = std::sin(dlat / 2.0) * std::sin(dlat / 2.0) +
                     std::cos(degToRad(lat1)) * std::cos(degToRad(lat2)) *
                         std::sin(dlon / 2.0) * std::sin(dlon / 2.0);
    const double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1.0 - a)));
    return kEarthRadiusKm * c;
}

const char* sourceLabel(StationLocationSource source) {
    switch (source) {
        case StationLocationSource::Beacon:
            return "Beacon";
        case StationLocationSource::PositionBeacon:
            return "Position Beacon";
        case StationLocationSource::DirectPosition:
            return "Direct";
        case StationLocationSource::APRS:
            return "APRS";
    }
    return "Unknown";
}

ImU32 sourceColor(StationLocationSource source) {
    switch (source) {
        case StationLocationSource::Beacon:
            return IM_COL32(245, 192, 66, 255);
        case StationLocationSource::PositionBeacon:
            return IM_COL32(88, 181, 255, 255);
        case StationLocationSource::DirectPosition:
            return IM_COL32(255, 107, 107, 255);
        case StationLocationSource::APRS:
            return IM_COL32(92, 201, 123, 255);
    }
    return IM_COL32(230, 230, 230, 255);
}

bool resolveLocalPosition(const SharedAppState& state, double& lat, double& lon) {
    if (state.my_lat && state.my_lon) {
        const double candidate_lat = *state.my_lat;
        const double candidate_lon = *state.my_lon;
        if (std::abs(candidate_lat) > 1e-6 || std::abs(candidate_lon) > 1e-6) {
            lat = candidate_lat;
            lon = candidate_lon;
            return true;
        }
    }

    if (state.config && !state.config->station.grid_square.empty()) {
        return FrequencyManager::gridSquareToLatLon(state.config->station.grid_square, lat, lon);
    }

    return false;
}

const std::vector<std::vector<GeoPoint>>& worldCoastlines() {
    static const std::vector<std::vector<GeoPoint>> coastlines = {
        {{72, -168}, {70, -150}, {66, -136}, {61, -128}, {56, -132}, {51, -126},
         {48, -124}, {45, -124}, {40, -123}, {34, -118}, {28, -112}, {23, -106},
         {22, -97}, {25, -90}, {29, -83}, {32, -80}, {36, -76}, {41, -71},
         {47, -66}, {54, -68}, {60, -78}, {66, -96}, {71, -124}, {75, -150}, {72, -168}},
        {{82, -73}, {80, -52}, {76, -36}, {72, -26}, {66, -22}, {60, -36},
         {61, -50}, {67, -58}, {74, -64}, {82, -73}},
        {{12, -79}, {9, -74}, {4, -77}, {-4, -80}, {-12, -77}, {-20, -74},
         {-31, -71}, {-40, -72}, {-50, -68}, {-54, -63}, {-50, -56}, {-39, -51},
         {-27, -48}, {-16, -43}, {-7, -37}, {4, -39}, {9, -50}, {10, -60}, {12, -79}},
        {{36, -10}, {44, -10}, {52, -5}, {59, 8}, {64, 28}, {68, 55}, {72, 90},
         {72, 122}, {66, 150}, {58, 163}, {52, 148}, {47, 130}, {42, 114},
         {38, 96}, {34, 78}, {31, 62}, {30, 46}, {34, 30}, {39, 18}, {44, 10},
         {48, 4}, {45, -2}, {40, -6}, {36, -10}},
        {{29, 67}, {23, 73}, {19, 79}, {16, 87}, {11, 95}, {5, 103}, {1, 110},
         {3, 118}, {9, 123}, {16, 119}, {21, 110}, {26, 98}, {29, 86}, {31, 76}, {29, 67}},
        {{31, 34}, {29, 41}, {25, 49}, {18, 55}, {13, 52}, {15, 44},
         {19, 40}, {24, 36}, {31, 34}},
        {{37, -17}, {32, -5}, {28, 10}, {21, 23}, {13, 32}, {3, 39}, {-9, 42},
         {-20, 35}, {-31, 24}, {-35, 12}, {-31, 1}, {-20, -7}, {-5, -13},
         {12, -16}, {26, -14}, {34, -10}, {37, -17}},
        {{-13, 49}, {-16, 50}, {-21, 48}, {-25, 45}, {-20, 43}, {-15, 46}, {-13, 49}},
        {{45, 141}, {41, 145}, {36, 143}, {33, 138}, {35, 135}, {40, 138}, {45, 141}},
        {{-11, 113}, {-19, 114}, {-28, 121}, {-35, 132}, {-38, 145}, {-34, 153},
         {-26, 153}, {-17, 146}, {-12, 136}, {-11, 113}},
        {{-60, -180}, {-66, -150}, {-69, -110}, {-71, -70}, {-72, -20}, {-70, 30},
         {-69, 75}, {-68, 120}, {-64, 160}, {-60, 180}}
    };
    return coastlines;
}

std::string formatDistance(double distance_km) {
    if (distance_km < 0.0) {
        return "--";
    }

    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.0f km", distance_km);
    return buffer;
}

std::string formatCoordinates(double lat, double lon) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.3f, %.3f", lat, lon);
    return buffer;
}

std::string formatLatitudeLabel(double lat) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%.0f%c", std::abs(lat), lat >= 0.0 ? 'N' : 'S');
    return buffer;
}

std::string formatLongitudeLabel(double lon) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%.0f%c", std::abs(lon), lon >= 0.0 ? 'E' : 'W');
    return buffer;
}

std::string formatTimestamp(int64_t timestamp) {
    if (timestamp <= 0) {
        return "--";
    }

    const time_t time_value = static_cast<time_t>(timestamp);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", std::localtime(&time_value));
    return buffer;
}

} // namespace

StationMapWindow::StationMapWindow(SharedAppState& state) : WindowBase(state) {}

StationMapWindow::~StationMapWindow() {
    releaseBasemap();
}

void StationMapWindow::releaseBasemap() {
    if (basemap_texture_id_ != 0) {
        const GLuint texture_id = static_cast<GLuint>(basemap_texture_id_);
        glDeleteTextures(1, &texture_id);
        basemap_texture_id_ = 0;
    }
    basemap_width_ = 0;
    basemap_height_ = 0;
}

std::string StationMapWindow::resolveBasemapPath() const {
    std::vector<std::filesystem::path> candidate_roots;

    char* base_path = SDL_GetBasePath();
    if (base_path != nullptr) {
        candidate_roots.emplace_back(base_path);
        SDL_free(base_path);
    }

    std::error_code current_path_ec;
    const std::filesystem::path current_path = std::filesystem::current_path(current_path_ec);
    if (!current_path_ec) {
        candidate_roots.push_back(current_path);
    }

#if defined(SKYWAVE_LINK_SOURCE_DIR)
    if constexpr (sizeof(SKYWAVE_LINK_SOURCE_DIR) > 1) {
        candidate_roots.emplace_back(SKYWAVE_LINK_SOURCE_DIR);
    }
#endif

    std::error_code exists_ec;
    for (const auto& root : candidate_roots) {
        const std::filesystem::path candidate = root / kBasemapRelativePath;
        if (std::filesystem::exists(candidate, exists_ec) && !exists_ec) {
            return candidate.string();
        }
        exists_ec.clear();
    }

    return "";
}

bool StationMapWindow::loadBasemapTexture(const std::string& path) {
    const ImageData image = decodeImageFile(path);
    if (!image.valid || image.width <= 0 || image.height <= 0 || image.pixels.empty()) {
        basemap_error_ = "Natural Earth basemap could not be decoded.";
        return false;
    }

    releaseBasemap();

    GLuint texture_id = 0;
    glGenTextures(1, &texture_id);
    if (texture_id == 0) {
        basemap_error_ = "Natural Earth basemap texture upload failed.";
        return false;
    }

    GLint previous_texture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);

    glBindTexture(GL_TEXTURE_2D, texture_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA,
                 image.width,
                 image.height,
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 image.pixels.data());
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous_texture));

    basemap_texture_id_ = static_cast<uint32_t>(texture_id);
    basemap_width_ = image.width;
    basemap_height_ = image.height;
    basemap_error_.clear();
    basemap_path_ = path;
    return true;
}

void StationMapWindow::ensureBasemapLoaded() {
    if (basemap_texture_id_ != 0 || basemap_load_attempted_) {
        return;
    }

    basemap_load_attempted_ = true;
    const std::string path = resolveBasemapPath();
    if (path.empty()) {
        basemap_error_ = "Natural Earth basemap not found in assets/maps.";
        return;
    }

    if (!loadBasemapTexture(path) && basemap_error_.empty()) {
        basemap_error_ = "Natural Earth basemap could not be loaded.";
    }
}

void StationMapWindow::render() {
    if (!state_.station_locations) {
        ImGui::TextDisabled("Station map data is unavailable.");
        return;
    }

    const auto& stations = *state_.station_locations;
    double local_lat = 0.0;
    double local_lon = 0.0;
    const bool has_local_position = resolveLocalPosition(state_, local_lat, local_lon);
    ensureBasemapLoaded();

    ImGui::Text("Mapped stations: %zu", stations.size());
    ImGui::SameLine();
    if (has_local_position) {
        ImGui::TextDisabled("Local reference: %s", formatCoordinates(local_lat, local_lon).c_str());
    } else {
        ImGui::TextDisabled("Set your station position or grid for distance calculations.");
    }
    ImGui::TextDisabled("Basemap: Natural Earth Gray Earth 1:50m");
    if (!basemap_error_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", basemap_error_.c_str());
    }

    ImGui::Separator();

    const float canvas_height = 320.0f;
    ImVec2 canvas_size(ImGui::GetContentRegionAvail().x, canvas_height);
    if (canvas_size.x < 220.0f) {
        canvas_size.x = 220.0f;
    }

    const ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##StationMapCanvas", canvas_size);
    const ImVec2 canvas_max(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(canvas_pos, canvas_max, IM_COL32(14, 21, 33, 255), 8.0f);
    draw_list->AddRect(canvas_pos, canvas_max, IM_COL32(75, 103, 137, 255), 8.0f, 0, 1.5f);
    draw_list->AddText(ImVec2(canvas_pos.x + 14.0f, canvas_pos.y + 10.0f),
                       IM_COL32(228, 235, 243, 255),
                       "Received Station Map");

    const ImVec2 plot_min(canvas_pos.x + 46.0f, canvas_pos.y + 28.0f);
    const ImVec2 plot_max(canvas_max.x - 14.0f, canvas_max.y - 28.0f);
    draw_list->AddRectFilled(plot_min, plot_max, IM_COL32(10, 16, 25, 255), 6.0f);
    draw_list->AddRect(plot_min, plot_max, IM_COL32(64, 85, 110, 255), 6.0f, 0, 1.0f);

    const StationLocationEntry* hovered_station = nullptr;
    double hovered_distance_km = -1.0;

    bool bounds_initialized = false;
    double min_lat = 0.0;
    double max_lat = 0.0;
    double min_lon = 0.0;
    double max_lon = 0.0;

    auto extend_bounds = [&](double lat, double lon) {
        if (!bounds_initialized) {
            min_lat = max_lat = lat;
            min_lon = max_lon = lon;
            bounds_initialized = true;
            return;
        }
        min_lat = std::min(min_lat, lat);
        max_lat = std::max(max_lat, lat);
        min_lon = std::min(min_lon, lon);
        max_lon = std::max(max_lon, lon);
    };

    if (has_local_position) {
        extend_bounds(local_lat, local_lon);
    }
    for (const auto& station : stations) {
        if (station.has_coordinates) {
            extend_bounds(station.latitude, station.longitude);
        }
    }

    if (!bounds_initialized) {
        min_lat = -70.0;
        max_lat = 80.0;
        min_lon = -180.0;
        max_lon = 180.0;
    } else {
        const double lat_center = (min_lat + max_lat) * 0.5;
        const double lon_center = (min_lon + max_lon) * 0.5;
        const double lat_span = std::max(40.0, (max_lat - min_lat) * 1.35 + 6.0);
        const double lon_span = std::max(60.0, (max_lon - min_lon) * 1.35 + 8.0);
        min_lat = lat_center - lat_span * 0.5;
        max_lat = lat_center + lat_span * 0.5;
        min_lon = lon_center - lon_span * 0.5;
        max_lon = lon_center + lon_span * 0.5;
    }

    min_lat = clampValue(min_lat, -89.5, 89.5);
    max_lat = clampValue(max_lat, -89.5, 89.5);
    min_lon = clampValue(min_lon, -180.0, 180.0);
    max_lon = clampValue(max_lon, -180.0, 180.0);

    if (max_lat - min_lat < 8.0) {
        const double lat_center = (min_lat + max_lat) * 0.5;
        min_lat = clampValue(lat_center - 4.0, -89.5, 89.5);
        max_lat = clampValue(lat_center + 4.0, -89.5, 89.5);
    }
    if (max_lon - min_lon < 12.0) {
        const double lon_center = (min_lon + max_lon) * 0.5;
        min_lon = clampValue(lon_center - 6.0, -180.0, 180.0);
        max_lon = clampValue(lon_center + 6.0, -180.0, 180.0);
    }

    const float plot_width = plot_max.x - plot_min.x;
    const float plot_height = plot_max.y - plot_min.y;

    auto project = [&](double lat, double lon) -> ImVec2 {
        const double lon_ratio = (lon - min_lon) / (max_lon - min_lon);
        const double lat_ratio = (lat - min_lat) / (max_lat - min_lat);
        const float x = plot_min.x + static_cast<float>(lon_ratio) * plot_width;
        const float y = plot_max.y - static_cast<float>(lat_ratio) * plot_height;
        return ImVec2(x, y);
    };

    draw_list->PushClipRect(plot_min, plot_max, true);
    if (basemap_texture_id_ != 0) {
        const ImVec2 uv0(
            static_cast<float>(clampValue((min_lon + 180.0) / 360.0, 0.0, 1.0)),
            static_cast<float>(clampValue((90.0 - max_lat) / 180.0, 0.0, 1.0)));
        const ImVec2 uv1(
            static_cast<float>(clampValue((max_lon + 180.0) / 360.0, 0.0, 1.0)),
            static_cast<float>(clampValue((90.0 - min_lat) / 180.0, 0.0, 1.0)));
        draw_list->AddImage(toImTextureId(basemap_texture_id_),
                            plot_min,
                            plot_max,
                            uv0,
                            uv1,
                            IM_COL32(255, 255, 255, 255));
        draw_list->AddRectFilled(plot_min, plot_max, IM_COL32(4, 10, 18, 36), 0.0f);
    } else {
        for (const auto& path : worldCoastlines()) {
            if (path.size() < 2) {
                continue;
            }

            std::vector<ImVec2> projected;
            projected.reserve(path.size());
            for (const auto& point : path) {
                projected.push_back(project(point.lat, point.lon));
            }

            draw_list->AddPolyline(projected.data(),
                                   static_cast<int>(projected.size()),
                                   IM_COL32(114, 146, 181, 220),
                                   0,
                                   1.6f);
        }
    }

    for (int step = 0; step <= 6; ++step) {
        const float ratio = static_cast<float>(step) / 6.0f;
        const float x = plot_min.x + plot_width * ratio;
        const float y = plot_min.y + plot_height * ratio;

        draw_list->AddLine(ImVec2(x, plot_min.y), ImVec2(x, plot_max.y),
                           step == 0 || step == 6
                               ? IM_COL32(78, 100, 128, 255)
                               : IM_COL32(36, 52, 70, 170),
                           1.0f);
        draw_list->AddLine(ImVec2(plot_min.x, y), ImVec2(plot_max.x, y),
                           step == 0 || step == 6
                               ? IM_COL32(78, 100, 128, 255)
                               : IM_COL32(36, 52, 70, 170),
                           1.0f);
    }

    const ImVec2 mouse_pos = ImGui::GetIO().MousePos;
    float best_hover_distance_sq = 81.0f;

    if (has_local_position) {
        const ImVec2 local_point = project(local_lat, local_lon);
        draw_list->AddCircleFilled(local_point, 6.0f, IM_COL32(112, 230, 161, 255));
        draw_list->AddCircle(local_point, 10.0f, IM_COL32(112, 230, 161, 160), 0, 1.0f);
        draw_list->AddText(ImVec2(local_point.x + 8.0f, local_point.y - 6.0f),
                           IM_COL32(196, 255, 219, 255),
                           "YOU");
    }

    for (const auto& station : stations) {
        if (!station.has_coordinates) {
            continue;
        }

        const ImVec2 point = project(station.latitude, station.longitude);
        const ImU32 color = sourceColor(station.source);
        draw_list->AddCircleFilled(point, 5.0f, color);
        draw_list->AddCircle(point, 8.0f, IM_COL32(255, 255, 255, 48), 0, 1.0f);
        draw_list->AddText(ImVec2(point.x + 7.0f, point.y - 6.0f),
                           IM_COL32(232, 239, 247, 255),
                           station.callsign.c_str());

        const float dx = mouse_pos.x - point.x;
        const float dy = mouse_pos.y - point.y;
        const float hover_distance_sq = dx * dx + dy * dy;
        if (hover_distance_sq < best_hover_distance_sq) {
            best_hover_distance_sq = hover_distance_sq;
            hovered_station = &station;
            hovered_distance_km = has_local_position
                ? haversineKm(local_lat, local_lon, station.latitude, station.longitude)
                : -1.0;
        }
    }

    draw_list->PopClipRect();

    for (int step = 0; step <= 6; ++step) {
        const float ratio = static_cast<float>(step) / 6.0f;
        const float x = plot_min.x + plot_width * ratio;
        const float y = plot_min.y + plot_height * ratio;
        const double lon_value = min_lon + (max_lon - min_lon) * ratio;
        const double lat_value = max_lat - (max_lat - min_lat) * ratio;

        draw_list->AddText(ImVec2(x - 14.0f, plot_max.y + 6.0f),
                           IM_COL32(160, 177, 197, 255),
                           formatLongitudeLabel(lon_value).c_str());
        draw_list->AddText(ImVec2(canvas_pos.x + 8.0f, y - 6.0f),
                           IM_COL32(160, 177, 197, 255),
                           formatLatitudeLabel(lat_value).c_str());
    }

    if (stations.empty()) {
        draw_list->AddText(ImVec2(plot_min.x + 14.0f, plot_min.y + 14.0f),
                           IM_COL32(196, 204, 215, 255),
                           "No station markers yet. Received beacons/APRS positions will appear here.");
    }

    if (hovered_station) {
        ImGui::BeginTooltip();
        ImGui::Text("%s", hovered_station->callsign.c_str());
        ImGui::Separator();
        ImGui::Text("Source: %s", sourceLabel(hovered_station->source));
        if (!hovered_station->grid_square.empty()) {
            ImGui::Text("Grid: %s", hovered_station->grid_square.c_str());
        }
        ImGui::Text("Coords: %s",
                    formatCoordinates(hovered_station->latitude, hovered_station->longitude).c_str());
        if (hovered_distance_km >= 0.0) {
            ImGui::Text("Distance: %s", formatDistance(hovered_distance_km).c_str());
        }
        if (!hovered_station->details.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", hovered_station->details.c_str());
        }
        ImGui::EndTooltip();
    }

    ImGui::Spacing();
    ImGui::Separator();

    if (ImGui::BeginTable("##StationMapTable",
                          6,
                          ImGuiTableFlags_Borders |
                              ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_Resizable |
                              ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupColumn("Callsign", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Grid / Coords", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Distance", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Last Heard", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableHeadersRow();

        for (const auto& station : stations) {
            const double distance_km = has_local_position && station.has_coordinates
                ? haversineKm(local_lat, local_lon, station.latitude, station.longitude)
                : -1.0;

            std::string location_text;
            if (!station.grid_square.empty()) {
                location_text = station.grid_square;
            }
            if (station.has_coordinates) {
                if (!location_text.empty()) {
                    location_text += "  ";
                }
                location_text += formatCoordinates(station.latitude, station.longitude);
            } else if (location_text.empty()) {
                location_text = "--";
            }

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s", station.callsign.c_str());
            if (station.email_gateway) {
                ImGui::TextDisabled("Email gateway");
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(sourceColor(station.source)),
                               "%s",
                               sourceLabel(station.source));

            ImGui::TableSetColumnIndex(2);
            ImGui::TextWrapped("%s", location_text.c_str());

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%s", formatDistance(distance_km).c_str());

            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%s", formatTimestamp(station.timestamp).c_str());

            ImGui::TableSetColumnIndex(5);
            if (station.details.empty()) {
                ImGui::TextDisabled("--");
            } else {
                ImGui::TextWrapped("%s", station.details.c_str());
            }
        }

        ImGui::EndTable();
    }
}

} // namespace skylink
