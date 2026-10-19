// GUI helpers shared by the windows (gui_util.hpp), the SharedAppState
// result helpers the windows use to report App actions, and the window
// status line.

#include "test_macros.hpp"
#include "skywave_link/gui_util.hpp"
#include "skywave_link/shared_state.hpp"
#include "skywave_link/window_base.hpp"

#include <cstring>
#include <limits>

using namespace skylink;

namespace {

double readDoubleAt(const std::vector<uint8_t>& data, size_t offset) {
    double value = 0.0;
    std::memcpy(&value, data.data() + offset, sizeof(double));  // test hosts are little-endian
    return value;
}

ChatMessage systemLine(const std::string& text) {
    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = text;
    return msg;
}

class ProbeWindow : public WindowBase {
public:
    using WindowBase::WindowBase;
    void render() override {}
    const char* name() const override { return "probe"; }
};

}  // namespace

TEST(normalize_callsign_trims_and_uppercases) {
    ASSERT_EQ(gui::normalizeCallsign("  n0call-2 \t"), std::string("N0CALL-2"));
    ASSERT_EQ(gui::normalizeCallsign("   "), std::string());
    PASS();
}

TEST(normalize_settings_values) {
    ASSERT_EQ(gui::normalizeRadioBackend(" Hamlib "), std::string("hamlib"));
    ASSERT_EQ(gui::normalizeRadioBackend("kenwood tcp"), std::string("kenwood_tcp"));
    ASSERT_EQ(gui::normalizeRadioBackend("other"), std::string("none"));
    ASSERT_EQ(gui::normalizeModemFamily("ultra"), std::string("ULTRA"));
    ASSERT_EQ(gui::normalizeModemFamily("bogus"), std::string("WIDE"));
    ASSERT_EQ(std::string(gui::familyBandwidthLabel("narrow")), std::string("500"));
    ASSERT_EQ(std::string(gui::familyBandwidthLabel("")), std::string());
    ASSERT_TRUE(gui::parseVoiceMode(" broadcast ") == VoiceMode::Broadcast);
    ASSERT_TRUE(gui::parseVoiceMode("connected") == VoiceMode::Connected);
    ASSERT_TRUE(gui::parseVoiceMode("x") == VoiceMode::Off);
    ASSERT_EQ(gui::normalizeVoiceCodec("700c"), std::string("700C"));
    ASSERT_EQ(gui::normalizeVoiceCodec("9600"), std::string("AUTO"));
    PASS();
}

TEST(position_without_fix_is_not_usable) {
    ASSERT_FALSE(gui::isUsablePosition(0.0, 0.0));
    ASSERT_FALSE(gui::isUsablePosition(91.0, 10.0));
    ASSERT_FALSE(gui::isUsablePosition(10.0, -181.0));
    ASSERT_FALSE(gui::isUsablePosition(std::numeric_limits<double>::quiet_NaN(), 1.0));
    ASSERT_TRUE(gui::isUsablePosition(40.5, -74.25));
    ASSERT_TRUE(gui::isUsablePosition(0.0, 12.0));
    PASS();
}

// Layout parsed by App's 0xF4 handler: lat@1, lon@9, grid_len@17, grid,
// then "CALL comment".
TEST(position_beacon_packet_layout) {
    const auto packet = gui::buildPositionBeaconPacket(40.5, -74.25, " FN20xr12ab ", "n0call",
                                                       " hello ");
    ASSERT_EQ(packet[0], uint8_t(0xF4));
    ASSERT_NEAR(readDoubleAt(packet, 1), 40.5, 1e-12);
    ASSERT_NEAR(readDoubleAt(packet, 9), -74.25, 1e-12);
    ASSERT_EQ(packet[17], uint8_t(8));  // grid truncated to 8 characters
    ASSERT_EQ(std::string(packet.begin() + 18, packet.begin() + 26), std::string("FN20xr12"));
    ASSERT_EQ(std::string(packet.begin() + 26, packet.end()), std::string("N0CALL hello"));
    PASS();
}

TEST(position_beacon_without_comment_or_grid) {
    const auto packet = gui::buildPositionBeaconPacket(1.0, 2.0, "", "N0CALL", "");
    ASSERT_EQ(packet[17], uint8_t(0));
    ASSERT_EQ(std::string(packet.begin() + 18, packet.end()), std::string("N0CALL"));
    PASS();
}

TEST(position_relay_packet_layout) {
    const auto packet = gui::buildPositionRelayPacket(-33.5, 151.0, "QF56", "mobile");
    ASSERT_EQ(packet[0], uint8_t(0xF5));
    ASSERT_NEAR(readDoubleAt(packet, 1), -33.5, 1e-12);
    ASSERT_NEAR(readDoubleAt(packet, 9), 151.0, 1e-12);
    ASSERT_EQ(packet[17], uint8_t(4));
    ASSERT_EQ(std::string(packet.begin() + 18, packet.begin() + 22), std::string("QF56"));
    ASSERT_EQ(std::string(packet.begin() + 22, packet.end()), std::string("mobile"));
    PASS();
}

