// MeshRelay tests - packet serialization, TTL, dedup, loop prevention, routing

#include "test_macros.hpp"
#include "skywave_link/mesh_relay.hpp"
#include <cstring>

using namespace skylink;

// Helper to build a test packet from header + payload
static std::vector<uint8_t> buildPacket(const RelayHeader& hdr,
                                         const std::vector<uint8_t>& payload) {
    auto hdr_bytes = MeshRelay::serializeHeader(hdr);
    std::vector<uint8_t> packet;
    packet.insert(packet.end(), hdr_bytes.begin(), hdr_bytes.end());
    packet.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    packet.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xFF));
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

TEST(header_roundtrip) {
    RelayHeader hdr;
    hdr.ttl = 3;
    hdr.hop_count = 1;
    hdr.source = "W1ABC";
    hdr.destination = "K2DEF";
    hdr.message_id = "msg_001";
    hdr.path.push_back("N3GHI");

    auto bytes = MeshRelay::serializeHeader(hdr);
    ASSERT_TRUE(bytes.size() > 0);
    ASSERT_EQ(bytes[0], RELAY_PACKET_MARKER);

    RelayHeader hdr2;
    size_t hdr_size = 0;
    ASSERT_TRUE(MeshRelay::deserializeHeader(bytes.data(), bytes.size(), hdr2, hdr_size));
    ASSERT_EQ(hdr2.ttl, uint8_t(3));
    ASSERT_EQ(hdr2.hop_count, uint8_t(1));
    ASSERT_EQ(hdr2.source, std::string("W1ABC"));
    ASSERT_EQ(hdr2.destination, std::string("K2DEF"));
    ASSERT_EQ(hdr2.message_id, std::string("msg_001"));
    ASSERT_EQ(hdr2.path.size(), size_t(1));
    ASSERT_EQ(hdr2.path[0], std::string("N3GHI"));
    ASSERT_EQ(hdr_size, bytes.size());
    PASS();
}

TEST(header_empty_path) {
    RelayHeader hdr;
    hdr.ttl = 5;
    hdr.hop_count = 0;
    hdr.source = "AB";
    hdr.destination = "CD";
    hdr.message_id = "x";

    auto bytes = MeshRelay::serializeHeader(hdr);
    RelayHeader hdr2;
    size_t hdr_size = 0;
    ASSERT_TRUE(MeshRelay::deserializeHeader(bytes.data(), bytes.size(), hdr2, hdr_size));
    ASSERT_EQ(hdr2.path.size(), size_t(0));
    PASS();
}

TEST(header_bad_marker) {
    uint8_t data[20] = {0};
    data[0] = 0x00;  // Not 0xE5
    RelayHeader hdr;
    size_t hdr_size;
    ASSERT_FALSE(MeshRelay::deserializeHeader(data, sizeof(data), hdr, hdr_size));
    PASS();
}

TEST(header_too_short) {
    uint8_t data[2] = {RELAY_PACKET_MARKER, 3};
    RelayHeader hdr;
    size_t hdr_size;
    ASSERT_FALSE(MeshRelay::deserializeHeader(data, sizeof(data), hdr, hdr_size));
    PASS();
}

// #3: String fields > 255 bytes should cause serializeHeader to return empty
TEST(header_string_too_long) {
    RelayHeader hdr;
    hdr.ttl = 3;
    hdr.source = std::string(256, 'A');  // Too long
    hdr.destination = "K2DEF";
    hdr.message_id = "msg";
    auto bytes = MeshRelay::serializeHeader(hdr);
    ASSERT_EQ(bytes.size(), size_t(0));

    // Destination too long
    hdr.source = "W1ABC";
    hdr.destination = std::string(256, 'B');
    bytes = MeshRelay::serializeHeader(hdr);
    ASSERT_EQ(bytes.size(), size_t(0));

    // Message ID too long
    hdr.destination = "K2DEF";
    hdr.message_id = std::string(256, 'C');
    bytes = MeshRelay::serializeHeader(hdr);
    ASSERT_EQ(bytes.size(), size_t(0));

    // Path entry too long
    hdr.message_id = "msg";
    hdr.path.push_back(std::string(256, 'D'));
    bytes = MeshRelay::serializeHeader(hdr);
    ASSERT_EQ(bytes.size(), size_t(0));
    PASS();
}

