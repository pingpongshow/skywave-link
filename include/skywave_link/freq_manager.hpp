// FrequencyManager - HF band and channel management
// Handles calling frequencies, working slots, and QSY operations

#pragma once

#include <string>
#include <vector>
#include <chrono>
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
    int64_t last_checked;       // Unix timestamp of the last status update
    int64_t last_busy = 0;      // Unix timestamp it was last seen busy (0: never)
};

// What chooseBestSlot() found.
struct BestSlotChoice {
    int slot = 1;               // the slot to use (never 0, the calling frequency)
    bool known_clear = false;   // fresh scan/heard data says it is clear
    bool all_busy = false;      // every slot was busy recently; `slot` is the
                                // least recently busy one
    std::string reason;         // one line for the operator
};

// One step of the slot scanner: what the caller must do next.
struct SlotScanStep {
    enum class Action {
        None,       // keep listening (or no scan running)
        Tune,       // tune to frequency_hz (slot `slot`), then keep calling update
        Done,       // scan finished: tune back to frequency_hz (the start frequency)
    };
    Action action = Action::None;
    uint64_t frequency_hz = 0;
    int slot = 0;
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
    void updateSlotStatus(const std::string& band, int slot, SlotStatus status,
                          const std::string& occupant, int64_t now_unix);
    // A station was heard on `freq_hz` (e.g. a monitored frame while tuned
    // there): the slot it falls in is marked busy. False when the frequency
    // is not within half a slot spacing of any slot.
    bool noteActivity(uint64_t freq_hz, const std::string& occupant, int64_t now_unix);

    // Slot selection. Slot statuses older than kSlotFreshSec count as
    // unknown. Prefers a slot known clear, then an unknown one; among those
    // the least recently busy, then the nearest to the calling frequency.
    static constexpr int64_t kSlotFreshSec = 15 * 60;
    BestSlotChoice chooseBestSlot(const std::string& band, int64_t now_unix) const;
    BestSlotChoice chooseBestSlot(const std::string& band) const;
    int findBestSlot(const std::string& band) const { return chooseBestSlot(band).slot; }

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

    // Slot scanner (the app's channel sniffer drives it): tune to each slot
    // of `band` in turn, listen for `dwell`, and record busy/clear from the
    // modem's busy flag sampled on every update() after `settle` (the busy
    // detector needs ~2-3 s to forget the previous frequency). Any busy
    // sample marks the slot InUse; a slot with no sample after settling stays
    // Unknown. The caller performs the tunes; on a failed tune it calls
    // abortSlotScan() and returns to the frequency that gives back.
    static constexpr std::chrono::milliseconds kDefaultScanDwell{6000};
    static constexpr std::chrono::milliseconds kDefaultScanSettle{2500};
    // First step: Tune to the first slot (Action::None if the band is unknown).
    SlotScanStep beginSlotScan(const std::string& band, uint64_t return_frequency_hz,
                               std::chrono::steady_clock::time_point now,
                               std::chrono::milliseconds dwell = kDefaultScanDwell,
                               std::chrono::milliseconds settle = kDefaultScanSettle);
    SlotScanStep updateSlotScan(bool busy_now, std::chrono::steady_clock::time_point now,
                                int64_t now_unix);
    // Stops a scan; returns the frequency to go back to (nullopt if none ran).
    std::optional<uint64_t> abortSlotScan();
    bool isScanningSlots() const { return scan_.active; }
    std::string scanningBand() const { return scan_.band; }
    int scanningSlot() const;            // slot being listened to; 0 when idle

private:
    // Default HF amateur bands
    void initializeDefaultBands();

    std::map<std::string, BandConfig> bands_;
    std::map<std::string, std::vector<WorkingSlot>> slots_;

    struct ScanState {
        bool active = false;
        std::string band;
        size_t index = 0;
        uint64_t return_frequency_hz = 0;
        std::chrono::milliseconds dwell{0};
        std::chrono::milliseconds settle{0};
        std::chrono::steady_clock::time_point slot_started{};
        int samples = 0;
        bool busy = false;
    };
    ScanState scan_;
    WorkingSlot* findSlot(const std::string& band, int slot);
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
