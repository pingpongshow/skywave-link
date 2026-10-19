// FrequencyManager implementation - HF band and channel management

#include "skywave_link/freq_manager.hpp"
#include "skywave_link/radio_controller.hpp"
#include <iostream>
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
    const double offset = static_cast<double>(static_cast<int64_t>(freq_hz) -
                                              static_cast<int64_t>(config.calling_freq_hz));
    // Nearest slot (truncation put 7.0549 MHz, just below 40 m calling, on 0
    // but 7.0551 on 0 too, and -749 Hz on -1 only by luck).
    const int slot = static_cast<int>(std::lround(offset / static_cast<double>(config.slot_spacing_hz)));
    const int half_slots = config.num_slots / 2;
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

namespace {
int64_t unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
}  // namespace

WorkingSlot* FrequencyManager::findSlot(const std::string& band, int slot) {
    auto it = slots_.find(band);
    if (it == slots_.end()) return nullptr;
    for (auto& s : it->second) {
        if (s.slot_number == slot) return &s;
    }
    return nullptr;
}

void FrequencyManager::updateSlotStatus(const std::string& band, int slot, SlotStatus status,
                                         const std::string& occupant) {
    updateSlotStatus(band, slot, status, occupant, unixNow());
}

void FrequencyManager::updateSlotStatus(const std::string& band, int slot, SlotStatus status,
                                         const std::string& occupant, int64_t now_unix) {
    WorkingSlot* s = findSlot(band, slot);
    if (!s) return;
    s->status = status;
    s->occupant = occupant;
    s->last_checked = now_unix;
    if (status == SlotStatus::InUse) s->last_busy = now_unix;
}

bool FrequencyManager::noteActivity(uint64_t freq_hz, const std::string& occupant,
                                    int64_t now_unix) {
    const auto band = frequencyToBand(freq_hz);
    if (!band) return false;
    const auto& cfg = bands_.at(*band);
    const int slot = frequencyToSlot(*band, freq_hz);
    const uint64_t slot_freq = getSlotFrequency(*band, slot);
    const uint64_t diff = slot_freq > freq_hz ? slot_freq - freq_hz : freq_hz - slot_freq;
    if (slot_freq == 0 || diff * 2 >= cfg.slot_spacing_hz) return false;
    updateSlotStatus(*band, slot, SlotStatus::InUse, occupant, now_unix);
    return true;
}

BestSlotChoice FrequencyManager::chooseBestSlot(const std::string& band) const {
    return chooseBestSlot(band, unixNow());
}

BestSlotChoice FrequencyManager::chooseBestSlot(const std::string& band, int64_t now_unix) const {
    BestSlotChoice choice;
    auto it = slots_.find(band);
    if (it == slots_.end()) {
        choice.reason = "Unknown band " + band;
        return choice;
    }

    // Rank: 0 known clear, 1 unknown (or stale), 2 busy recently.
    struct Candidate {
        int rank;
        int64_t last_busy;
        int slot;
    };
    std::vector<Candidate> candidates;
    for (const auto& s : it->second) {
        if (s.slot_number == 0) continue;  // the calling frequency is not a working slot
        const bool fresh = s.last_checked > 0 && now_unix - s.last_checked <= kSlotFreshSec;
        int rank = 1;
        if (fresh && (s.status == SlotStatus::Available || s.status == SlotStatus::MySlot)) {
            rank = 0;
        } else if (fresh && s.status == SlotStatus::InUse) {
            rank = 2;
        }
        candidates.push_back({rank, s.last_busy, s.slot_number});
    }
    if (candidates.empty()) {
        choice.reason = "Band " + band + " has no working slots";
        return choice;
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        if (a.last_busy != b.last_busy) return a.last_busy < b.last_busy;  // never busy first
        if (std::abs(a.slot) != std::abs(b.slot)) return std::abs(a.slot) < std::abs(b.slot);
        return a.slot > b.slot;  // +n before -n
    });
    const Candidate& best = candidates.front();
    choice.slot = best.slot;
    choice.known_clear = (best.rank == 0);
    choice.all_busy = (best.rank == 2);
    const std::string label = (best.slot > 0 ? "+" : "") + std::to_string(best.slot);
    if (best.rank == 0) {
        choice.reason = "Slot " + label + " was clear when last checked";
    } else if (best.rank == 1) {
        choice.reason = "No recent data for slot " + label +
                        " - run the channel sniffer to check the band";
    } else {
        choice.reason = "Every slot was busy recently; slot " + label +
                        " was busy least recently";
    }
    return choice;
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

SlotScanStep FrequencyManager::beginSlotScan(const std::string& band,
                                             uint64_t return_frequency_hz,
                                             std::chrono::steady_clock::time_point now,
                                             std::chrono::milliseconds dwell,
                                             std::chrono::milliseconds settle) {
    SlotScanStep step;
    auto it = slots_.find(band);
    if (it == slots_.end() || it->second.empty()) {
        scan_ = ScanState{};
        return step;
    }
    scan_ = ScanState{};
    scan_.active = true;
    scan_.band = band;
    scan_.index = 0;
    scan_.return_frequency_hz = return_frequency_hz;
    scan_.dwell = std::max(dwell, std::chrono::milliseconds(1));
    scan_.settle = std::clamp(settle, std::chrono::milliseconds(0), scan_.dwell);
    scan_.slot_started = now;
    step.action = SlotScanStep::Action::Tune;
    step.slot = it->second[0].slot_number;
    step.frequency_hz = it->second[0].frequency_hz;
    return step;
}

SlotScanStep FrequencyManager::updateSlotScan(bool busy_now,
                                              std::chrono::steady_clock::time_point now,
                                              int64_t now_unix) {
    SlotScanStep step;
    if (!scan_.active) return step;
    auto it = slots_.find(scan_.band);
    if (it == slots_.end() || scan_.index >= it->second.size()) {
        scan_ = ScanState{};
        return step;
    }
    const auto elapsed = now - scan_.slot_started;
    if (elapsed >= scan_.settle) {
        ++scan_.samples;
        scan_.busy = scan_.busy || busy_now;
    }
    if (elapsed < scan_.dwell) return step;  // keep listening

    WorkingSlot& slot = it->second[scan_.index];
    if (scan_.samples > 0) {
        updateSlotStatus(scan_.band, slot.slot_number,
                         scan_.busy ? SlotStatus::InUse : SlotStatus::Available, "", now_unix);
    }
    ++scan_.index;
    scan_.samples = 0;
    scan_.busy = false;
    scan_.slot_started = now;
    if (scan_.index >= it->second.size()) {
        step.action = SlotScanStep::Action::Done;
        step.frequency_hz = scan_.return_frequency_hz;
        scan_ = ScanState{};
        return step;
    }
    step.action = SlotScanStep::Action::Tune;
    step.slot = it->second[scan_.index].slot_number;
    step.frequency_hz = it->second[scan_.index].frequency_hz;
    return step;
}

std::optional<uint64_t> FrequencyManager::abortSlotScan() {
    if (!scan_.active) return std::nullopt;
    const uint64_t back = scan_.return_frequency_hz;
    scan_ = ScanState{};
    return back;
}

int FrequencyManager::scanningSlot() const {
    if (!scan_.active) return 0;
    auto it = slots_.find(scan_.band);
    if (it == slots_.end() || scan_.index >= it->second.size()) return 0;
    return it->second[scan_.index].slot_number;
}

} // namespace skylink
