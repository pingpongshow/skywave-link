// FrequencyManager tests
#include "test_macros.hpp"
#include <chrono>
#include "skywave_link/freq_manager.hpp"

TEST(freq_default_bands) {
    skylink::FrequencyManager fm;
    auto bands = fm.getBands();
    ASSERT_EQ(bands.size(), (size_t)9);
    PASS();
}

TEST(freq_calling_frequencies) {
    skylink::FrequencyManager fm;
    ASSERT_EQ(fm.getCallingFrequency("80m"), skylink::DefaultFrequencies::BAND_80M);
    ASSERT_EQ(fm.getCallingFrequency("60m"), skylink::DefaultFrequencies::BAND_60M);
    ASSERT_EQ(fm.getCallingFrequency("40m"), skylink::DefaultFrequencies::BAND_40M);
    ASSERT_EQ(fm.getCallingFrequency("30m"), skylink::DefaultFrequencies::BAND_30M);
    ASSERT_EQ(fm.getCallingFrequency("20m"), skylink::DefaultFrequencies::BAND_20M);
    ASSERT_EQ(fm.getCallingFrequency("17m"), skylink::DefaultFrequencies::BAND_17M);
    ASSERT_EQ(fm.getCallingFrequency("15m"), skylink::DefaultFrequencies::BAND_15M);
    ASSERT_EQ(fm.getCallingFrequency("12m"), skylink::DefaultFrequencies::BAND_12M);
    ASSERT_EQ(fm.getCallingFrequency("10m"), skylink::DefaultFrequencies::BAND_10M);
    PASS();
}

TEST(freq_unknown_band) {
    skylink::FrequencyManager fm;
    ASSERT_EQ(fm.getCallingFrequency("6m"), (uint64_t)0);
    auto band = fm.getBand("nonexistent");
    ASSERT_FALSE(band.has_value());
    PASS();
}

TEST(freq_slot_frequency_40m) {
    skylink::FrequencyManager fm;
    // 40m calling freq = 7055000, slot spacing = 500
    // slot +3 = 7055000 + 3*500 = 7056500
    ASSERT_EQ(fm.getSlotFrequency("40m", 3), (uint64_t)7056500);
    // slot 0 = calling freq
    ASSERT_EQ(fm.getSlotFrequency("40m", 0), (uint64_t)7055000);
    // slot -2 = 7055000 - 1000 = 7054000
    ASSERT_EQ(fm.getSlotFrequency("40m", -2), (uint64_t)7054000);
    PASS();
}

TEST(freq_slot_out_of_range) {
    skylink::FrequencyManager fm;
    // 40m has 12 slots (±6), so slot ±7 should return 0
    ASSERT_EQ(fm.getSlotFrequency("40m", 7), (uint64_t)0);
    ASSERT_EQ(fm.getSlotFrequency("40m", -7), (uint64_t)0);
    // But ±6 should be valid
    ASSERT_NE(fm.getSlotFrequency("40m", 6), (uint64_t)0);
    ASSERT_NE(fm.getSlotFrequency("40m", -6), (uint64_t)0);
    PASS();
}

TEST(freq_frequency_to_slot) {
    skylink::FrequencyManager fm;
    // 7056500 = 40m calling + 3*500 = slot +3
    ASSERT_EQ(fm.frequencyToSlot("40m", 7056500), 3);
    // calling freq = slot 0
    ASSERT_EQ(fm.frequencyToSlot("40m", 7055000), 0);
    // 7054000 = slot -2
    ASSERT_EQ(fm.frequencyToSlot("40m", 7054000), -2);
    // Nearest slot, not truncation toward the calling frequency.
    ASSERT_EQ(fm.frequencyToSlot("40m", 7054700), -1);
    ASSERT_EQ(fm.frequencyToSlot("40m", 7055300), 1);
    PASS();
}

TEST(freq_frequency_to_slot_clamped) {
    skylink::FrequencyManager fm;
    // Bug fix test: frequency far out of range should clamp to valid slot range
    // 40m has ±6 slots, so a freq 100 slots away should clamp to 6
    uint64_t far_freq = 7055000 + 100 * 500;  // way above
    int slot = fm.frequencyToSlot("40m", far_freq);
    ASSERT_EQ(slot, 6);  // clamped to max

    uint64_t far_below = 7055000 - 100 * 500;  // way below
    slot = fm.frequencyToSlot("40m", far_below);
    ASSERT_EQ(slot, -6);  // clamped to min
    PASS();
}

TEST(freq_frequency_to_band) {
    skylink::FrequencyManager fm;
    auto band = fm.frequencyToBand(7100000);
    ASSERT_TRUE(band.has_value());
    ASSERT_EQ(*band, std::string("40m"));

    // Out of any band
    auto none = fm.frequencyToBand(1000000);
    ASSERT_FALSE(none.has_value());
    PASS();
}

