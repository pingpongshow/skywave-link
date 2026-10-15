// ChannelScanner - Multi-band scanning with history and analysis

#include "skywave_link/channel_scanner.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>

namespace skylink {

static constexpr float NAN_SENTINEL = -999.0f;
static constexpr float NAN_SENTINEL_THRESHOLD = -998.0f;

ChannelScanner::ChannelScanner() = default;

std::string ChannelScanner::makeKey(const std::string& band, int slot) {
    return band + ":" + std::to_string(slot);
}

// SlotHistory helpers
float SlotHistory::busyPercent() const {
    if (entries.empty()) return 0.0f;
    int busy = 0;
    for (const auto& e : entries) {
        if (e.busy) busy++;
    }
    return 100.0f * static_cast<float>(busy) / static_cast<float>(entries.size());
}

float SlotHistory::avgSNR() const {
    if (entries.empty()) return 0.0f;
    float sum = 0.0f;
    int count = 0;
    for (const auto& e : entries) {
        if (!std::isnan(e.snr_db)) {
            sum += e.snr_db;
            count++;
        }
    }
    return count > 0 ? sum / static_cast<float>(count) : NAN;
}

// Scan control

void ChannelScanner::startScan(const std::vector<std::string>& bands) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bands.empty()) {
        std::cerr << "ChannelScanner::startScan: empty bands vector, ignoring\n";
        return;
    }
    if (scanning_) {
        // Re-entry: stop current scan before starting new one
        std::cerr << "ChannelScanner::startScan: already scanning, restarting\n";
        scanning_ = false;
    }
    scan_bands_ = bands;
    current_band_idx_ = 0;
    current_slot_ = 0;
    scanning_ = true;
}

void ChannelScanner::stopScan() {
    std::lock_guard<std::mutex> lock(mutex_);
    scanning_ = false;
}

bool ChannelScanner::isScanning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return scanning_;
}

std::string ChannelScanner::currentBand() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (scan_bands_.empty() || current_band_idx_ >= static_cast<int>(scan_bands_.size()))
        return "";
    return scan_bands_[current_band_idx_];
}

int ChannelScanner::currentSlot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_slot_;
}

void ChannelScanner::recordResult(const SlotScanResult& result) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string key = makeKey(result.band, result.slot);
    auto& history = histories_[key];
    history.band = result.band;
    history.slot = result.slot;
    history.freq_hz = result.freq_hz;

    history.entries.push_front(result);
    if (static_cast<int>(history.entries.size()) > MAX_HISTORY_PER_SLOT) {
        history.entries.pop_back();
    }
}

std::vector<SlotScanResult> ChannelScanner::getLatestResults(const std::string& band) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SlotScanResult> results;

    for (const auto& [key, hist] : histories_) {
        if (hist.band == band && !hist.entries.empty()) {
            results.push_back(hist.entries.front());
        }
    }

    std::sort(results.begin(), results.end(),
              [](const SlotScanResult& a, const SlotScanResult& b) {
                  return a.slot < b.slot;
              });
    return results;
}

std::optional<SlotHistory> ChannelScanner::getSlotHistory(const std::string& band, int slot) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = histories_.find(makeKey(band, slot));
    if (it == histories_.end()) return std::nullopt;
    return it->second;
}

// NOTE: Returns by value, which deep-copies deques in each SlotHistory.
// This is acceptable since callers need an independent snapshot of the data,
// but be aware of the cost if histories are large.
std::vector<SlotHistory> ChannelScanner::getAllHistories(const std::string& band) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SlotHistory> results;
    for (const auto& [key, hist] : histories_) {
        if (hist.band == band) {
            results.push_back(hist);
        }
    }
    std::sort(results.begin(), results.end(),
              [](const SlotHistory& a, const SlotHistory& b) {
                  return a.slot < b.slot;
              });
    return results;
}

