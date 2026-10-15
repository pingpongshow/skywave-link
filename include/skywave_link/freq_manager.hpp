// FrequencyManager - HF band and channel management
// Handles calling frequencies, working slots, and QSY operations

#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <cstdint>

namespace skylink {

// Forward declaration
class RadioController;

// Band configuration
struct BandConfig {
    std::string name;           // "40m"
    std::string description;    // "40 Meter Band"
    uint64_t calling_freq_hz;   // 7055000
    uint64_t slot_spacing_hz;   // 500
    int num_slots;              // 12 (±6 from calling freq)
    uint64_t band_start_hz;     // 7000000
    uint64_t band_end_hz;       // 7300000
    bool enabled;               // User can disable bands
};

// Slot status
enum class SlotStatus {
    Unknown,
    Available,
    InUse,
    MySlot
};

// Working slot info
struct WorkingSlot {
    std::string band;
    int slot_number;            // -6 to +6 (0 = calling freq)
    uint64_t frequency_hz;
    SlotStatus status;
    std::string occupant;       // Callsign if known
    int64_t last_checked;       // Unix timestamp
};

struct PropagationEstimate {
    bool valid = false;
    double distance_km = 0.0;
    double luf_mhz = 0.0;
    double muf_mhz = 0.0;
};

class FrequencyManager {
public:
    FrequencyManager();
    ~FrequencyManager();

    // Band configuration
    std::vector<BandConfig> getBands() const;
    std::optional<BandConfig> getBand(const std::string& name) const;
    bool setBandEnabled(const std::string& name, bool enabled);

    // Frequency calculations
    uint64_t getCallingFrequency(const std::string& band) const;
    uint64_t getSlotFrequency(const std::string& band, int slot) const;
    int frequencyToSlot(const std::string& band, uint64_t freq_hz) const;
    std::optional<std::string> frequencyToBand(uint64_t freq_hz) const;

    // Slot management
    std::vector<WorkingSlot> getSlots(const std::string& band) const;
    std::optional<WorkingSlot> getSlot(const std::string& band, int slot) const;
    void updateSlotStatus(const std::string& band, int slot, SlotStatus status,
                          const std::string& occupant = "");

    // Slot selection
    int findAvailableSlot(const std::string& band) const;
    int findBestSlot(const std::string& band) const;

    // Propagation helpers for band suggestions.
    static bool gridSquareToLatLon(const std::string& grid, double& lat, double& lon);
    static double gridDistanceKm(const std::string& from_grid, const std::string& to_grid);
    static PropagationEstimate estimatePropagationWindow(const std::string& from_grid,
                                                         const std::string& to_grid,
                                                         int local_hour);
    double scoreBandForPropagation(const std::string& band,
                                   const PropagationEstimate& estimate) const;

    // QSY operations (requires local radio control)
    bool qsyToCallingFrequency(RadioController& radio, const std::string& band);
    bool qsyToSlot(RadioController& radio, const std::string& band, int slot);

    // Slot ID encoding/decoding (for CQ messages)
    std::string encodeSlotId(const std::string& band, int slot) const;
    std::optional<std::pair<std::string, int>> decodeSlotId(const std::string& slot_id) const;

    // Sniffer support
    void startSlotScan(const std::string& band);
    void stopSlotScan();
    bool isScanningSlots() const { return scanning_; }

    // Persistence
    bool saveConfig(const std::string& path) const;
    bool loadConfig(const std::string& path);

private:
    // Default HF amateur bands
    void initializeDefaultBands();

    std::map<std::string, BandConfig> bands_;
    std::map<std::string, std::vector<WorkingSlot>> slots_;
    bool scanning_ = false;
    std::string scanning_band_;
};

// Default calling frequencies (ITU Region 2 / Americas focused)
// These can be customized by region
namespace DefaultFrequencies {
    constexpr uint64_t BAND_80M = 3590000;   // 3.590 MHz
    constexpr uint64_t BAND_60M = 5357000;   // 5.357 MHz (channel 3)
    constexpr uint64_t BAND_40M = 7055000;   // 7.055 MHz
    constexpr uint64_t BAND_30M = 10130000;  // 10.130 MHz
    constexpr uint64_t BAND_20M = 14105000;  // 14.105 MHz
    constexpr uint64_t BAND_17M = 18105000;  // 18.105 MHz
    constexpr uint64_t BAND_15M = 21105000;  // 21.105 MHz
    constexpr uint64_t BAND_12M = 24925000;  // 24.925 MHz
    constexpr uint64_t BAND_10M = 28120000;  // 28.120 MHz
}

} // namespace skylink