TEST(process_packet_for_us) {
    MeshRelay relay;
    relay.setMyCallsign("K2DEF");
    relay.setEnabled(true);

    RelayHeader hdr;
    hdr.ttl = 3;
    hdr.hop_count = 0;
    hdr.source = "W1ABC";
    hdr.destination = "K2DEF";
    hdr.message_id = "test_msg_1";

    auto packet = buildPacket(hdr, {0x48, 0x65, 0x6C, 0x6C, 0x6F});  // "Hello"

    std::vector<uint8_t> out_payload;
    auto result = relay.processRelayPacket(packet, out_payload);
    ASSERT_TRUE(result == RelayResult::ForUs);
    ASSERT_EQ(out_payload.size(), size_t(5));
    ASSERT_EQ(out_payload[0], uint8_t(0x48));

    auto stats = relay.getStats();
    ASSERT_EQ(stats.packets_received, uint64_t(1));
    PASS();
}

TEST(process_packet_relay) {
    MeshRelay relay;
    relay.setMyCallsign("N3GHI");
    relay.setEnabled(true);

    RelayHeader hdr;
    hdr.ttl = 3;
    hdr.hop_count = 0;
    hdr.source = "W1ABC";
    hdr.destination = "K2DEF";
    hdr.message_id = "relay_msg_1";

    auto packet = buildPacket(hdr, {0x01, 0x02});

    std::vector<uint8_t> out_payload;
    auto result = relay.processRelayPacket(packet, out_payload);
    ASSERT_TRUE(result == RelayResult::Relayed);  // #12: Relayed, not ForUs

    // Should be queued for relay
    ASSERT_TRUE(relay.hasRelayQueue());
    ASSERT_EQ(relay.relayQueueSize(), size_t(1));

    auto stats = relay.getStats();
    ASSERT_EQ(stats.packets_relayed, uint64_t(1));
    PASS();
}

TEST(ttl_zero_drop) {
    MeshRelay relay;
    relay.setMyCallsign("N3GHI");
    relay.setEnabled(true);

    RelayHeader hdr;
    hdr.ttl = 0;  // Already exhausted
    hdr.source = "W1ABC";
    hdr.destination = "K2DEF";
    hdr.message_id = "ttl_msg_1";

    auto packet = buildPacket(hdr, {0x01});

    std::vector<uint8_t> out;
    auto result = relay.processRelayPacket(packet, out);
    ASSERT_TRUE(result == RelayResult::Dropped);
    ASSERT_FALSE(relay.hasRelayQueue());

    auto stats = relay.getStats();
    ASSERT_EQ(stats.packets_dropped_ttl, uint64_t(1));
    PASS();
}

TEST(hop_limit_drop) {
    MeshRelay relay;
    relay.setMyCallsign("N3GHI");
    relay.setEnabled(true);
    relay.setMaxHopCount(2);

    RelayHeader hdr;
    hdr.ttl = 3;
    hdr.hop_count = 2;  // Already at hop limit
    hdr.source = "W1ABC";
    hdr.destination = "K2DEF";
    hdr.message_id = "hop_limit_msg_1";

    auto packet = buildPacket(hdr, {0x01});

    std::vector<uint8_t> out;
    auto result = relay.processRelayPacket(packet, out);
    ASSERT_TRUE(result == RelayResult::Dropped);
    ASSERT_FALSE(relay.hasRelayQueue());

    auto stats = relay.getStats();
    ASSERT_EQ(stats.packets_dropped_hop_limit, uint64_t(1));
    PASS();
}

TEST(loop_prevention) {
    MeshRelay relay;
    relay.setMyCallsign("N3GHI");
    relay.setEnabled(true);

    RelayHeader hdr;
    hdr.ttl = 3;
    hdr.source = "W1ABC";
    hdr.destination = "K2DEF";
    hdr.message_id = "loop_msg_1";
    hdr.path.push_back("N3GHI");  // We're already in the path!

    auto packet = buildPacket(hdr, {0x01});

    std::vector<uint8_t> out;
    auto result = relay.processRelayPacket(packet, out);
    ASSERT_TRUE(result == RelayResult::Dropped);
    ASSERT_FALSE(relay.hasRelayQueue());

    auto stats = relay.getStats();
    ASSERT_EQ(stats.packets_dropped_loop, uint64_t(1));
    PASS();
}

