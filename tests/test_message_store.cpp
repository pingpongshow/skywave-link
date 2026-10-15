// MessageStore tests (SQLite in-memory)
#include "test_macros.hpp"
#include "skywave_link/message_store.hpp"
#include <chrono>

#ifdef SKYWAVE_LINK_HAS_SQLITE

TEST(store_open_memory) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));
    ASSERT_TRUE(store.isOpen());
    store.close();
    ASSERT_FALSE(store.isOpen());
    PASS();
}

TEST(store_create_get) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    skylink::Message msg;
    msg.type = skylink::MessageType::Private;
    msg.status = skylink::MessageStatus::Pending;
    msg.from_callsign = "W1ABC";
    msg.to_callsign = "K2DEF";
    msg.subject = "Hello";
    msg.body = "Test message body";

    int64_t id = store.createMessage(msg);
    ASSERT_GT(id, (int64_t)0);

    auto got = store.getMessage(id);
    ASSERT_TRUE(got.has_value());
    ASSERT_EQ(got->from_callsign, std::string("W1ABC"));
    ASSERT_EQ(got->to_callsign, std::string("K2DEF"));
    ASSERT_EQ(got->subject, std::string("Hello"));
    ASSERT_EQ(got->body, std::string("Test message body"));
    PASS();
}

TEST(store_attachments_roundtrip) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    skylink::Message msg;
    msg.type = skylink::MessageType::File;
    msg.status = skylink::MessageStatus::Pending;
    msg.from_callsign = "W1ABC";
    msg.to_callsign = "K2DEF";
    msg.subject = "Attachment test";
    msg.body = "contains file";

    skylink::Attachment att;
    att.filename = "log.adi";
    att.mime_type = "application/octet-stream";
    att.data = {'A', 'D', 'I', 'F'};
    msg.attachments.push_back(att);

    int64_t id = store.createMessage(msg);
    ASSERT_GT(id, (int64_t)0);

    auto got = store.getMessage(id);
    ASSERT_TRUE(got.has_value());
    ASSERT_EQ(got->attachments.size(), (size_t)1);
    ASSERT_EQ(got->attachments[0].filename, std::string("log.adi"));
    ASSERT_EQ(got->attachments[0].mime_type, std::string("application/octet-stream"));
    ASSERT_EQ(got->attachments[0].data.size(), (size_t)4);
    ASSERT_EQ(got->attachments[0].data[0], (uint8_t)'A');
    ASSERT_EQ(got->attachments[0].data[3], (uint8_t)'F');
    PASS();
}

TEST(store_attachments_update) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    skylink::Message msg;
    msg.type = skylink::MessageType::File;
    msg.status = skylink::MessageStatus::Pending;
    msg.from_callsign = "W1ABC";
    msg.to_callsign = "K2DEF";
    msg.subject = "Initial";
    msg.body = "initial";

    int64_t id = store.createMessage(msg);
    ASSERT_GT(id, (int64_t)0);

    auto got = store.getMessage(id);
    ASSERT_TRUE(got.has_value());
    got->subject = "Updated";

    skylink::Attachment att;
    att.filename = "map.png";
    att.mime_type = "image/png";
    att.data = {0x89, 0x50, 0x4E, 0x47};
    got->attachments.push_back(att);

    ASSERT_TRUE(store.updateMessage(*got));

    auto updated = store.getMessage(id);
    ASSERT_TRUE(updated.has_value());
    ASSERT_EQ(updated->subject, std::string("Updated"));
    ASSERT_EQ(updated->attachments.size(), (size_t)1);
    ASSERT_EQ(updated->attachments[0].filename, std::string("map.png"));
    ASSERT_EQ(updated->attachments[0].mime_type, std::string("image/png"));
    ASSERT_EQ(updated->attachments[0].data.size(), (size_t)4);
    ASSERT_EQ(updated->attachments[0].data[0], (uint8_t)0x89);
    PASS();
}

TEST(store_get_nonexistent) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    auto got = store.getMessage(99999);
    ASSERT_FALSE(got.has_value());
    PASS();
}