TEST(freq_slot_encode_decode) {
    skylink::FrequencyManager fm;
    // Encode
    std::string id = fm.encodeSlotId("40m", 3);
    ASSERT_EQ(id, std::string("40M+3"));

    // Decode
    auto decoded = fm.decodeSlotId("40M+3");
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->first, std::string("40m"));
    ASSERT_EQ(decoded->second, 3);

    // Negative slot
    id = fm.encodeSlotId("20m", -2);
    ASSERT_EQ(id, std::string("20M-2"));

    decoded = fm.decodeSlotId("20M-2");
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->first, std::string("20m"));
    ASSERT_EQ(decoded->second, -2);
    PASS();
}

TEST(freq_slot_decode_invalid) {
    skylink::FrequencyManager fm;
    auto decoded = fm.decodeSlotId("invalid");
    ASSERT_FALSE(decoded.has_value());

    decoded = fm.decodeSlotId("");
    ASSERT_FALSE(decoded.has_value());
    PASS();
}

TEST(freq_band_enable_disable) {
    skylink::FrequencyManager fm;
    auto band = fm.getBand("40m");
    ASSERT_TRUE(band.has_value());
    ASSERT_TRUE(band->enabled);

    ASSERT_TRUE(fm.setBandEnabled("40m", false));
    band = fm.getBand("40m");
    ASSERT_FALSE(band->enabled);

    ASSERT_TRUE(fm.setBandEnabled("40m", true));
    band = fm.getBand("40m");
    ASSERT_TRUE(band->enabled);

    // Unknown band
    ASSERT_FALSE(fm.setBandEnabled("6m", true));
    PASS();
}

TEST(freq_slot_status) {
    skylink::FrequencyManager fm;
    fm.updateSlotStatus("40m", 3, skylink::SlotStatus::InUse, "W1ABC");
    auto slot = fm.getSlot("40m", 3);
    ASSERT_TRUE(slot.has_value());
    ASSERT_EQ(static_cast<int>(slot->status), static_cast<int>(skylink::SlotStatus::InUse));
    ASSERT_EQ(slot->occupant, std::string("W1ABC"));
    PASS();
}

TEST(freq_get_slots) {
    skylink::FrequencyManager fm;
    // 40m has 12 slots = 13 entries (-6 to +6 inclusive)
    auto slots = fm.getSlots("40m");
    ASSERT_EQ(slots.size(), (size_t)13);

    // Empty for unknown band
    auto empty = fm.getSlots("nonexistent");
    ASSERT_TRUE(empty.empty());
    PASS();
}

TEST(freq_grid_to_latlon) {
    double lat = 0.0;
    double lon = 0.0;
    ASSERT_TRUE(skylink::FrequencyManager::gridSquareToLatLon("FN42ig", lat, lon));
    ASSERT_NEAR(lat, 42.27, 0.8);
    ASSERT_NEAR(lon, -71.62, 0.8);
    ASSERT_FALSE(skylink::FrequencyManager::gridSquareToLatLon("BAD", lat, lon));
    PASS();
}

TEST(freq_grid_distance) {
    const double d1 = skylink::FrequencyManager::gridDistanceKm("FN42ig", "EM48ss");
    ASSERT_GT(d1, 1200.0);
    ASSERT_LT(d1, 2200.0);

    const double d2 = skylink::FrequencyManager::gridDistanceKm("FN42ig", "FN42ig");
    ASSERT_LT(d2, 5.0);
    PASS();
}

TEST(freq_propagation_window_and_band_score) {
    skylink::FrequencyManager fm;
    auto day = skylink::FrequencyManager::estimatePropagationWindow("FN42ig", "DM79", 13);
    ASSERT_TRUE(day.valid);
    ASSERT_GT(day.distance_km, 2000.0);
    ASSERT_LT(day.luf_mhz, day.muf_mhz);

    const double score_20m_day = fm.scoreBandForPropagation("20m", day);
    const double score_80m_day = fm.scoreBandForPropagation("80m", day);
    ASSERT_GT(score_20m_day, score_80m_day);

    auto night = skylink::FrequencyManager::estimatePropagationWindow("FN42ig", "EM10", 1);
    ASSERT_TRUE(night.valid);
    const double score_40m_night = fm.scoreBandForPropagation("40m", night);
    const double score_10m_night = fm.scoreBandForPropagation("10m", night);
    ASSERT_GT(score_40m_night, score_10m_night);
    PASS();
}