TEST(dedup) {
    MeshRelay relay;
    relay.setMyCallsign("K2DEF");
    relay.setEnabled(true);

    RelayHeader hdr;
    hdr.ttl = 3;
    hdr.source = "W1ABC";
    hdr.destination = "K2DEF";
    hdr.message_id = "dedup_msg_1";

    auto packet = buildPacket(hdr, {0x01});

    std::vector<uint8_t> out;
    auto result1 = relay.processRelayPacket(packet, out);
    ASSERT_TRUE(result1 == RelayResult::ForUs);  // First time: accepted

    // Send same packet again
    auto result2 = relay.processRelayPacket(packet, out);
    ASSERT_TRUE(result2 == RelayResult::Dropped);  // Duplicate: dropped

    auto stats = relay.getStats();
    ASSERT_EQ(stats.packets_dropped_dedup, uint64_t(1));
    PASS();
}

TEST(dedup_window_config) {
    MeshRelay relay;
    relay.setDedupWindowSec(120);
    ASSERT_EQ(relay.getDedupWindowSec(), int64_t(120));
    relay.setDedupWindowSec(1);  // Clamp low values
    ASSERT_EQ(relay.getDedupWindowSec(), int64_t(30));
    PASS();
}

TEST(route_table) {
    MeshRelay relay;
    relay.setMyCallsign("W1ABC");

    relay.processBeaconRoute("K2DEF", 15.0f);
    relay.processBeaconRoute("N3GHI", -5.0f);

    auto routes = relay.getRouteTable();
    ASSERT_EQ(routes.size(), size_t(2));

    // Find K2DEF route
    bool found = false;
    for (const auto& r : routes) {
        if (r.destination == "K2DEF") {
            found = true;
            ASSERT_EQ(r.next_hop, std::string("K2DEF"));
            ASSERT_EQ(r.hop_count, uint8_t(1));
        }
    }
    ASSERT_TRUE(found);
    PASS();
}

TEST(route_pruning) {
    MeshRelay relay;
    relay.setMyCallsign("W1ABC");

    RouteEntry old_route;
    old_route.destination = "OLD";
    old_route.next_hop = "OLD";
    old_route.hop_count = 1;
    old_route.snr_db = 0;
    old_route.last_seen = 1000;  // Very old
    relay.addRoute(old_route);

    relay.processBeaconRoute("NEW", 10.0f);

    relay.pruneStaleRoutes(3600);  // 1 hour

    auto routes = relay.getRouteTable();
    ASSERT_EQ(routes.size(), size_t(1));
    ASSERT_EQ(routes[0].destination, std::string("NEW"));
    PASS();
}

TEST(create_relay_packet) {
    MeshRelay relay;
    relay.setMyCallsign("W1ABC");
    relay.setDefaultTTL(5);

    std::vector<uint8_t> payload = {0x48, 0x49};
    auto packet = relay.createRelayPacket("K2DEF", payload);

    ASSERT_TRUE(packet.size() > 0);
    ASSERT_EQ(packet[0], RELAY_PACKET_MARKER);

    // Verify we can deserialize it back
    RelayHeader hdr;
    size_t hdr_size;
    ASSERT_TRUE(MeshRelay::deserializeHeader(packet.data(), packet.size(), hdr, hdr_size));
    ASSERT_EQ(hdr.ttl, uint8_t(5));
    ASSERT_EQ(hdr.source, std::string("W1ABC"));
    ASSERT_EQ(hdr.destination, std::string("K2DEF"));
    ASSERT_TRUE(!hdr.message_id.empty());
    PASS();
}

TEST(create_relay_packet_with_explicit_message_id) {
    MeshRelay relay;
    relay.setMyCallsign("W1ABC");

    std::vector<uint8_t> payload = {0x10, 0x20};
    auto packet = relay.createRelayPacket("K2DEF", payload, "MAILMID123");

    RelayHeader hdr;
    size_t hdr_size = 0;
    ASSERT_TRUE(MeshRelay::deserializeHeader(packet.data(), packet.size(), hdr, hdr_size));
    ASSERT_EQ(hdr.message_id, std::string("MAILMID123"));
    PASS();
}