TEST(no_modem_means_no_link) {
    ASSERT_FALSE(gui::isStationLinked(nullptr));
    ModemClient modem;  // never connected
    ASSERT_FALSE(gui::isStationLinked(&modem));
    PASS();
}

TEST(aprs_passcode_prefers_configured) {
    ASSERT_EQ(APRSClient::effectivePasscode("", ""), std::string("-1"));
    ASSERT_EQ(APRSClient::effectivePasscode("", "  "), std::string("-1"));
    ASSERT_EQ(APRSClient::effectivePasscode("", "N0CALL"),
              std::to_string(APRSClient::calculatePasscode("N0CALL")));
    ASSERT_EQ(APRSClient::effectivePasscode("12345", "N0CALL"), std::string("12345"));
    PASS();
}

TEST(formatting_helpers) {
    ASSERT_EQ(gui::formatSnr(std::numeric_limits<float>::quiet_NaN()), std::string("-"));
    ASSERT_EQ(gui::formatSnr(std::numeric_limits<float>::quiet_NaN(), true), std::string("-"));
    ASSERT_EQ(gui::formatSnr(-7.4f), std::string("-7"));
    ASSERT_EQ(gui::formatSnr(3.0f, true), std::string("3 dB"));
    ASSERT_EQ(gui::formatDuration(0), std::string("00:00:00"));
    ASSERT_EQ(gui::formatDuration(3723), std::string("01:02:03"));
    ASSERT_EQ(gui::formatDuration(-5), std::string("00:00:00"));
    ASSERT_EQ(gui::formatDialFrequency(7055000), std::string("7.055.000"));
    ASSERT_EQ(gui::formatDialFrequency(14105500), std::string("14.105.500"));
    ASSERT_EQ(gui::slotLabel(3), std::string("+3"));
    ASSERT_EQ(gui::slotLabel(-2), std::string("-2"));
    ASSERT_EQ(gui::slotLabel(0), std::string("0"));
    ASSERT_EQ(gui::ladderModeName(gui::kMaxLadderMode), std::string("M13"));
    PASS();
}

TEST(file_type_labels) {
    ASSERT_TRUE(gui::isImageFile("/a/b/photo.WEBP"));
    ASSERT_TRUE(gui::isImageFile("x.jpeg"));
    ASSERT_FALSE(gui::isImageFile("x.txt"));
    ASSERT_FALSE(gui::isImageFile("dir.png/file"));
    ASSERT_EQ(gui::fileTypeDescription("log.ADIF"), std::string("ADIF Log"));
    ASSERT_EQ(gui::fileTypeDescription("contest.cbr"), std::string("Cabrillo Log"));
    ASSERT_EQ(gui::fileTypeDescription("card.qsl"), std::string("QSL Card"));
    ASSERT_EQ(gui::fileTypeDescription("station.log"), std::string("Log File"));
    ASSERT_EQ(gui::fileTypeDescription("noext"), std::string("File"));
    PASS();
}

TEST(enabled_bands_are_sorted_by_frequency) {
    FrequencyManager freq_mgr;
    auto bands = gui::enabledBands(freq_mgr);
    ASSERT_GT(bands.size(), size_t(3));
    for (size_t i = 1; i < bands.size(); ++i) {
        ASSERT_LT(bands[i - 1].calling_freq_hz, bands[i].calling_freq_hz);
    }
    ASSERT_TRUE(freq_mgr.setBandEnabled(bands.front().name, false));
    const auto fewer = gui::enabledBands(freq_mgr);
    ASSERT_EQ(fewer.size(), bands.size() - 1);
    PASS();
}

TEST(chat_history_result_helpers) {
    SharedAppState state;
    ASSERT_EQ(state.chatHistorySize(), size_t(0));
    ASSERT_EQ(state.systemMessageSince(0), std::string());

    std::vector<ChatMessage> history;
    state.chat_history = &history;
    history.push_back(systemLine("old line"));
    const size_t mark = state.chatHistorySize();
    ASSERT_EQ(state.systemMessageSince(mark), std::string());
    ASSERT_FALSE(state.outgoingMessageSince(mark));

    history.push_back(systemLine("first"));
    history.push_back(systemLine("Message not sent"));
    ASSERT_EQ(state.systemMessageSince(mark), std::string("Message not sent"));
    ASSERT_FALSE(state.outgoingMessageSince(mark));

    ChatMessage sent;
    sent.from = "N0CALL";
    sent.text = "hi";
    sent.is_outgoing = true;
    history.push_back(sent);
    ASSERT_TRUE(state.outgoingMessageSince(mark));
    PASS();
}

TEST(window_status_line) {
    SharedAppState state;
    ProbeWindow window(state);
    ASSERT_TRUE(window.status().empty());
    window.postStatus("Tuned", StatusKind::Success);
    ASSERT_EQ(window.status().text, std::string("Tuned"));
    ASSERT_TRUE(window.status().kind == StatusKind::Success);
    window.postStatus("Failed");
    ASSERT_TRUE(window.status().kind == StatusKind::Info);
    window.clearStatus();
    ASSERT_TRUE(window.status().empty());
    PASS();
}

TEST_MAIN()
