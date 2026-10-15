// FrequencyManager tests
#include "test_macros.hpp"
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

TEST_MAIN()
