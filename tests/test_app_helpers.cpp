// App receive-path policy helpers (app_detail): broadcast whitelist,
// session-only and mesh-deliverable types, APRS text sanitising, URL
// encoding, chat fragment reassembly, the relayed-sender marker, the chat
// type byte, malformed-frame rejection, private-mail routing, the relay
// allowlist, the away EMAIL command, station beacons, gateway backoff and
// weather alerts.

#include "test_macros.hpp"
#include "skywave_link/app.hpp"

#include <limits>

using namespace skylink;
using namespace skylink::app_detail;

TEST(broadcast_whitelist_includes_bbs_and_mesh) {
    ASSERT_TRUE(isDisconnectedBroadcastPayload(0xD0));
    ASSERT_TRUE(isDisconnectedBroadcastPayload(0xD1));
    ASSERT_TRUE(isDisconnectedBroadcastPayload(0xE5));
    ASSERT_TRUE(isDisconnectedBroadcastPayload(0xE7));
    PASS();
}

TEST(broadcast_whitelist_excludes_session_only) {
    for (int t = 0; t < 256; ++t) {
        const uint8_t type = static_cast<uint8_t>(t);
        if (isSessionOnlyPayload(type)) {
            ASSERT_FALSE(isDisconnectedBroadcastPayload(type));
            ASSERT_FALSE(isMeshDeliverablePayload(type));
        }
    }
    ASSERT_TRUE(isSessionOnlyPayload(0x10));
    ASSERT_TRUE(isSessionOnlyPayload(0x18));
    ASSERT_TRUE(isSessionOnlyPayload(0xD2));
    ASSERT_TRUE(isSessionOnlyPayload(0xF2));
    ASSERT_TRUE(isSessionOnlyPayload(0xF6));
    ASSERT_TRUE(isSessionOnlyPayload(0xE8));
    ASSERT_FALSE(isSessionOnlyPayload(0xFB));
    PASS();
}

TEST(mesh_delivers_only_chat_and_mail) {
    ASSERT_TRUE(isMeshDeliverablePayload(0xE6));
    ASSERT_TRUE(isMeshDeliverablePayload(0xE2));
    ASSERT_TRUE(isMeshDeliverablePayload(kChatTextMarker));
    ASSERT_FALSE(isMeshDeliverablePayload('H'));  // untyped text is not a payload
    ASSERT_FALSE(isMeshDeliverablePayload(0xE5));  // no nested relay
    ASSERT_FALSE(isMeshDeliverablePayload(0xFA));
    ASSERT_FALSE(isMeshDeliverablePayload(0xF4));
    ASSERT_FALSE(isMeshDeliverablePayload(0xD0));
    PASS();
}

TEST(relay_attribution_marker) {
    ASSERT_EQ(relayAttributionMarker(""), std::string());
    const std::string m = relayAttributionMarker("K1ABC");
    ASSERT_CONTAINS(m, "via K1ABC");
    ASSERT_CONTAINS(m, "unverified");
    PASS();
}

TEST(lat_lon_validation) {
    ASSERT_TRUE(isValidLatLon(42.5, -71.2));
    ASSERT_FALSE(isValidLatLon(std::numeric_limits<double>::quiet_NaN(), 0.0));
    ASSERT_FALSE(isValidLatLon(0.0, std::numeric_limits<double>::infinity()));
    ASSERT_FALSE(isValidLatLon(91.0, 0.0));
    ASSERT_FALSE(isValidLatLon(0.0, -181.0));
    ASSERT_FALSE(isValidLatLon(1e300, 0.0));
    PASS();
}

TEST(sanitize_strips_line_breaks) {
    ASSERT_EQ(sanitizeRemoteText("hi\r\nuser N0CALL pass 1", 100), std::string("hiuser N0CALL pass 1"));
    ASSERT_EQ(sanitizeRemoteText(std::string("a\0b\x7f" "c", 5), 100), std::string("abc"));
    ASSERT_EQ(sanitizeRemoteText("abcdef", 3), std::string("abc"));
    ASSERT_TRUE(containsControlChars("KBOS\r\nHost: x"));
    ASSERT_FALSE(containsControlChars("KBOS"));
    PASS();
}

