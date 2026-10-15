// FrequencyManager implementation - HF band and channel management

#include "skywave_link/freq_manager.hpp"
#include "skywave_link/radio_controller.hpp"
#include "skywave_link/modem_client.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>

namespace skylink {

namespace {

constexpr double kPi = 3.14159265358979323846;

double degToRad(double deg) {
    return deg * kPi / 180.0;
}

}  // namespace

FrequencyManager::FrequencyManager() {
    initializeDefaultBands();
}

FrequencyManager::~FrequencyManager() = default;

void FrequencyManager::initializeDefaultBands() {
    // Define all standard HF amateur bands with calling frequencies
    // Slot spacing is 500 Hz, with 12 slots per band (±6 from calling freq)

    bands_["80m"] = {
        "80m", "80 Meter Band",
        DefaultFrequencies::BAND_80M,  // 3.590 MHz
        500, 12,
        3500000, 4000000,
        true
    };

    bands_["60m"] = {
        "60m", "60 Meter Band",
        DefaultFrequencies::BAND_60M,  // 5.357 MHz (Channel 3)
        500, 6,  // Fewer slots due to channelized nature
        5330000, 5405000,
        true
    };

    bands_["40m"] = {
        "40m", "40 Meter Band",
        DefaultFrequencies::BAND_40M,  // 7.055 MHz
        500, 12,
        7000000, 7300000,
        true
    };

    bands_["30m"] = {
        "30m", "30 Meter Band",
        DefaultFrequencies::BAND_30M,  // 10.130 MHz
        500, 8,  // Narrow band
        10100000, 10150000,
        true
    };

    bands_["20m"] = {
        "20m", "20 Meter Band",
        DefaultFrequencies::BAND_20M,  // 14.105 MHz
        500, 12,
        14000000, 14350000,
        true
    };

    bands_["17m"] = {
        "17m", "17 Meter Band",
        DefaultFrequencies::BAND_17M,  // 18.105 MHz
        500, 12,
        18068000, 18168000,
        true
    };

    bands_["15m"] = {
        "15m", "15 Meter Band",
        DefaultFrequencies::BAND_15M,  // 21.105 MHz
        500, 12,
        21000000, 21450000,
        true
    };

    bands_["12m"] = {
        "12m", "12 Meter Band",
        DefaultFrequencies::BAND_12M,  // 24.925 MHz
        500, 12,
        24890000, 24990000,
        true
    };

    bands_["10m"] = {
        "10m", "10 Meter Band",
        DefaultFrequencies::BAND_10M,  // 28.120 MHz
        500, 12,
        28000000, 29700000,
        true
    };

    // Initialize slots for each band
    for (const auto& [name, config] : bands_) {
        std::vector<WorkingSlot> band_slots;
        int half_slots = config.num_slots / 2;

        for (int i = -half_slots; i <= half_slots; ++i) {
            WorkingSlot slot;
            slot.band = name;
            slot.slot_number = i;
            slot.frequency_hz = config.calling_freq_hz + (i * config.slot_spacing_hz);
            slot.status = SlotStatus::Unknown;
            slot.last_checked = 0;
            band_slots.push_back(slot);
        }

        slots_[name] = band_slots;
    }
}

std::vector<BandConfig> FrequencyManager::getBands() const {
    std::vector<BandConfig> result;
    for (const auto& [name, config] : bands_) {
        result.push_back(config);
    }
    return result;
}

std::optional<BandConfig> FrequencyManager::getBand(const std::string& name) const {
    auto it = bands_.find(name);
    if (it == bands_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool FrequencyManager::setBandEnabled(const std::string& name, bool enabled) {
    auto it = bands_.find(name);
    if (it == bands_.end()) {
        return false;
    }
    it->second.enabled = enabled;
    return true;
}

uint64_t FrequencyManager::getCallingFrequency(const std::string& band) const {
    auto it = bands_.find(band);
    if (it == bands_.end()) {
        return 0;
    }
    return it->second.calling_freq_hz;
}

uint64_t FrequencyManager::getSlotFrequency(const std::string& band, int slot) const {
    auto it = bands_.find(band);
    if (it == bands_.end()) {
        return 0;
    }

    const auto& config = it->second;
    int half_slots = config.num_slots / 2;
    if (slot < -half_slots || slot > half_slots) {
        return 0;
    }

    return config.calling_freq_hz + (slot * config.slot_spacing_hz);
}

int FrequencyManager::frequencyToSlot(const std::string& band, uint64_t freq_hz) const {
    auto it = bands_.find(band);
    if (it == bands_.end()) {
        return 0;
    }

    const auto& config = it->second;
    int64_t offset = static_cast<int64_t>(freq_hz) - static_cast<int64_t>(config.calling_freq_hz);
    int slot = static_cast<int>(offset / static_cast<int64_t>(config.slot_spacing_hz));
    int half_slots = config.num_slots / 2;
    return std::clamp(slot, -half_slots, half_slots);
}

std::optional<std::string> FrequencyManager::frequencyToBand(uint64_t freq_hz) const {
    for (const auto& [name, config] : bands_) {
        if (freq_hz >= config.band_start_hz && freq_hz <= config.band_end_hz) {
            return name;
        }
    }
    return std::nullopt;
}

std::vector<WorkingSlot> FrequencyManager::getSlots(const std::string& band) const {
    auto it = slots_.find(band);
    if (it == slots_.end()) {
        return {};
    }
    return it->second;
}

std::optional<WorkingSlot> FrequencyManager::getSlot(const std::string& band, int slot) const {
    auto it = slots_.find(band);
    if (it == slots_.end()) {
        return std::nullopt;
    }

    for (const auto& s : it->second) {
        if (s.slot_number == slot) {
            return s;
        }
    }
    return std::nullopt;
}

void FrequencyManager::updateSlotStatus(const std::string& band, int slot, SlotStatus status,
                                         const std::string& occupant) {
    auto it = slots_.find(band);
    if (it == slots_.end()) {
        return;
    }

    for (auto& s : it->second) {
        if (s.slot_number == slot) {
            s.status = status;
            s.occupant = occupant;
            s.last_checked = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            break;
        }
    }
}

int FrequencyManager::findAvailableSlot(const std::string& band) const {
    auto it = slots_.find(band);
    if (it == slots_.end()) {
        return 1;  // Default to slot 1
    }

    // Prefer slots away from the calling frequency
    // Check positive slots first, then negative
    for (int distance = 1; distance <= 6; ++distance) {
        for (const auto& s : it->second) {
            if (std::abs(s.slot_number) == distance &&
                s.status == SlotStatus::Available) {
                return s.slot_number;
            }
        }
    }

    // If no available slots found, return slot 1 (assume it's available)
    return 1;
}

int FrequencyManager::findBestSlot(const std::string& band) const {
    // For now, same as findAvailableSlot
    // Could be enhanced to consider propagation, QRM, etc.
    return findAvailableSlot(band);
}

bool FrequencyManager::gridSquareToLatLon(const std::string& grid, double& lat, double& lon) {
    if (grid.size() < 4) {
        return false;
    }

    std::string g = grid;
    std::transform(g.begin(), g.end(), g.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    if (g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R') {
        return false;
    }
    if (g[2] < '0' || g[2] > '9' || g[3] < '0' || g[3] > '9') {
        return false;
    }

    const int field_lon = g[0] - 'A';
    const int field_lat = g[1] - 'A';
    const int square_lon = g[2] - '0';
    const int square_lat = g[3] - '0';

    lon = (field_lon * 20.0) - 180.0 + (square_lon * 2.0) + 1.0;
    lat = (field_lat * 10.0) - 90.0 + (square_lat * 1.0) + 0.5;

    if (g.size() >= 6) {
        const char sub_lon = static_cast<char>(std::tolower(static_cast<unsigned char>(g[4])));
        const char sub_lat = static_cast<char>(std::tolower(static_cast<unsigned char>(g[5])));
        if (sub_lon >= 'a' && sub_lon <= 'x' && sub_lat >= 'a' && sub_lat <= 'x') {
            lon = (field_lon * 20.0) - 180.0 + (square_lon * 2.0) +
                  ((sub_lon - 'a') * (2.0 / 24.0)) + (1.0 / 24.0);
            lat = (field_lat * 10.0) - 90.0 + (square_lat * 1.0) +
                  ((sub_lat - 'a') * (1.0 / 24.0)) + (0.5 / 24.0);
        }
    }

    return true;
}

double FrequencyManager::gridDistanceKm(const std::string& from_grid, const std::string& to_grid) {
    double from_lat = 0.0;
    double from_lon = 0.0;
    double to_lat = 0.0;
    double to_lon = 0.0;
    if (!gridSquareToLatLon(from_grid, from_lat, from_lon) ||
        !gridSquareToLatLon(to_grid, to_lat, to_lon)) {
        return -1.0;
    }

    const double lat1 = degToRad(from_lat);
    const double lon1 = degToRad(from_lon);
    const double lat2 = degToRad(to_lat);
    const double lon2 = degToRad(to_lon);
    const double dlat = lat2 - lat1;
    const double dlon = lon2 - lon1;

    const double a = (std::sin(dlat / 2.0) * std::sin(dlat / 2.0)) +
                     (std::cos(lat1) * std::cos(lat2) *
                      std::sin(dlon / 2.0) * std::sin(dlon / 2.0));
    const double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
    constexpr double kEarthRadiusKm = 6371.0;
    return kEarthRadiusKm * c;
}

PropagationEstimate FrequencyManager::estimatePropagationWindow(const std::string& from_grid,
                                                                const std::string& to_grid,
                                                                int local_hour) {
    PropagationEstimate est;
    const double distance_km = gridDistanceKm(from_grid, to_grid);
    if (distance_km <= 0.0) {
        return est;
    }

    est.valid = true;
    est.distance_km = distance_km;

    const int hour = std::clamp(local_hour, 0, 23);
    const double sun_factor = std::max(0.0, std::sin((static_cast<double>(hour) - 6.0) * kPi / 12.0));

    // Simple HF path model:
    // - LUF rises in daytime (D-layer absorption), especially for longer paths.
    // - MUF rises with sunlight and path length, then caps by practical HF limits.
    double luf = 1.8 + (distance_km / 4500.0) + (sun_factor * 2.6);
    if (distance_km < 650.0) {
        luf -= 0.4;
    }
    luf = std::clamp(luf, 1.8, 12.0);

    double muf = 4.2 + (sun_factor * 15.0) + (distance_km / 520.0);
    if (distance_km < 700.0) {
        muf = std::min(muf, 12.0 + (sun_factor * 5.0));
    }
    if (distance_km > 3000.0) {
        muf += 1.2;
    }
    muf = std::clamp(muf, luf + 0.6, 30.0);

    est.luf_mhz = luf;
    est.muf_mhz = muf;
    return est;
}

double FrequencyManager::scoreBandForPropagation(const std::string& band,
                                                 const PropagationEstimate& estimate) const {
    auto it = bands_.find(band);
    if (it == bands_.end() || !estimate.valid) {
        return -12.0;
    }

    const double freq_mhz = static_cast<double>(it->second.calling_freq_hz) / 1'000'000.0;
    const double luf = estimate.luf_mhz;
    const double muf = estimate.muf_mhz;
    const double center = (luf + muf) * 0.5;

    double score = 0.0;
    if (freq_mhz >= luf && freq_mhz <= muf) {
        score += 9.0;
    } else if (freq_mhz < luf) {
        score -= (luf - freq_mhz) * 4.0;
    } else {
        score -= (freq_mhz - muf) * 2.8;
    }

    score -= std::abs(freq_mhz - center) * 1.2;

    // Distance sanity nudges for HF behavior.
    if (estimate.distance_km < 650.0 && freq_mhz > 14.5) {
        score -= 3.0;
    }
    if (estimate.distance_km > 2500.0 && freq_mhz < 7.0) {
        score -= 3.0;
    }
    if (estimate.distance_km > 3500.0 && freq_mhz < 10.0) {
        score -= 2.0;
    }

    return score;
}

bool FrequencyManager::qsyToCallingFrequency(RadioController& radio, const std::string& band) {
    uint64_t freq = getCallingFrequency(band);
    if (freq == 0) {
        std::cerr << "[FreqManager] Unknown band: " << band << "\n";
        return false;
    }

    if (!radio.setFrequency(freq)) {
        std::cerr << "[FreqManager] Failed to set frequency to " << freq << " Hz\n";
        return false;
    }

    return true;
}

bool FrequencyManager::qsyToSlot(RadioController& radio, const std::string& band, int slot) {
    uint64_t freq = getSlotFrequency(band, slot);
    if (freq == 0) {
        std::cerr << "[FreqManager] Invalid slot " << slot << " for band " << band << "\n";
        return false;
    }

    if (!radio.setFrequency(freq)) {
        std::cerr << "[FreqManager] Failed to set frequency to " << freq << " Hz\n";
        return false;
    }

    updateSlotStatus(band, slot, SlotStatus::MySlot, "");
    return true;
}

std::string FrequencyManager::encodeSlotId(const std::string& band, int slot) const {
    // Format: BAND-SLOT (e.g., "40M+3" or "20M-2")
    std::string result = band;
    // Remove 'm' suffix for compactness
    if (!result.empty() && result.back() == 'm') {
        result.pop_back();
        result += 'M';
    }

    if (slot >= 0) {
        result += '+';
    }
    result += std::to_string(slot);

    return result;
}

std::optional<std::pair<std::string, int>> FrequencyManager::decodeSlotId(const std::string& slot_id) const {
    // Parse format: BAND[+-]SLOT
    size_t sign_pos = slot_id.find_first_of("+-");
    if (sign_pos == std::string::npos) {
        return std::nullopt;
    }

    std::string band_part = slot_id.substr(0, sign_pos);
    std::string slot_part = slot_id.substr(sign_pos);

    // Convert band format (e.g., "40M" -> "40m")
    if (!band_part.empty() && band_part.back() == 'M') {
        band_part.back() = 'm';
    }

    try {
        int slot = std::stoi(slot_part);
        return std::make_pair(band_part, slot);
    } catch (...) {
        return std::nullopt;
    }
}

void FrequencyManager::startSlotScan(const std::string& band) {
    scanning_ = true;
    scanning_band_ = band;

    // Reset all slots to Unknown
    auto it = slots_.find(band);
    if (it != slots_.end()) {
        for (auto& s : it->second) {
            s.status = SlotStatus::Unknown;
            s.occupant.clear();
        }
    }

}

void FrequencyManager::stopSlotScan() {
    scanning_ = false;
    scanning_band_.clear();
}

bool FrequencyManager::saveConfig(const std::string& path) const {
    std::ofstream file(path);
    if (!file) {
        return false;
    }

    file << "# Skywave Link Frequency Configuration\n\n";

    for (const auto& [name, config] : bands_) {
        file << "[band." << name << "]\n";
        file << "enabled = " << (config.enabled ? "true" : "false") << "\n";
        file << "calling_freq_hz = " << config.calling_freq_hz << "\n";
        file << "\n";
    }

    return true;
}

bool FrequencyManager::loadConfig(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }

    // Simple config parsing
    std::string line;
    std::string current_band;

    while (std::getline(file, line)) {
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') continue;

        // Check for section header
        if (line[0] == '[') {
            size_t end = line.find(']');
            if (end != std::string::npos) {
                std::string section = line.substr(1, end - 1);
                if (section.rfind("band.", 0) == 0) {
                    current_band = section.substr(5);
                }
            }
            continue;
        }

        // Parse key = value
        size_t eq = line.find('=');
        if (eq != std::string::npos && !current_band.empty()) {
            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);

            // Trim whitespace
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            auto it = bands_.find(current_band);
            if (it != bands_.end()) {
                if (key == "enabled") {
                    it->second.enabled = (value == "true");
                } else if (key == "calling_freq_hz") {
                    try {
                        it->second.calling_freq_hz = std::stoull(value);
                    } catch (const std::exception& ex) {
                        std::cerr << "[FreqManager] Invalid calling_freq_hz for band "
                                  << current_band << ": '" << value << "' (" << ex.what() << ")\n";
                    }
                }
            }
        }
    }

    // Regenerate slot frequencies from updated band configs
    for (const auto& [name, config] : bands_) {
        auto sit = slots_.find(name);
        if (sit != slots_.end()) {
            for (auto& slot : sit->second) {
                slot.frequency_hz = config.calling_freq_hz +
                    (slot.slot_number * config.slot_spacing_hz);
            }
        }
    }

    return true;
}

} // namespace skylink