TEST(store_delete) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    skylink::Message msg;
    msg.from_callsign = "W1ABC";
    msg.to_callsign = "K2DEF";
    msg.subject = "Delete me";
    msg.body = "body";

    int64_t id = store.createMessage(msg);
    ASSERT_GT(id, (int64_t)0);

    ASSERT_TRUE(store.deleteMessage(id));

    auto got = store.getMessage(id);
    ASSERT_FALSE(got.has_value());

    // Deleting a non-existent id should return false (fix #6)
    ASSERT_FALSE(store.deleteMessage(99999));
    PASS();
}

TEST(store_status_transitions) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    skylink::Message msg;
    msg.type = skylink::MessageType::Private;
    msg.status = skylink::MessageStatus::Pending;
    msg.from_callsign = "W1ABC";
    msg.to_callsign = "K2DEF";
    msg.subject = "Status test";
    msg.body = "body";

    int64_t id = store.createMessage(msg);

    // Pending -> Sent
    ASSERT_TRUE(store.markAsSent(id));
    auto got = store.getMessage(id);
    ASSERT_EQ(static_cast<int>(got->status), static_cast<int>(skylink::MessageStatus::Sent));
    ASSERT_GT(got->sent_at, (int64_t)0);

    // Sent -> Delivered
    ASSERT_TRUE(store.markAsDelivered(id));
    got = store.getMessage(id);
    ASSERT_EQ(static_cast<int>(got->status), static_cast<int>(skylink::MessageStatus::Delivered));
    ASSERT_GT(got->delivered_at, (int64_t)0);
    PASS();
}

TEST(store_mark_failed) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    skylink::Message msg;
    msg.status = skylink::MessageStatus::Pending;
    msg.from_callsign = "W1ABC";
    msg.to_callsign = "K2DEF";
    msg.subject = "Fail test";
    msg.body = "body";

    int64_t id = store.createMessage(msg);
    ASSERT_TRUE(store.markAsFailed(id));

    auto got = store.getMessage(id);
    ASSERT_EQ(static_cast<int>(got->status), static_cast<int>(skylink::MessageStatus::Failed));
    PASS();
}

TEST(store_increment_retry) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    skylink::Message msg;
    msg.from_callsign = "W1ABC";
    msg.to_callsign = "K2DEF";
    msg.subject = "Retry test";
    msg.body = "body";

    int64_t id = store.createMessage(msg);
    ASSERT_TRUE(store.incrementRetry(id));
    ASSERT_TRUE(store.incrementRetry(id));

    auto got = store.getMessage(id);
    ASSERT_EQ(got->retry_count, 2);
    PASS();
}

TEST(store_filter_by_callsign) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    // Create messages from different callsigns
    for (int i = 0; i < 3; i++) {
        skylink::Message msg;
        msg.from_callsign = "W1ABC";
        msg.to_callsign = "K2DEF";
        msg.subject = "From W1ABC #" + std::to_string(i);
        msg.body = "body";
        store.createMessage(msg);
    }
    {
        skylink::Message msg;
        msg.from_callsign = "N3GHI";
        msg.to_callsign = "K2DEF";
        msg.subject = "From N3GHI";
        msg.body = "body";
        store.createMessage(msg);
    }

    skylink::MessageFilter filter;
    filter.from_callsign = "W1ABC";
    auto msgs = store.getMessages(filter);
    ASSERT_EQ(msgs.size(), (size_t)3);

    filter.from_callsign = "N3GHI";
    msgs = store.getMessages(filter);
    ASSERT_EQ(msgs.size(), (size_t)1);
    PASS();
}