TEST(disabled_relay_no_forward) {
    MeshRelay relay;
    relay.setMyCallsign("N3GHI");
    relay.setEnabled(false);  // Disabled

    RelayHeader hdr;
    hdr.ttl = 3;
    hdr.source = "W1ABC";
    hdr.destination = "K2DEF";
    hdr.message_id = "disabled_msg_1";

    auto packet = buildPacket(hdr, {0x01});

    std::vector<uint8_t> out;
    auto result = relay.processRelayPacket(packet, out);
    ASSERT_TRUE(result == RelayResult::Dropped);
    ASSERT_FALSE(relay.hasRelayQueue());
    PASS();
}

// #2: Payload > 65535 should return empty packet
TEST(payload_too_large) {
    MeshRelay relay;
    relay.setMyCallsign("W1ABC");

    std::vector<uint8_t> big_payload(70000, 0x42);
    auto packet = relay.createRelayPacket("K2DEF", big_payload);
    ASSERT_EQ(packet.size(), size_t(0));
    PASS();
}

// #4: Broadcast relay - deliver locally AND relay when enabled
TEST(broadcast_deliver_and_relay) {
    MeshRelay relay;
    relay.setMyCallsign("N3GHI");
    relay.setEnabled(true);

    RelayHeader hdr;
    hdr.ttl = 3;
    hdr.hop_count = 0;
    hdr.source = "W1ABC";
    hdr.destination = "*";  // Broadcast
    hdr.message_id = "bcast_msg_1";

    auto packet = buildPacket(hdr, {0xAA, 0xBB});

    std::vector<uint8_t> out;
    auto result = relay.processRelayPacket(packet, out);

    // Should be ForUs (delivered locally)
    ASSERT_TRUE(result == RelayResult::ForUs);
    ASSERT_EQ(out.size(), size_t(2));
    ASSERT_EQ(out[0], uint8_t(0xAA));

    // AND should also be queued for relay
    ASSERT_TRUE(relay.hasRelayQueue());
    ASSERT_EQ(relay.relayQueueSize(), size_t(1));

    auto stats = relay.getStats();
    ASSERT_EQ(stats.packets_relayed, uint64_t(1));
    PASS();
}

// #7: Empty callsign guard
TEST(empty_callsign_guard) {
    MeshRelay relay;
    // Don't set callsign - it's empty
    relay.setEnabled(true);

    RelayHeader hdr;
    hdr.ttl = 3;
    hdr.source = "W1ABC";
    hdr.destination = "K2DEF";
    hdr.message_id = "empty_cs_msg";

    auto packet = buildPacket(hdr, {0x01});

    std::vector<uint8_t> out;
    auto result = relay.processRelayPacket(packet, out);
    ASSERT_TRUE(result == RelayResult::Dropped);
    PASS();
}

// #8: packets_dropped_queue_full counter
TEST(queue_full_counter) {
    MeshRelay relay;
    relay.setMyCallsign("N3GHI");
    relay.setEnabled(true);

    // Fill the queue to MAX_RELAY_QUEUE
    for (int i = 0; i < MeshRelay::MAX_RELAY_QUEUE + 2; ++i) {
        RelayHeader hdr;
        hdr.ttl = 3;
        hdr.hop_count = 0;
        hdr.source = "W1ABC";
        hdr.destination = "K2DEF";
        hdr.message_id = "qfull_msg_" + std::to_string(i);

        auto packet = buildPacket(hdr, {0x01});

        std::vector<uint8_t> out;
        relay.processRelayPacket(packet, out);
    }

    auto stats = relay.getStats();
    ASSERT_EQ(stats.packets_dropped_queue_full, uint64_t(2));
    ASSERT_EQ(stats.packets_relayed, uint64_t(MeshRelay::MAX_RELAY_QUEUE));
    PASS();
}