std::vector<BandPropagation> ChannelScanner::getBandPropagation(const std::string& band) const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Aggregate by hour
    std::map<int, BandPropagation> hourly;
    for (int h = 0; h < 24; ++h) {
        hourly[h].band = band;
        hourly[h].hour = h;
        hourly[h].avg_noise_db = 0;
        hourly[h].avg_snr_db = 0;
        hourly[h].total_scans = 0;
        hourly[h].busy_scans = 0;
    }

    // Track valid (non-NaN) counts separately for SNR and noise
    std::map<int, int> valid_snr_count;
    std::map<int, int> valid_noise_count;

    for (const auto& [key, hist] : histories_) {
        if (hist.band != band) continue;
        for (const auto& entry : hist.entries) {
            auto ts = static_cast<time_t>(entry.timestamp);
            struct tm tm_buf;
#ifdef _WIN32
            gmtime_s(&tm_buf, &ts);
#else
            gmtime_r(&ts, &tm_buf);
#endif
            int hour = tm_buf.tm_hour;
            auto& hp = hourly[hour];
            hp.total_scans++;
            if (entry.busy) hp.busy_scans++;
            if (!std::isnan(entry.noise_floor_db)) {
                hp.avg_noise_db += entry.noise_floor_db;
                valid_noise_count[hour]++;
            }
            if (!std::isnan(entry.snr_db)) {
                hp.avg_snr_db += entry.snr_db;
                valid_snr_count[hour]++;
            }
        }
    }

    std::vector<BandPropagation> result;
    for (auto& [hour, hp] : hourly) {
        if (hp.total_scans > 0) {
            int snr_n = valid_snr_count[hour];
            int noise_n = valid_noise_count[hour];
            hp.avg_snr_db = snr_n > 0 ? hp.avg_snr_db / snr_n : 0.0f;
            hp.avg_noise_db = noise_n > 0 ? hp.avg_noise_db / noise_n : 0.0f;
            hp.activity_pct = 100.0f * static_cast<float>(hp.busy_scans) /
                              static_cast<float>(hp.total_scans);
        }
        result.push_back(hp);
    }
    return result;
}

HeatmapData ChannelScanner::getHeatmap(const std::string& band) const {
    std::lock_guard<std::mutex> lock(mutex_);
    HeatmapData heatmap;
    heatmap.band = band;
    heatmap.num_hours = 24;

    // Find all slots for this band
    std::vector<int> slots;
    for (const auto& [key, hist] : histories_) {
        if (hist.band == band) {
            slots.push_back(hist.slot);
        }
    }
    std::sort(slots.begin(), slots.end());
    heatmap.num_slots = static_cast<int>(slots.size());

    // Build cells
    for (int slot : slots) {
        // Count busy entries per hour for this slot
        std::map<int, std::pair<int, int>> hour_counts;  // hour -> (busy, total)
        auto it = histories_.find(makeKey(band, slot));
        if (it == histories_.end()) continue;

        for (const auto& entry : it->second.entries) {
            auto ts = static_cast<time_t>(entry.timestamp);
            struct tm tm_buf;
#ifdef _WIN32
            gmtime_s(&tm_buf, &ts);
#else
            gmtime_r(&ts, &tm_buf);
#endif
            int hour = tm_buf.tm_hour;
            hour_counts[hour].second++;
            if (entry.busy) hour_counts[hour].first++;
        }

        for (int h = 0; h < 24; ++h) {
            HeatmapCell cell;
            cell.slot = slot;
            cell.hour = h;
            auto hc = hour_counts.find(h);
            if (hc != hour_counts.end() && hc->second.second > 0) {
                cell.activity = static_cast<float>(hc->second.first) /
                                static_cast<float>(hc->second.second);
            } else {
                cell.activity = 0.0f;
            }
            heatmap.cells.push_back(cell);
        }
    }

    return heatmap;
}

float ChannelScanner::scoreSlot(const SlotHistory& h) const {
    if (h.entries.empty()) return 50.0f;  // Unknown = neutral

    // Score based on: low busy%, low noise, recent data
    float busy_pct = h.busyPercent();
    float busy_score = 100.0f - busy_pct;  // 100 if never busy, 0 if always busy

    // Recency score: 0-100 based on how recently scanned and whether free
    float recency_score = 0.0f;
    if (!h.entries.empty()) {
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        int64_t age = now - h.entries.front().timestamp;
        if (age < 300 && !h.entries.front().busy) {
            recency_score = 100.0f;  // Scanned within 5 min and free
        } else if (age < 3600 && !h.entries.front().busy) {
            recency_score = 50.0f;   // Within 1 hour and free
        }
    }

    // Weight: 70% historical availability, 30% recency
    return std::min(100.0f, busy_score * 0.7f + recency_score * 0.3f);
}