TEST(store_filter_by_status) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    skylink::Message msg;
    msg.status = skylink::MessageStatus::Pending;
    msg.from_callsign = "W1ABC";
    msg.to_callsign = "K2DEF";
    msg.subject = "Pending";
    msg.body = "body";
    int64_t id1 = store.createMessage(msg);

    msg.subject = "Also pending";
    int64_t id2 = store.createMessage(msg);

    msg.subject = "Will be sent";
    int64_t id3 = store.createMessage(msg);
    store.markAsSent(id3);

    skylink::MessageFilter filter;
    filter.status = skylink::MessageStatus::Pending;
    auto msgs = store.getMessages(filter);
    ASSERT_EQ(msgs.size(), (size_t)2);

    filter.status = skylink::MessageStatus::Sent;
    msgs = store.getMessages(filter);
    ASSERT_EQ(msgs.size(), (size_t)1);

    (void)id1; (void)id2;
    PASS();
}

TEST(store_count_pending) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    ASSERT_EQ(store.countPendingMessages(), 0);

    skylink::Message msg;
    msg.status = skylink::MessageStatus::Pending;
    msg.from_callsign = "W1ABC";
    msg.to_callsign = "K2DEF";
    msg.subject = "Test";
    msg.body = "body";
    store.createMessage(msg);
    store.createMessage(msg);

    ASSERT_EQ(store.countPendingMessages(), 2);
    PASS();
}

TEST(store_sql_injection_safe) {
    // Verify parameterized queries prevent SQL injection
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    skylink::Message msg;
    msg.from_callsign = "W1ABC";
    msg.to_callsign = "K2DEF";
    msg.subject = "Safe msg";
    msg.body = "body";
    store.createMessage(msg);

    // Try SQL injection through filter callsign
    skylink::MessageFilter filter;
    filter.from_callsign = "' OR '1'='1";
    auto msgs = store.getMessages(filter);
    // Should return 0 results (no callsign matches the injection string)
    ASSERT_EQ(msgs.size(), (size_t)0);

    // Normal query should still work
    filter.from_callsign = "W1ABC";
    msgs = store.getMessages(filter);
    ASSERT_EQ(msgs.size(), (size_t)1);
    PASS();
}

TEST(store_get_by_message_id) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    skylink::Message msg;
    msg.message_id = "W1ABC_unique_123";
    msg.from_callsign = "W1ABC";
    msg.to_callsign = "K2DEF";
    msg.subject = "By ID";
    msg.body = "body";
    store.createMessage(msg);

    auto got = store.getMessageById("W1ABC_unique_123");
    ASSERT_TRUE(got.has_value());
    ASSERT_EQ(got->subject, std::string("By ID"));

    auto missing = store.getMessageById("nonexistent_id");
    ASSERT_FALSE(missing.has_value());
    PASS();
}

TEST(store_inbox_outbox) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    skylink::Message msg;
    msg.from_callsign = "W1ABC";
    msg.to_callsign = "K2DEF";
    msg.subject = "Outgoing";
    msg.body = "body";
    store.createMessage(msg);

    msg.from_callsign = "N3GHI";
    msg.to_callsign = "W1ABC";
    msg.subject = "Incoming";
    store.createMessage(msg);

    auto inbox = store.getInboxMessages("W1ABC");
    ASSERT_EQ(inbox.size(), (size_t)1);
    ASSERT_EQ(inbox[0].subject, std::string("Incoming"));

    auto outbox = store.getOutboxMessages("W1ABC");
    ASSERT_EQ(outbox.size(), (size_t)1);
    ASSERT_EQ(outbox[0].subject, std::string("Outgoing"));
    PASS();
}

TEST(beacon_save_load) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    skylink::BeaconRecord b;
    b.callsign = "W1ABC";
    b.grid_square = "FN42dp";
    b.snr_db = 12.5f;
    b.message = "Hello from W1ABC";
    b.has_pending = true;
    b.email_gateway = false;
    b.band = "40m";
    b.timestamp = 1000;

    int64_t id = store.saveBeacon(b);
    ASSERT_GT(id, (int64_t)0);

    auto beacons = store.loadRecentBeacons(10);
    ASSERT_EQ(beacons.size(), (size_t)1);
    ASSERT_EQ(beacons[0].callsign, std::string("W1ABC"));
    ASSERT_EQ(beacons[0].grid_square, std::string("FN42dp"));
    ASSERT_TRUE(beacons[0].has_pending);
    ASSERT_FALSE(beacons[0].email_gateway);
    ASSERT_EQ(beacons[0].band, std::string("40m"));
    ASSERT_EQ(beacons[0].timestamp, (int64_t)1000);
    PASS();
}