// #9: Verify relay queue entry uses 'packet' field
TEST(relay_queue_packet_field) {
    MeshRelay relay;
    relay.setMyCallsign("N3GHI");
    relay.setEnabled(true);

    RelayHeader hdr;
    hdr.ttl = 3;
    hdr.hop_count = 0;
    hdr.source = "W1ABC";
    hdr.destination = "K2DEF";
    hdr.message_id = "pkt_field_msg_1";

    auto packet = buildPacket(hdr, {0x42, 0x43});

    std::vector<uint8_t> out;
    relay.processRelayPacket(packet, out);

    RelayQueueEntry entry;
    ASSERT_TRUE(relay.getNextRelay(entry));
    // 'packet' field should contain the full serialized relay packet
    ASSERT_TRUE(entry.packet.size() > 0);
    ASSERT_EQ(entry.packet[0], RELAY_PACKET_MARKER);
    PASS();
}

// #11: Route SNR from received_snr_db parameter
TEST(route_snr_from_parameter) {
    MeshRelay relay;
    relay.setMyCallsign("K2DEF");
    relay.setEnabled(true);

    RelayHeader hdr;
    hdr.ttl = 3;
    hdr.hop_count = 0;
    hdr.source = "W1ABC";
    hdr.destination = "K2DEF";
    hdr.message_id = "snr_msg_1";

    auto packet = buildPacket(hdr, {0x01});

    std::vector<uint8_t> out;
    auto result = relay.processRelayPacket(packet, out, 12.5f);
    ASSERT_TRUE(result == RelayResult::ForUs);

    // Check that the route was learned with the provided SNR
    auto routes = relay.getRouteTable();
    ASSERT_EQ(routes.size(), size_t(1));
    ASSERT_NEAR(routes[0].snr_db, 12.5f, 0.01f);
    PASS();
}

// #12: Tri-state return values
TEST(relay_result_tri_state) {
    MeshRelay relay;
    relay.setMyCallsign("K2DEF");
    relay.setEnabled(true);

    // ForUs
    {
        RelayHeader hdr;
        hdr.ttl = 3;
        hdr.source = "W1ABC";
        hdr.destination = "K2DEF";
        hdr.message_id = "tri_for_us";
        auto packet = buildPacket(hdr, {0x01});
        std::vector<uint8_t> out;
        ASSERT_TRUE(relay.processRelayPacket(packet, out) == RelayResult::ForUs);
    }

    // Relayed
    {
        relay.setMyCallsign("N3GHI");
        RelayHeader hdr;
        hdr.ttl = 3;
        hdr.source = "W1ABC";
        hdr.destination = "K2DEF";
        hdr.message_id = "tri_relayed";
        auto packet = buildPacket(hdr, {0x01});
        std::vector<uint8_t> out;
        ASSERT_TRUE(relay.processRelayPacket(packet, out) == RelayResult::Relayed);
    }

    // Dropped (disabled relay)
    {
        relay.setEnabled(false);
        RelayHeader hdr;
        hdr.ttl = 3;
        hdr.source = "W1ABC";
        hdr.destination = "K2DEF";
        hdr.message_id = "tri_dropped";
        auto packet = buildPacket(hdr, {0x01});
        std::vector<uint8_t> out;
        ASSERT_TRUE(relay.processRelayPacket(packet, out) == RelayResult::Dropped);
    }
    PASS();
}

TEST(empty_source_rejected) {
    RelayHeader hdr;
    hdr.ttl = 3;
    hdr.source = "";  // Empty source
    hdr.destination = "K2DEF";
    hdr.message_id = "msg_001";
    auto bytes = MeshRelay::serializeHeader(hdr);
    // serializeHeader allows empty source, but deserialize rejects it
    RelayHeader hdr2;
    size_t hdr_size = 0;
    ASSERT_FALSE(MeshRelay::deserializeHeader(bytes.data(), bytes.size(), hdr2, hdr_size));
    PASS();
}

TEST(empty_message_id_rejected) {
    RelayHeader hdr;
    hdr.ttl = 3;
    hdr.source = "W1ABC";
    hdr.destination = "K2DEF";
    hdr.message_id = "";  // Empty message_id
    auto bytes = MeshRelay::serializeHeader(hdr);
    RelayHeader hdr2;
    size_t hdr_size = 0;
    ASSERT_FALSE(MeshRelay::deserializeHeader(bytes.data(), bytes.size(), hdr2, hdr_size));
    PASS();
}

