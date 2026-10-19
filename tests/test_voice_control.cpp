// Live voice control decisions (voice_control.hpp): start blockers, modem
// error explanations, the VOICE button view, notice rate limiting and
// uplink silence pacing.

#include "test_macros.hpp"
#include "skywave_link/voice_control.hpp"

using namespace skylink;

namespace {

bool contains(const std::string& s, const std::string& part) {
    return s.find(part) != std::string::npos;
}

VoiceContext linkedConnected() {
    VoiceContext c;
    c.modem_connected = true;
    c.voice_media = true;
    c.configured_mode = VoiceMode::Connected;
    c.link_state = LinkState::Connected;
    c.link_mode_index = 10;
    c.family = "WIDE";
    c.my_callsign = "K1AAA";
    return c;
}

}  // namespace

TEST(start_blockers_in_order) {
    VoiceContext c;
    ASSERT_TRUE(contains(voiceStartBlocker(c), "modem is not connected"));
    c = linkedConnected();
    ASSERT_EQ(voiceStartBlocker(c), std::string());
    c.modem_voice_unsupported = true;
    ASSERT_TRUE(contains(voiceStartBlocker(c), "without voice"));
    c = linkedConnected();
    c.configured_mode = VoiceMode::Off;
    ASSERT_TRUE(contains(voiceStartBlocker(c), "Voice is off"));
    c = linkedConnected();
    c.voice_media = false;
    ASSERT_TRUE(contains(voiceStartBlocker(c), "voice port is closed"));
    PASS();
}

TEST(connected_needs_link_and_voice_mode) {
    VoiceContext c = linkedConnected();
    c.link_state = LinkState::Idle;
    c.link_mode_index = -1;
    ASSERT_TRUE(contains(voiceStartBlocker(c), "needs a station link"));
    c = linkedConnected();
    c.link_mode_index = 6;
    ASSERT_TRUE(contains(voiceStartBlocker(c), "M6"));
    ASSERT_TRUE(contains(voiceStartBlocker(c), "M8"));
    c.family = "MID";
    c.link_mode_index = 11;
    ASSERT_TRUE(contains(voiceStartBlocker(c), "M12"));
    c.family = "NARROW";
    ASSERT_TRUE(contains(voiceStartBlocker(c), "NARROW"));
    PASS();
}

TEST(broadcast_needs_idle_station) {
    VoiceContext c = linkedConnected();
    c.configured_mode = VoiceMode::Broadcast;
    ASSERT_TRUE(contains(voiceStartBlocker(c), "idle station"));
    c.link_state = LinkState::Idle;
    c.link_mode_index = -1;
    ASSERT_EQ(voiceStartBlocker(c), std::string());
    c.my_callsign = "N0CALL";
    ASSERT_TRUE(contains(voiceStartBlocker(c), "callsign"));
    PASS();
}

TEST(modem_errors_explained) {
    VoiceContext c = linkedConnected();
    ASSERT_EQ(explainVoiceCommandError("VOICESTART", "VOICESTART", c), std::string());
    ASSERT_TRUE(contains(explainVoiceCommandError("VOICESTART", "ERROR:UNSUPPORTED", c),
                         "built without voice"));
    ASSERT_TRUE(contains(explainVoiceCommandError("VOICESTART", "ERROR:STATE", c),
                         "peer may not support voice"));
    ASSERT_TRUE(contains(explainVoiceCommandError("VOICEPTT", "ERROR:STATE", c), "not open"));
    ASSERT_TRUE(contains(explainVoiceCommandError("VOICESTART", "", c), "no answer"));
    c.my_callsign = "N0CALL";
    ASSERT_TRUE(contains(explainVoiceCommandError("VOICESTART", "ERROR:STATE", c), "N0CALL"));
    PASS();
}

TEST(button_view_follows_state) {
    VoiceContext c = linkedConnected();
    VoiceButtonView v = voiceButtonView(c, false, "");
    ASSERT_EQ(v.label, std::string("VOICE"));
    ASSERT_TRUE(v.kind == VoiceIndicator::Idle);
    ASSERT_TRUE(!v.session_open);

    v = voiceButtonView(c, true, "");
    ASSERT_TRUE(v.kind == VoiceIndicator::Starting);
    ASSERT_TRUE(v.session_open);  // a click cancels

    c.voice.state = VoiceState::Ready;
    c.voice.codec = "AUTO(1300)";
    v = voiceButtonView(c, false, "");
    ASSERT_EQ(v.label, std::string("VOICE ON"));
    ASSERT_TRUE(v.session_open);

    c.voice.state = VoiceState::TxActive;
    ASSERT_EQ(voiceButtonView(c, false, "").label, std::string("VOICE TX"));
    c.voice.state = VoiceState::RxActive;
    ASSERT_EQ(voiceButtonView(c, false, "").label, std::string("VOICE RX"));

    VoiceContext b;
    b.modem_connected = true;
    b.voice_media = true;
    b.configured_mode = VoiceMode::Broadcast;
    b.my_callsign = "K1BBB";
    b.voice.broadcast_rx = true;
    v = voiceButtonView(b, false, "");
    ASSERT_EQ(v.label, std::string("VOICE RX"));
    ASSERT_TRUE(!v.session_open);

    VoiceContext off;
    v = voiceButtonView(off, false, "earlier problem");
    ASSERT_TRUE(v.kind == VoiceIndicator::Unavailable);
    ASSERT_TRUE(contains(v.tooltip, "earlier problem"));
    PASS();
}

TEST(ptt_readiness) {
    VoiceStatus v;
    ASSERT_TRUE(!voiceSessionOpen(v));
    ASSERT_TRUE(!voiceSessionReadyForPtt(v));
    v.state = VoiceState::Opening;
    ASSERT_TRUE(voiceSessionOpen(v));
    ASSERT_TRUE(!voiceSessionReadyForPtt(v));
    v.state = VoiceState::Ready;
    ASSERT_TRUE(voiceSessionReadyForPtt(v));
    v.state = VoiceState::Closing;
    ASSERT_TRUE(!voiceSessionOpen(v));
    PASS();
}

TEST(notice_limiter_suppresses_repeats) {
    VoiceNoticeLimiter lim(std::chrono::milliseconds(3000));
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(lim.allow("a", t0));
    ASSERT_TRUE(!lim.allow("a", t0 + std::chrono::milliseconds(500)));
    ASSERT_TRUE(lim.allow("b", t0 + std::chrono::milliseconds(600)));
    ASSERT_TRUE(lim.allow("a", t0 + std::chrono::milliseconds(700)));
    ASSERT_TRUE(lim.allow("a", t0 + std::chrono::milliseconds(3800)));
    PASS();
}

TEST(silence_pacing_is_real_time) {
    using std::chrono::milliseconds;
    ASSERT_EQ(voiceSamplesOwed(milliseconds(0), 0), uint64_t(0));
    ASSERT_EQ(voiceSamplesOwed(milliseconds(250), 0), uint64_t(2000));
    ASSERT_EQ(voiceSamplesOwed(milliseconds(250), 1600), uint64_t(400));
    ASSERT_EQ(voiceSamplesOwed(milliseconds(250), 5000), uint64_t(0));
    PASS();
}

TEST_MAIN()