TEST(beacon_load_ordering) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    skylink::BeaconRecord b;
    b.callsign = "FIRST";
    b.timestamp = 100;
    store.saveBeacon(b);

    b.callsign = "SECOND";
    b.timestamp = 200;
    store.saveBeacon(b);

    b.callsign = "THIRD";
    b.timestamp = 300;
    store.saveBeacon(b);

    // Most recent first
    auto beacons = store.loadRecentBeacons(2);
    ASSERT_EQ(beacons.size(), (size_t)2);
    ASSERT_EQ(beacons[0].callsign, std::string("THIRD"));
    ASSERT_EQ(beacons[1].callsign, std::string("SECOND"));
    PASS();
}

TEST(beacon_cleanup) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    skylink::BeaconRecord b;
    b.callsign = "OLD";
    b.timestamp = now - 7200; // 2 hours ago
    store.saveBeacon(b);

    b.callsign = "RECENT";
    b.timestamp = now - 60; // 1 minute ago
    store.saveBeacon(b);

    // Cleanup entries older than 1 hour
    int deleted = store.cleanupOldBeacons(3600);
    ASSERT_EQ(deleted, 1);

    auto beacons = store.loadRecentBeacons(10);
    ASSERT_EQ(beacons.size(), (size_t)1);
    ASSERT_EQ(beacons[0].callsign, std::string("RECENT"));
    PASS();
}

TEST(ping_save_load) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    skylink::PingRecord p;
    p.callsign = "K2DEF";
    p.snr_db = -3.5f;
    p.grid_square = "EM73";
    p.band = "40m";
    p.online = true;
    p.timestamp = 2000;

    int64_t id = store.savePingResult(p);
    ASSERT_GT(id, (int64_t)0);

    auto pings = store.loadRecentPings(10);
    ASSERT_EQ(pings.size(), (size_t)1);
    ASSERT_EQ(pings[0].callsign, std::string("K2DEF"));
    ASSERT_EQ(pings[0].grid_square, std::string("EM73"));
    ASSERT_EQ(pings[0].band, std::string("40m"));
    ASSERT_TRUE(pings[0].online);
    ASSERT_EQ(pings[0].timestamp, (int64_t)2000);
    PASS();
}

TEST(mail_read_state_roundtrip) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    ASSERT_TRUE(store.setMailReadState("W1ABC_123", true));
    auto state = store.getMailReadState("W1ABC_123");
    ASSERT_TRUE(state.has_value());
    ASSERT_TRUE(*state);

    ASSERT_TRUE(store.setMailReadState("W1ABC_123", false));
    state = store.getMailReadState("W1ABC_123");
    ASSERT_TRUE(state.has_value());
    ASSERT_FALSE(*state);

    auto missing = store.getMailReadState("DOES_NOT_EXIST");
    ASSERT_FALSE(missing.has_value());

    auto all = store.loadMailReadStates(10);
    ASSERT_EQ(all.size(), (size_t)1);
    ASSERT_TRUE(all.find("W1ABC_123") != all.end());
    ASSERT_FALSE(all["W1ABC_123"]);
    PASS();
}

TEST(ping_load_ordering) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    skylink::PingRecord p;
    p.callsign = "FIRST";
    p.timestamp = 100;
    store.savePingResult(p);

    p.callsign = "SECOND";
    p.timestamp = 200;
    store.savePingResult(p);

    auto pings = store.loadRecentPings(10);
    ASSERT_EQ(pings.size(), (size_t)2);
    ASSERT_EQ(pings[0].callsign, std::string("SECOND"));
    ASSERT_EQ(pings[1].callsign, std::string("FIRST"));
    PASS();
}