TEST(empty_destination_rejected) {
    RelayHeader hdr;
    hdr.ttl = 3;
    hdr.source = "W1ABC";
    hdr.destination = "";  // Empty destination
    hdr.message_id = "msg_001";
    auto bytes = MeshRelay::serializeHeader(hdr);
    RelayHeader hdr2;
    size_t hdr_size = 0;
    ASSERT_FALSE(MeshRelay::deserializeHeader(bytes.data(), bytes.size(), hdr2, hdr_size));
    PASS();
}

TEST(route_validation_empty_fields) {
    MeshRelay relay;
    relay.setMyCallsign("W1ABC");

    // Route with empty destination should be ignored
    RouteEntry bad_route;
    bad_route.destination = "";
    bad_route.next_hop = "K2DEF";
    bad_route.hop_count = 1;
    bad_route.snr_db = 10.0f;
    bad_route.last_seen = 12345;
    relay.addRoute(bad_route);

    auto routes = relay.getRouteTable();
    ASSERT_EQ(routes.size(), size_t(0));

    // Route with empty next_hop should be ignored
    bad_route.destination = "K2DEF";
    bad_route.next_hop = "";
    relay.addRoute(bad_route);
    routes = relay.getRouteTable();
    ASSERT_EQ(routes.size(), size_t(0));
    PASS();
}

TEST(create_relay_empty_callsign_fails) {
    MeshRelay relay;
    // Don't set callsign
    std::vector<uint8_t> payload = {0x01};
    auto packet = relay.createRelayPacket("K2DEF", payload);
    ASSERT_EQ(packet.size(), size_t(0));
    PASS();
}

TEST(create_relay_empty_destination_fails) {
    MeshRelay relay;
    relay.setMyCallsign("W1ABC");
    std::vector<uint8_t> payload = {0x01};
    auto packet = relay.createRelayPacket("", payload);
    ASSERT_EQ(packet.size(), size_t(0));
    PASS();
}

TEST(nested_relay_payload_dropped) {
    MeshRelay relay;
    relay.setMyCallsign("K2DEF");
    relay.setEnabled(true);

    RelayHeader inner;
    inner.source = "N0SPF";
    inner.destination = "K2DEF";
    inner.message_id = "inner_1";
    auto inner_packet = buildPacket(inner, {0x48, 0x69});

    // Wrap the relay packet inside another one, for us and for relaying.
    for (const char* dest : {"K2DEF", "W9XYZ", "*"}) {
        RelayHeader outer;
        outer.source = "W1ABC";
        outer.destination = dest;
        outer.message_id = std::string("outer_") + dest;
        auto packet = buildPacket(outer, inner_packet);
        std::vector<uint8_t> out_payload;
        ASSERT_TRUE(relay.processRelayPacket(packet, out_payload) == RelayResult::Dropped);
        ASSERT_TRUE(out_payload.empty());
    }
    ASSERT_EQ(relay.getStats().packets_dropped_nested, uint64_t(3));
    ASSERT_EQ(relay.relayQueueSize(), size_t(0));

    // And we never build one.
    ASSERT_TRUE(relay.createRelayPacket("W9XYZ", inner_packet).empty());
    PASS();
}