TEST(url_encode_query) {
    ASSERT_EQ(urlEncode("KBOS"), std::string("KBOS"));
    ASSERT_EQ(urlEncode("KBOS&format=json"), std::string("KBOS%26format%3Djson"));
    ASSERT_EQ(urlEncode("a b/c"), std::string("a%20b%2Fc"));
    PASS();
}

TEST(fragments_reassemble_in_any_order) {
    ChatFragmentAssembler a;
    std::string out;
    ASSERT_FALSE(a.add("W1AW", 1, 2, "world", 100, &out));
    ASSERT_TRUE(a.add("W1AW", 0, 2, "hello ", 101, &out));
    ASSERT_EQ(out, std::string("hello world"));
    ASSERT_EQ(a.pendingSenders(), size_t(0));
    PASS();
}

TEST(fragments_restart_on_new_message) {
    ChatFragmentAssembler a;
    std::string out;
    // First message loses fragment 2 of 3.
    ASSERT_FALSE(a.add("W1AW", 0, 3, "A0", 100, &out));
    ASSERT_FALSE(a.add("W1AW", 1, 3, "A1", 100, &out));
    // Next message (2 fragments): count change resets the buffer.
    ASSERT_FALSE(a.add("W1AW", 0, 2, "B0", 101, &out));
    ASSERT_TRUE(a.add("W1AW", 1, 2, "B1", 101, &out));
    ASSERT_EQ(out, std::string("B0B1"));

    // Same count: a repeated fragment 0 starts a new message.
    ASSERT_FALSE(a.add("W1AW", 0, 2, "C0", 102, &out));
    ASSERT_FALSE(a.add("W1AW", 0, 2, "D0", 103, &out));
    ASSERT_TRUE(a.add("W1AW", 1, 2, "D1", 103, &out));
    ASSERT_EQ(out, std::string("D0D1"));
    PASS();
}

TEST(fragments_expire_and_refuse_unknown) {
    ChatFragmentAssembler a;
    std::string out;
    ASSERT_FALSE(a.add("W1AW", 1, 2, "late", 100, &out));
    // Fragment 0 arrives after the timeout: the stale half is gone.
    ASSERT_FALSE(a.add("W1AW", 0, 2, "new", 100 + ChatFragmentAssembler::kTimeoutSec + 1, &out));
    ASSERT_EQ(a.pendingSenders(), size_t(1));
    ASSERT_FALSE(a.add("UNKNOWN", 0, 1, "x", 100, &out));
    ASSERT_FALSE(a.add("", 0, 1, "x", 100, &out));
    ASSERT_FALSE(a.add("W1AW", 2, 2, "bad", 100, &out));
    PASS();
}

TEST(fragments_cap_senders) {
    ChatFragmentAssembler a;
    std::string out;
    for (size_t i = 0; i < ChatFragmentAssembler::kMaxSenders + 10; ++i) {
        a.add("K" + std::to_string(i), 0, 2, "x", static_cast<int64_t>(100 + i), &out);
    }
    ASSERT_EQ(a.pendingSenders(), ChatFragmentAssembler::kMaxSenders);
    PASS();
}

// --- Chat type byte (A2) ---------------------------------------------------

static std::string reassemble(const std::vector<std::vector<uint8_t>>& packets) {
    if (packets.size() == 1) {
        std::string text;
        if (!decodeChatPacket(packets[0], &text)) return "<not chat>";
        return text;
    }
    ChatFragmentAssembler a;
    std::string out;
    for (const auto& p : packets) {
        if (p.size() < 4 || p[0] != kChatFragmentMarker) return "<bad fragment>";
        a.add("K1AAA", p[1], p[2], std::string(p.begin() + 3, p.end()), 100, &out);
    }
    return out;
}

TEST(chat_round_trip_utf8) {
    const std::string euro = "\xE2\x82\xAC" "5 for the QSL card";          // starts with 0xE2
    const std::string cjk = "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C";  // 4 CJK, lead 0xE4
    const std::string emoji = "\xF0\x9F\x93\xA1 73";                       // lead 0xF0
    for (const std::string& text : {euro, cjk, emoji, std::string("plain ascii")}) {
        const auto packets = buildChatPackets(text, 200);
        ASSERT_EQ(packets.size(), size_t(1));
        ASSERT_EQ(packets[0][0], kChatTextMarker);
        ASSERT_TRUE(isWellFormedPayload(packets[0]));
        ASSERT_EQ(reassemble(packets), text);
    }
    PASS();
}

