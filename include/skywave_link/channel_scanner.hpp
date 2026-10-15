// ChannelScanner - Multi-band channel scanning with history and propagation analysis
#pragma once

#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace skylink {

// Single slot scan result
struct SlotScanResult {
    std::string band;
    int slot;
    uint64_t freq_hz;
    bool busy;
    float snr_db;
    float noise_floor_db;
    int64_t timestamp;
};

// History ring buffer entry per slot
struct SlotHistory {
    std::string band;
    int slot;
    uint64_t freq_hz;
    std::deque<SlotScanResult> entries;  // Most recent first

    float busyPercent() const;
    float avgSNR() const;
};

// Per-band propagation summary (hourly aggregation)
struct BandPropagation {
    std::string band;
    int hour;            // 0-23 UTC
    float avg_noise_db;
    float avg_snr_db;
    int total_scans;
    int busy_scans;
    float activity_pct;  // busy_scans / total_scans * 100
};

// Heatmap data for UI rendering
struct HeatmapCell {
    int slot;
    int hour;
    float activity;  // 0.0 (empty) to 1.0 (always busy)
};

struct HeatmapData {
    std::string band;
    std::vector<HeatmapCell> cells;
    int num_slots;
    int num_hours;  // Always 24
};

// Slot recommendation for optimal channel selection
struct SlotRecommendation {
    std::string band;
    int slot;
    uint64_t freq_hz;
    float score;       // Higher = better (0-100)
    std::string reason;
};

class ChannelScanner {
public:
    ChannelScanner();
    ~ChannelScanner() = default;

    // Non-copyable
    ChannelScanner(const ChannelScanner&) = delete;
    ChannelScanner& operator=(const ChannelScanner&) = delete;

    // Scan control
    void startScan(const std::vector<std::string>& bands);
    void stopScan();
    bool isScanning() const;
    std::string currentBand() const;
    int currentSlot() const;

    // Advance to next slot (wraps to next band when total_slots reached)
    void advanceSlot(int total_slots);

    // Record a scan result (called per slot during scan)
    void recordResult(const SlotScanResult& result);

    // Query results
    std::vector<SlotScanResult> getLatestResults(const std::string& band) const;
    std::optional<SlotHistory> getSlotHistory(const std::string& band, int slot) const;
    std::vector<SlotHistory> getAllHistories(const std::string& band) const;

    // Propagation analysis
    std::vector<BandPropagation> getBandPropagation(const std::string& band) const;

    // Heatmap
    HeatmapData getHeatmap(const std::string& band) const;

    // Recommendations
    std::vector<SlotRecommendation> recommendSlots(const std::string& band,
                                                    int count = 3) const;

    // Persistence
    bool saveHistory(const std::string& path) const;
    bool loadHistory(const std::string& path);

    // Cleanup
    void pruneOldEntries(int64_t max_age_sec);

    // Configuration
    static constexpr int MAX_HISTORY_PER_SLOT = 1000;
    static constexpr int DWELL_MS = 3000;

private:
    mutable std::mutex mutex_;
    bool scanning_ = false;
    std::vector<std::string> scan_bands_;
    int current_band_idx_ = 0;
    int current_slot_ = 0;

    // Key: "band:slot"
    std::map<std::string, SlotHistory> histories_;

    static std::string makeKey(const std::string& band, int slot);
    float scoreSlot(const SlotHistory& h) const;
};

} // namespace skylink