// K16: routes are used. A station that knows a fresh route sends (and
// relays) directed through its next hop; only that station forwards; with
// no route the packet floods.
TEST(directed_relay_along_learned_route) {
    MeshRelay b, c, e;
    b.setMyCallsign("K1BBB");
    c.setMyCallsign("K1CCC");
    e.setMyCallsign("K1EEE");
    for (MeshRelay* r : {&b, &c, &e}) r->setEnabled(true);

    // B hears a packet from K1DDD relayed by C: route to D via C, 2 hops.
    RelayHeader hdr;
    hdr.ttl = 3;
    hdr.hop_count = 1;
    hdr.source = "K1DDD";
    hdr.destination = "*";
    hdr.message_id = "learn_1";
    hdr.path = {"K1CCC"};
    std::vector<uint8_t> out;
    b.processRelayPacket(buildPacket(hdr, {0x42}), out, 6.0f);
    auto route = b.getRoute("K1DDD");
    ASSERT_TRUE(route.has_value());
    ASSERT_EQ(route->next_hop, std::string("K1CCC"));
    ASSERT_EQ(route->hop_count, uint8_t(2));
    ASSERT_TRUE(route->snr_known);
    auto neighbour = b.getRoute("K1CCC");
    ASSERT_TRUE(neighbour.has_value());
    ASSERT_EQ(neighbour->hop_count, uint8_t(1));

    // B sends to D: directed through C.
    auto packet = b.createRelayPacket("K1DDD", {0x01, 0x02});
    RelayHeader sent;
    size_t hs = 0;
    ASSERT_TRUE(MeshRelay::deserializeHeader(packet.data(), packet.size(), sent, hs));
    ASSERT_EQ(sent.next_hop, std::string("K1CCC"));
    ASSERT_EQ(b.getStats().packets_directed, uint64_t(1));

    // E hears it too but is not the next hop: no flood.
    ASSERT_TRUE(e.processRelayPacket(packet, out) == RelayResult::Dropped);
    ASSERT_EQ(e.getStats().packets_not_next_hop, uint64_t(1));
    ASSERT_EQ(e.relayQueueSize(), size_t(0));

    // C forwards it; C hears D directly, so it directs the last hop to D.
    c.processBeaconRoute("K1DDD");
    ASSERT_TRUE(c.processRelayPacket(packet, out) == RelayResult::Relayed);
    RelayQueueEntry entry;
    ASSERT_TRUE(c.getNextRelay(entry));
    ASSERT_EQ(entry.header.next_hop, std::string("K1DDD"));
    ASSERT_EQ(entry.header.path.size(), size_t(1));

    // No route: flooded.
    auto flood = b.createRelayPacket("K1ZZZ", {0x03});
    ASSERT_TRUE(MeshRelay::deserializeHeader(flood.data(), flood.size(), sent, hs));
    ASSERT_TRUE(sent.next_hop.empty());
    PASS();
}

TEST(stale_routes_unused_and_pruned) {
    MeshRelay relay;
    relay.setMyCallsign("K1AAA");
    relay.setEnabled(true);
    relay.setRouteMaxAgeSec(600);
    RouteEntry old_route;
    old_route.destination = "K1DDD";
    old_route.next_hop = "K1CCC";
    old_route.hop_count = 2;
    old_route.last_seen = 1000;  // long ago
    relay.addRoute(old_route);
    ASSERT_FALSE(relay.getRoute("K1DDD").has_value());
    auto packet = relay.createRelayPacket("K1DDD", {0x01});
    RelayHeader hdr;
    size_t hs = 0;
    ASSERT_TRUE(MeshRelay::deserializeHeader(packet.data(), packet.size(), hdr, hs));
    ASSERT_TRUE(hdr.next_hop.empty());
    ASSERT_EQ(relay.getRouteTable().size(), size_t(1));
    relay.pruneStaleRoutes();
    ASSERT_EQ(relay.getRouteTable().size(), size_t(0));
    PASS();
}

TEST(route_snr_only_when_measured) {
    MeshRelay relay;
    relay.setMyCallsign("K1AAA");
    relay.processBeaconRoute("K1BBB");  // no measurement
    auto r = relay.getRoute("K1BBB");
    ASSERT_TRUE(r.has_value());
    ASSERT_FALSE(r->snr_known);
    relay.processBeaconRoute("K1BBB", -3.0f);  // a measured one replaces it
    r = relay.getRoute("K1BBB");
    ASSERT_TRUE(r->snr_known);
    ASSERT_NEAR(r->snr_db, -3.0f, 0.01f);
    PASS();
}

TEST(header_next_hop_roundtrip) {
    RelayHeader hdr;
    hdr.source = "K1AAA";
    hdr.destination = "K1DDD";
    hdr.message_id = "m1";
    hdr.next_hop = "K1CCC";
    hdr.path = {"K1BBB"};
    auto bytes = MeshRelay::serializeHeader(hdr);
    RelayHeader back;
    size_t hs = 0;
    ASSERT_TRUE(MeshRelay::deserializeHeader(bytes.data(), bytes.size(), back, hs));
    ASSERT_EQ(back.next_hop, std::string("K1CCC"));
    ASSERT_EQ(back.path.size(), size_t(1));
    ASSERT_EQ(hs, bytes.size());
    PASS();
}

TEST_MAIN()