TEST(chat_text_never_looks_like_a_control_frame) {
    // Text whose first byte is a mail/ping/weather/beacon marker as UTF-8
    // lead byte still goes out behind the chat marker.
    for (int lead = 0xE0; lead <= 0xFE; ++lead) {
        std::string text(1, static_cast<char>(lead));
        text += "\x80\x80 hello";
        const auto packets = buildChatPackets(text, 200);
        ASSERT_EQ(packets.size(), size_t(1));
        ASSERT_EQ(packets[0][0], kChatTextMarker);
    }
    PASS();
}

TEST(chat_fragments_keep_utf8_characters_whole) {
    std::string text;
    for (int i = 0; i < 150; ++i) text += "\xE2\x82\xAC\xE4\xBD\xA0";  // 6 bytes each, 900 bytes
    const auto packets = buildChatPackets(text, 200);
    ASSERT_GT(packets.size(), size_t(1));
    for (const auto& p : packets) {
        ASSERT_EQ(p[0], kChatFragmentMarker);
        ASSERT_LE(p.size() - 3, size_t(200));
        // Every fragment starts on a character boundary.
        ASSERT_TRUE((p[3] & 0xC0) != 0x80);
    }
    ASSERT_EQ(reassemble(packets), text);
    PASS();
}

TEST(chat_too_long_or_empty_is_refused) {
    ASSERT_TRUE(buildChatPackets("", 200).empty());
    ASSERT_TRUE(buildChatPackets(std::string(200 * 255 + 1, 'x'), 200).empty());
    ASSERT_EQ(buildChatPackets(std::string(200 * 255, 'x'), 200).size(), size_t(255));
    PASS();
}

// --- Malformed / unknown frames are dropped (A2) ----------------------------

TEST(malformed_binary_frames_are_dropped) {
    auto frame = [](std::initializer_list<int> bytes, size_t pad = 0) {
        std::vector<uint8_t> v;
        for (int b : bytes) v.push_back(static_cast<uint8_t>(b));
        v.insert(v.end(), pad, 0x41);
        return v;
    };
    ASSERT_FALSE(isWellFormedPayload({}));
    ASSERT_FALSE(isWellFormedPayload(frame({0xF8, 0x10, 0x00})));      // removed SNR report
    ASSERT_FALSE(isWellFormedPayload(frame({0xFE})));                  // removed typing frame
    ASSERT_FALSE(isWellFormedPayload(frame({0xFE, 0x01})));
    ASSERT_FALSE(isWellFormedPayload(frame({0xFA, 0x00})));            // beacon < 3
    ASSERT_TRUE(isWellFormedPayload(frame({0xFA, 0x00, 0x00})));
    ASSERT_FALSE(isWellFormedPayload(frame({0xF4}, 16)));              // position < 18
    ASSERT_TRUE(isWellFormedPayload(frame({0xF4}, 17)));
    ASSERT_FALSE(isWellFormedPayload(frame({0xF5}, 10)));
    ASSERT_FALSE(isWellFormedPayload(frame({0xFD, 0x00, 0x02})));      // fragment with no data
    ASSERT_FALSE(isWellFormedPayload(frame({kChatTextMarker})));       // empty chat
    ASSERT_FALSE(isWellFormedPayload(frame({'H', 'i'})));              // untyped text
    ASSERT_FALSE(isWellFormedPayload(frame({0xBD, 0x31, 0x01, 0x01, 0xFB})));  // old envelope
    ASSERT_TRUE(isWellFormedPayload(frame({0xE6, 0x01, 'X', 0x00, 0x01, 'K'})));
    ASSERT_TRUE(isWellFormedPayload(frame({0x10})));
    PASS();
}

// --- Private mail goes direct only to the linked recipient (A3) ------------

TEST(private_mail_direct_only_to_linked_recipient) {
    ASSERT_TRUE(privateMailGoesDirect("K1BBB", "k1bbb"));
    ASSERT_TRUE(privateMailGoesDirect(" K1BBB ", "K1BBB"));
    ASSERT_FALSE(privateMailGoesDirect("K1BBB", "K1CCC"));
    ASSERT_FALSE(privateMailGoesDirect("", "K1BBB"));
    ASSERT_FALSE(privateMailGoesDirect("UNKNOWN", "UNKNOWN"));
    PASS();
}