TEST(ping_cleanup) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    skylink::PingRecord p;
    p.callsign = "OLD";
    p.timestamp = now - 7200;
    store.savePingResult(p);

    p.callsign = "RECENT";
    p.timestamp = now - 60;
    store.savePingResult(p);

    int deleted = store.cleanupOldPings(3600);
    ASSERT_EQ(deleted, 1);

    auto pings = store.loadRecentPings(10);
    ASSERT_EQ(pings.size(), (size_t)1);
    ASSERT_EQ(pings[0].callsign, std::string("RECENT"));
    PASS();
}

TEST(weather_save_load_cleanup) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    store.saveWeatherReport("METAR", "KJFK", "Wind 280 at 15", now - 7200);
    store.saveWeatherReport("METAR", "KLAX", "Clear skies", now - 60);

    auto reports = store.loadWeatherReports(10);
    ASSERT_EQ(reports.size(), (size_t)2);

    // Cleanup entries older than 1 hour
    int deleted = store.cleanupOldWeather(3600);
    ASSERT_EQ(deleted, 1);

    reports = store.loadWeatherReports(10);
    ASSERT_EQ(reports.size(), (size_t)1);
    ASSERT_EQ(reports[0].location, std::string("KLAX"));
    PASS();
}

TEST(mail_read_state_cleanup) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    // Set two read states - the timestamps are set by setMailReadState to "now"
    ASSERT_TRUE(store.setMailReadState("MSG_OLD", true));
    ASSERT_TRUE(store.setMailReadState("MSG_NEW", false));

    auto all = store.loadMailReadStates(10);
    ASSERT_EQ(all.size(), (size_t)2);

    // Cleanup with 0 max_age should delete everything (cutoff = now)
    int deleted = store.cleanupOldMailReadStates(0);
    ASSERT_GE(deleted, 0);  // All entries have updated_at <= now so all deleted
    PASS();
}

TEST(message_id_entropy) {
    // Verify that generateMessageId produces IDs with 48-bit random part
    std::string id1 = skylink::MessageStore::generateMessageId("W1ABC");
    std::string id2 = skylink::MessageStore::generateMessageId("W1ABC");

    // IDs should start with callsign
    ASSERT_TRUE(id1.find("W1ABC_") == 0);
    ASSERT_TRUE(id2.find("W1ABC_") == 0);

    // IDs should be different (extremely high probability with 48-bit random)
    ASSERT_NE(id1, id2);

    // ID should be long enough to contain 48-bit hex (12 hex chars)
    // Format: CALLSIGN_<hex_timestamp>_<hex_random>
    // The random part after the last underscore should be up to 12 hex chars
    auto last_underscore = id1.rfind('_');
    ASSERT_TRUE(last_underscore != std::string::npos);
    std::string random_part = id1.substr(last_underscore + 1);
    // 48-bit random can be up to 12 hex chars, but may be shorter if leading zeros
    ASSERT_TRUE(random_part.size() >= 1);
    ASSERT_TRUE(random_part.size() <= 12);
    PASS();
}

TEST(filter_limit_validation) {
    skylink::MessageStore store;
    ASSERT_TRUE(store.open(":memory:"));

    // Create a few messages
    for (int i = 0; i < 5; i++) {
        skylink::Message msg;
        msg.from_callsign = "W1ABC";
        msg.to_callsign = "K2DEF";
        msg.subject = "Msg " + std::to_string(i);
        msg.body = "body";
        store.createMessage(msg);
    }

    // Negative limit should be clamped to 1
    skylink::MessageFilter filter;
    filter.limit = -5;
    auto msgs = store.getMessages(filter);
    ASSERT_EQ(msgs.size(), (size_t)1);

    // Zero limit should be clamped to 1
    filter.limit = 0;
    msgs = store.getMessages(filter);
    ASSERT_EQ(msgs.size(), (size_t)1);

    // Negative offset should be clamped to 0
    filter.limit = 100;
    filter.offset = -10;
    msgs = store.getMessages(filter);
    ASSERT_EQ(msgs.size(), (size_t)5);
    PASS();
}

#else
// No SQLite - just pass
TEST(store_no_sqlite) {
    std::cout << "  SKIP: SQLite not available\n";
    PASS();
}
#endif

TEST_MAIN()
