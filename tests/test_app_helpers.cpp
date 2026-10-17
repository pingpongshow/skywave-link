// App receive-path policy helpers (app_detail): broadcast whitelist,
// session-only and mesh-deliverable types, APRS text sanitising, URL
// encoding, chat fragment reassembly and the relayed-sender marker.

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
    ASSERT_TRUE(isMeshDeliverablePayload('H'));
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

TEST_MAIN()