// K11: best slot uses real data. With nothing known it says so (and picks
// the nearest slot); a fresh clear reading wins; busy slots are avoided;
// stale readings count as unknown; the least recently busy slot wins ties.
TEST(freq_best_slot_uses_scan_data) {
    skylink::FrequencyManager fm;
    const int64_t t = 1700000000;
    auto c = fm.chooseBestSlot("40m", t);
    ASSERT_EQ(c.slot, 1);
    ASSERT_FALSE(c.known_clear);
    ASSERT_CONTAINS(c.reason, "sniffer");

    fm.updateSlotStatus("40m", 1, skylink::SlotStatus::InUse, "K1BBB", t);
    fm.updateSlotStatus("40m", -1, skylink::SlotStatus::InUse, "", t);
    fm.updateSlotStatus("40m", 4, skylink::SlotStatus::Available, "", t);
    c = fm.chooseBestSlot("40m", t + 60);
    ASSERT_EQ(c.slot, 4);
    ASSERT_TRUE(c.known_clear);
    ASSERT_EQ(fm.findBestSlot("40m") != 0, true);

    // The clear reading goes stale: unknown slots never seen busy win over
    // ones that were busy, nearest first.
    c = fm.chooseBestSlot("40m", t + skylink::FrequencyManager::kSlotFreshSec + 61);
    ASSERT_FALSE(c.known_clear);
    ASSERT_EQ(c.slot, 2);

    // Everything busy: the least recently busy one, flagged.
    for (const auto& s : fm.getSlots("40m")) {
        if (s.slot_number != 0) {
            fm.updateSlotStatus("40m", s.slot_number, skylink::SlotStatus::InUse, "", t + 100);
        }
    }
    fm.updateSlotStatus("40m", -5, skylink::SlotStatus::InUse, "", t + 50);
    c = fm.chooseBestSlot("40m", t + 120);
    ASSERT_TRUE(c.all_busy);
    ASSERT_EQ(c.slot, -5);
    ASSERT_TRUE(c.slot != 0);
    PASS();
}

TEST(freq_note_activity_marks_slot) {
    skylink::FrequencyManager fm;
    const int64_t t = 1700000000;
    ASSERT_TRUE(fm.noteActivity(7056500 + 40, "K1BBB", t));  // slot +3, a little off
    auto s = fm.getSlot("40m", 3);
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s->status == skylink::SlotStatus::InUse);
    ASSERT_EQ(s->occupant, std::string("K1BBB"));
    ASSERT_EQ(s->last_busy, t);
    ASSERT_FALSE(fm.noteActivity(7056250, "K1BBB", t));  // between slots
    ASSERT_FALSE(fm.noteActivity(9000000, "K1BBB", t));  // no band
    PASS();
}

// K28 / A10: the one slot scanner. Busy is sampled over the whole dwell
// (after settling), every slot is visited, and the scan ends by returning
// to the start frequency.
TEST(freq_slot_scan_samples_whole_dwell_and_returns) {
    using namespace std::chrono;
    skylink::FrequencyManager fm;
    const auto slots = fm.getSlots("30m");
    ASSERT_TRUE(slots.size() >= 3);
    auto now = steady_clock::time_point{} + hours(1);
    const int64_t unix_now = 1700000000;
    auto step = fm.beginSlotScan("30m", 10136000, now, milliseconds(1000), milliseconds(400));
    ASSERT_TRUE(step.action == skylink::SlotScanStep::Action::Tune);
    ASSERT_EQ(step.frequency_hz, slots[0].frequency_hz);
    ASSERT_TRUE(fm.isScanningSlots());
    ASSERT_EQ(fm.scanningSlot(), slots[0].slot_number);

    size_t tunes = 1;
    bool done = false;
    for (int i = 0; i < 1000 && !done; ++i) {
        now += milliseconds(100);
        const size_t idx = tunes - 1;
        // Slot index 1 is busy only briefly in the middle of its dwell; the
        // busy flag right after tuning (inherited from the previous slot)
        // must be ignored for index 2.
        const auto in_slot = (i % 10) * 100;
        bool busy = (idx == 1 && in_slot == 600) || (idx == 2 && in_slot == 0);
        step = fm.updateSlotScan(busy, now, unix_now);
        if (step.action == skylink::SlotScanStep::Action::Tune) {
            ++tunes;
            ASSERT_EQ(step.frequency_hz, slots[tunes - 1].frequency_hz);
        } else if (step.action == skylink::SlotScanStep::Action::Done) {
            done = true;
            ASSERT_EQ(step.frequency_hz, (uint64_t)10136000);
        }
    }
    ASSERT_TRUE(done);
    ASSERT_EQ(tunes, slots.size());
    ASSERT_FALSE(fm.isScanningSlots());
    auto after = fm.getSlots("30m");
    ASSERT_TRUE(after[0].status == skylink::SlotStatus::Available);
    ASSERT_TRUE(after[1].status == skylink::SlotStatus::InUse);
    ASSERT_TRUE(after[2].status == skylink::SlotStatus::Available);

    // Abort hands back the start frequency.
    fm.beginSlotScan("30m", 10140000, now);
    auto back = fm.abortSlotScan();
    ASSERT_TRUE(back.has_value());
    ASSERT_EQ(*back, (uint64_t)10140000);
    ASSERT_FALSE(fm.abortSlotScan().has_value());
    ASSERT_TRUE(fm.beginSlotScan("99m", 1, now).action == skylink::SlotScanStep::Action::None);
    PASS();
}

TEST_MAIN()