std::vector<SlotRecommendation> ChannelScanner::recommendSlots(const std::string& band,
                                                                int count) const {
    if (count <= 0) return {};

    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SlotRecommendation> recs;

    for (const auto& [key, hist] : histories_) {
        if (hist.band != band) continue;

        SlotRecommendation rec;
        rec.band = band;
        rec.slot = hist.slot;
        rec.freq_hz = hist.freq_hz;
        rec.score = scoreSlot(hist);

        if (hist.busyPercent() < 10.0f) {
            rec.reason = "Rarely busy";
        } else if (hist.busyPercent() < 30.0f) {
            rec.reason = "Usually available";
        } else if (hist.busyPercent() < 60.0f) {
            rec.reason = "Sometimes available";
        } else {
            rec.reason = "Frequently busy";
        }

        recs.push_back(rec);
    }

    std::sort(recs.begin(), recs.end(),
              [](const SlotRecommendation& a, const SlotRecommendation& b) {
                  return a.score > b.score;
              });

    if (static_cast<int>(recs.size()) > count) {
        recs.resize(count);
    }
    return recs;
}

bool ChannelScanner::saveHistory(const std::string& path) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::ofstream out(path);
    if (!out) return false;

    // Simple text format: band,slot,freq,busy,snr,noise,timestamp per line
    for (const auto& [key, hist] : histories_) {
        for (const auto& e : hist.entries) {
            // Use -999 as sentinel for NaN/Inf values for portable save/load
            float save_snr = std::isfinite(e.snr_db) ? e.snr_db : NAN_SENTINEL;
            float save_noise = std::isfinite(e.noise_floor_db) ? e.noise_floor_db : NAN_SENTINEL;
            out << e.band << "," << e.slot << "," << e.freq_hz << ","
                << (e.busy ? 1 : 0) << "," << save_snr << ","
                << save_noise << "," << e.timestamp << "\n";
        }
    }

    out.flush();
    return out.good();
}

bool ChannelScanner::loadHistory(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::ifstream in(path);
    if (!in) return false;

    // Intentionally clear all existing data: load replaces in-memory state
    histories_.clear();
    std::string line;
    int non_empty_lines = 0;
    int parsed_count = 0;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        non_empty_lines++;
        std::istringstream ss(line);
        SlotScanResult r;
        char comma;
        int busy_int;

        if (std::getline(ss, r.band, ',') &&
            ss >> r.slot >> comma >> r.freq_hz >> comma >>
            busy_int >> comma >> r.snr_db >> comma >>
            r.noise_floor_db >> comma >> r.timestamp) {
            r.busy = busy_int != 0;
            if (r.snr_db <= NAN_SENTINEL_THRESHOLD) r.snr_db = NAN;
            if (r.noise_floor_db <= NAN_SENTINEL_THRESHOLD) r.noise_floor_db = NAN;
            parsed_count++;

            std::string key = makeKey(r.band, r.slot);
            auto& hist = histories_[key];
            hist.band = r.band;
            hist.slot = r.slot;
            hist.freq_hz = r.freq_hz;

            // File is written newest-first; push_back preserves that order.
            // At capacity, skip older entries (don't add them).
            if (static_cast<int>(hist.entries.size()) < MAX_HISTORY_PER_SLOT) {
                hist.entries.push_back(r);
            }
        }
    }

    // Fail if file had content but nothing could be parsed
    if (non_empty_lines > 0 && parsed_count == 0) {
        return false;
    }
    return true;
}

void ChannelScanner::pruneOldEntries(int64_t max_age_sec) {
    if (max_age_sec <= 0) return;

    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t cutoff = now - max_age_sec;

    for (auto& [key, hist] : histories_) {
        auto new_end = std::remove_if(hist.entries.begin(), hist.entries.end(),
            [cutoff](const SlotScanResult& e) {
                return e.timestamp < cutoff;
            });
        hist.entries.erase(new_end, hist.entries.end());
    }

    // Remove empty histories
    for (auto it = histories_.begin(); it != histories_.end(); ) {
        if (it->second.entries.empty()) {
            it = histories_.erase(it);
        } else {
            ++it;
        }
    }
}

void ChannelScanner::advanceSlot(int total_slots) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (total_slots <= 0) return;
    if (!scanning_ || scan_bands_.empty()) return;

    current_slot_++;
    if (current_slot_ >= total_slots) {
        current_slot_ = 0;
        current_band_idx_++;
        if (current_band_idx_ >= static_cast<int>(scan_bands_.size())) {
            current_band_idx_ = 0;  // Wrap around to first band
        }
    }
}

} // namespace skylink