// --- Email relay allowlist fails closed (K1) -------------------------------

TEST(relay_allowlist_fails_closed) {
    ASSERT_FALSE(relaySenderAllowed({}, "K1BBB"));
    ASSERT_TRUE(relaySenderAllowed({"k1bbb", "W1AW"}, "K1BBB"));
    ASSERT_FALSE(relaySenderAllowed({"W1AW"}, "K1BBB"));
    ASSERT_FALSE(relaySenderAllowed({"UNKNOWN"}, "UNKNOWN"));
    ASSERT_FALSE(relaySenderAllowed({""}, ""));
    PASS();
}

// --- Away-mode EMAIL command (A13) -----------------------------------------

TEST(away_email_command_parses) {
    AwayEmailCommand cmd;
    ASSERT_TRUE(parseAwayEmailCommand("EMAIL:ops@example.org:Status:All well: 3 days left", &cmd));
    ASSERT_EQ(cmd.to, std::string("ops@example.org"));
    ASSERT_EQ(cmd.subject, std::string("Status"));
    ASSERT_EQ(cmd.body, std::string("All well: 3 days left"));
    ASSERT_TRUE(parseAwayEmailCommand("  email: a@b.c :  : body", &cmd));
    ASSERT_EQ(cmd.to, std::string("a@b.c"));
    ASSERT_EQ(cmd.subject, std::string(""));
    ASSERT_FALSE(parseAwayEmailCommand("EMAIL:not-an-address:s:b", &cmd));
    ASSERT_FALSE(parseAwayEmailCommand("EMAIL:a@b.c:s:", &cmd));      // no body
    ASSERT_FALSE(parseAwayEmailCommand("EMAIL:a@b.c", &cmd));
    ASSERT_FALSE(parseAwayEmailCommand("hello EMAIL:a@b.c:s:b", &cmd));
    PASS();
}

// --- Station beacon carries no SNR (A8) ------------------------------------

TEST(station_beacon_round_trip) {
    StationBeacon b;
    b.grid = "FN42ab";
    b.email_gateway = true;
    b.text = "K1AAA Skywave Link";
    const auto packet = encodeStationBeacon(b);
    ASSERT_EQ(packet[0], uint8_t(0xFA));
    ASSERT_EQ(packet[1], uint8_t(6));   // grid length right after the marker
    StationBeacon out;
    ASSERT_TRUE(decodeStationBeacon(packet, &out));
    ASSERT_EQ(out.grid, b.grid);
    ASSERT_TRUE(out.email_gateway);
    ASSERT_EQ(out.text, b.text);
    ASSERT_FALSE(decodeStationBeacon({0xFA, 0x09, 'A'}, &out));  // grid too long/truncated
    PASS();
}

// --- Gateway call backoff (A11) --------------------------------------------

TEST(gateway_backoff_grows_and_caps) {
    ASSERT_EQ(gatewayRetryDelaySec(1), 10);
    ASSERT_EQ(gatewayRetryDelaySec(2), 20);
    ASSERT_EQ(gatewayRetryDelaySec(3), 40);
    ASSERT_EQ(gatewayRetryDelaySec(20), 600);
    PASS();
}

// --- Weather alerts (A14) --------------------------------------------------

TEST(weather_alerts_formatting) {
    const std::string json = R"({"type":"FeatureCollection","features":[
      {"id":"x","properties":{"expires":"2026-10-19T18:00:00-04:00","severity":"Severe",
        "event":"Gale Warning","headline":"Gale Warning issued \"today\""}},
      {"id":"y","properties":{"severity":"Minor","event":"Small Craft Advisory","headline":null}}
    ],"title":"alerts"})";
    const std::string text = formatWeatherAlerts(json, "FN42");
    ASSERT_CONTAINS(text, "Gale Warning | Severity: Severe | Expires: 2026-10-19T18:00:00-04:00");
    ASSERT_CONTAINS(text, "Gale Warning issued \"today\"");
    ASSERT_CONTAINS(text, "Small Craft Advisory | Severity: Minor | Expires: N/A");
    ASSERT_EQ(formatWeatherAlerts(R"({"features":[]})", "FN42"),
              std::string("No active weather alerts for FN42"));
    ASSERT_EQ(formatWeatherAlerts("garbage", "FN42"), std::string());
    PASS();
}

TEST_MAIN()
