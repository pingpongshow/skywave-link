// Skywave link-frame decoding tests.
//
// The vectors here are built the way the modem builds them
// (Modem/skywave/src/link/frame.cpp), so a wire-format change on either
// side shows up as a failure rather than as silently unparsed monitor
// traffic.

#include "skywave_link/skywave_protocol.hpp"
#include "test_macros.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace skylink;

namespace {

uint32_t crc32Ref(const std::vector<uint8_t>& data) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t b : data) {
        crc ^= b;
        for (int k = 0; k < 8; ++k) {
            crc = (crc & 1u) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

// packFrame(): type, flags, session_id, sequence, ack_base, body, CRC-32.
std::vector<uint8_t> packFrame(uint8_t type, uint8_t flags, uint16_t session,
                               uint16_t sequence, uint16_t ack_base,
                               const std::vector<uint8_t>& body) {
    std::vector<uint8_t> out;
    out.push_back(type);
    out.push_back(flags);
    out.push_back(static_cast<uint8_t>(session & 0xFF));
    out.push_back(static_cast<uint8_t>(session >> 8));
    out.push_back(static_cast<uint8_t>(sequence & 0xFF));
    out.push_back(static_cast<uint8_t>(sequence >> 8));
    out.push_back(static_cast<uint8_t>(ack_base & 0xFF));
    out.push_back(static_cast<uint8_t>(ack_base >> 8));
    out.insert(out.end(), body.begin(), body.end());
    const uint32_t crc = crc32Ref(out);
    out.push_back(static_cast<uint8_t>(crc & 0xFF));
    out.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((crc >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((crc >> 24) & 0xFF));
    return out;
}

std::vector<uint8_t> packCall(const std::string& call) {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(call.size()));
    out.insert(out.end(), call.begin(), call.end());
    return out;
}

std::vector<uint8_t> beaconBody(const std::string& call, uint8_t snr_plus_20,
                                uint8_t family) {
    std::vector<uint8_t> body = packCall(call);
    body.push_back(snr_plus_20);
    body.push_back(family);
    body.push_back(1);  // sw version
    return body;
}

std::vector<uint8_t> bcastBody(uint8_t index, uint8_t count,
                               const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> body;
    body.push_back(0x5B);
    body.push_back(0x01);
    body.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    body.push_back(static_cast<uint8_t>(payload.size() >> 8));
    body.push_back(static_cast<uint8_t>((index << 4) | ((count - 1) & 0x0F)));
    body.insert(body.end(), payload.begin(), payload.end());
    return body;
}

} // namespace

TEST(frame_type_names) {
    ASSERT_EQ(skywaveFrameTypeName(20), std::string("CQ"));
    ASSERT_EQ(skywaveFrameTypeName(19), std::string("BEACON"));
    ASSERT_EQ(skywaveFrameTypeName(21), std::string("BCAST_DATA"));
    ASSERT_EQ(skywaveFrameTypeName(6), std::string("DATA"));
    // Unknown values must not collide with a real name.
    ASSERT_EQ(skywaveFrameTypeName(200), std::string("TYPE200"));
    PASS();
}

TEST(family_names) {
    ASSERT_EQ(std::string(skywaveFamilyName(0)), std::string("WIDE"));
    ASSERT_EQ(std::string(skywaveFamilyName(1)), std::string("MID"));
    ASSERT_EQ(std::string(skywaveFamilyName(2)), std::string("NARROW"));
    ASSERT_EQ(std::string(skywaveFamilyName(3)), std::string("ULTRA"));
    PASS();
}

TEST(envelope_round_trip) {
    const std::vector<uint8_t> body = {0xAA, 0xBB, 0xCC};
    const auto raw = packFrame(6, kSkywaveFlagCompressed, 0x1234, 0x5678,
                               0x9ABC, body);

    auto frame = parseSkywaveFrame(raw, false);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(frame->crc_ok);
    ASSERT_FALSE(frame->truncated);
    ASSERT_EQ(frame->type, (uint8_t)6);
    ASSERT_EQ(frame->type_name, std::string("DATA"));
    ASSERT_EQ(frame->flags, (uint8_t)kSkywaveFlagCompressed);
    ASSERT_EQ(frame->session_id, (uint16_t)0x1234);
    ASSERT_EQ(frame->sequence, (uint16_t)0x5678);
    ASSERT_EQ(frame->ack_base, (uint16_t)0x9ABC);
    ASSERT_EQ(frame->body.size(), (size_t)3);
    ASSERT_EQ(frame->body[0], (uint8_t)0xAA);
    PASS();
}

TEST(corrupt_crc_rejected_unless_truncation_allowed) {
    auto raw = packFrame(6, 0, 1, 2, 3, {0x01, 0x02});
    raw[9] ^= 0xFF;  // flip a body byte, invalidating the CRC

    ASSERT_FALSE(parseSkywaveFrame(raw, false).has_value());

    // MON: lines are capped at 64 bytes by the modem, so a caller that
    // expects truncation still gets the readable prefix - flagged.
    auto lenient = parseSkywaveFrame(raw, true);
    ASSERT_TRUE(lenient.has_value());
    ASSERT_TRUE(lenient->truncated);
    PASS();
}

TEST(unknown_frame_type_rejected) {
    const auto raw = packFrame(200, 0, 0, 0, 0, {0x00});
    ASSERT_FALSE(parseSkywaveFrame(raw, true).has_value());
    PASS();
}

TEST(too_short_frame_rejected) {
    const std::vector<uint8_t> raw = {6, 0, 0};
    ASSERT_FALSE(parseSkywaveFrame(raw, true).has_value());
    PASS();
}

TEST(beacon_body_parsed) {
    // snrFloorPlus20 = 12 -> -8 dB
    auto beacon = parseSkywaveBeacon(beaconBody("W1AW", 12, 1));
    ASSERT_TRUE(beacon.has_value());
    ASSERT_EQ(beacon->callsign, std::string("W1AW"));
    ASSERT_TRUE(beacon->snr_floor_db < -7.9f && beacon->snr_floor_db > -8.1f);
    ASSERT_EQ(beacon->family, (uint8_t)1);
    PASS();
}

TEST(beacon_body_rejects_bad_callsign_length) {
    std::vector<uint8_t> body = {2, 'A', 'B', 12, 0, 1};  // length 2 < minimum 3
    ASSERT_FALSE(parseSkywaveBeacon(body).has_value());

    std::vector<uint8_t> truncated = {4, 'W', '1'};  // claims 4, has 2
    ASSERT_FALSE(parseSkywaveBeacon(truncated).has_value());
    PASS();
}

TEST(cq_body_parsed) {
    std::vector<uint8_t> body = packCall("K6XYZ");
    body.push_back(0x34);
    body.push_back(0x12);
    auto cq = parseSkywaveCq(body);
    ASSERT_TRUE(cq.has_value());
    ASSERT_EQ(cq->callsign, std::string("K6XYZ"));
    ASSERT_EQ(cq->caps, (uint16_t)0x1234);
    PASS();
}

TEST(bcast_fragment_parsed) {
    auto frag = parseSkywaveBcast(bcastBody(1, 3, {0xDE, 0xAD}));
    ASSERT_TRUE(frag.has_value());
    ASSERT_EQ(frag->index, (uint8_t)1);
    ASSERT_EQ(frag->count, (uint8_t)3);
    ASSERT_EQ(frag->payload.size(), (size_t)2);
    ASSERT_EQ(frag->payload[0], (uint8_t)0xDE);
    PASS();
}

TEST(bcast_fragment_index_beyond_count_rejected) {
    // index 3 with count 2 is malformed
    std::vector<uint8_t> body = {0x5B, 0x01, 0x01, 0x00, 0x31, 0xAA};
    ASSERT_FALSE(parseSkywaveBcast(body).has_value());
    PASS();
}

TEST(broadcast_reassembly_in_order) {
    BroadcastReassembler reasm;
    SkywaveBcastFragment a{0, 2, {'h', 'e'}};
    SkywaveBcastFragment b{1, 2, {'y', '!'}};

    ASSERT_FALSE(reasm.feed(a).has_value());
    auto whole = reasm.feed(b);
    ASSERT_TRUE(whole.has_value());
    ASSERT_EQ(std::string(whole->begin(), whole->end()), std::string("hey!"));
    PASS();
}

TEST(broadcast_reassembly_out_of_order) {
    BroadcastReassembler reasm;
    SkywaveBcastFragment b{1, 2, {'y', '!'}};
    SkywaveBcastFragment a{0, 2, {'h', 'e'}};

    ASSERT_FALSE(reasm.feed(b).has_value());
    auto whole = reasm.feed(a);
    ASSERT_TRUE(whole.has_value());
    ASSERT_EQ(std::string(whole->begin(), whole->end()), std::string("hey!"));
    PASS();
}

TEST(broadcast_single_fragment_completes_immediately) {
    BroadcastReassembler reasm;
    SkywaveBcastFragment only{0, 1, {'x'}};
    auto whole = reasm.feed(only);
    ASSERT_TRUE(whole.has_value());
    ASSERT_EQ(whole->size(), (size_t)1);
    PASS();
}

TEST(broadcast_new_chain_resets_previous) {
    BroadcastReassembler reasm;
    ASSERT_FALSE(reasm.feed({0, 3, {'a'}}).has_value());
    // A chain with a different fragment count replaces the partial one -
    // matching the modem's own single-chain Monitor behaviour.
    ASSERT_FALSE(reasm.feed({0, 2, {'b'}}).has_value());
    auto whole = reasm.feed({1, 2, {'c'}});
    ASSERT_TRUE(whole.has_value());
    ASSERT_EQ(std::string(whole->begin(), whole->end()), std::string("bc"));
    PASS();
}

TEST(beacon_frame_end_to_end) {
    // What a monitor record actually delivers: a whole link frame whose body
    // must survive envelope stripping and body parsing together.
    const auto raw = packFrame(19, 0, 0, 0, 0, beaconBody("VE3ABC", 25, 0));
    auto frame = parseSkywaveFrame(raw, true);
    ASSERT_TRUE(frame.has_value());
    ASSERT_EQ(frame->type_name, std::string("BEACON"));

    auto beacon = parseSkywaveBeacon(frame->body);
    ASSERT_TRUE(beacon.has_value());
    ASSERT_EQ(beacon->callsign, std::string("VE3ABC"));
    ASSERT_TRUE(beacon->snr_floor_db > 4.9f && beacon->snr_floor_db < 5.1f);
    PASS();
}


TEST(monitor_line_parsed) {
    // A CQ from W1AW to everyone, WIDE M4, with a 3-byte body.
    const auto raw = packFrame(20, 0, 0, 0, 0, [] {
        std::vector<uint8_t> body = packCall("W1AW");
        body.push_back(0x01);
        body.push_back(0x00);
        return body;
    }());
    std::string hex;
    static const char kHex[] = "0123456789ABCDEF";
    for (uint8_t b : raw) {
        hex.push_back(kHex[(b >> 4) & 0x0F]);
        hex.push_back(kHex[b & 0x0F]);
    }

    auto line = parseSkywaveMonitorLine("W1AW:*:20:WIDE:M4:" + hex);
    ASSERT_TRUE(line.has_value());
    ASSERT_EQ(line->src, std::string("W1AW"));
    ASSERT_EQ(line->dst, std::string("*"));
    ASSERT_EQ(line->frame_type, 20);
    ASSERT_EQ(line->family, std::string("WIDE"));
    ASSERT_EQ(line->mode, 4);
    ASSERT_EQ(line->raw_frame.size(), raw.size());

    auto frame = parseSkywaveFrame(line->raw_frame, true);
    ASSERT_TRUE(frame.has_value());
    auto cq = parseSkywaveCq(frame->body);
    ASSERT_TRUE(cq.has_value());
    ASSERT_EQ(cq->callsign, std::string("W1AW"));
    PASS();
}

TEST(monitor_line_metadata_only_when_payload_empty) {
    // An encrypted frame surfaces with no hex payload at all.
    auto line = parseSkywaveMonitorLine("K6XYZ:W1AW:6:MID:M9:");
    ASSERT_TRUE(line.has_value());
    ASSERT_EQ(line->frame_type, 6);
    ASSERT_EQ(line->mode, 9);
    ASSERT_TRUE(line->raw_frame.empty());
    PASS();
}

TEST(monitor_line_malformed_rejected) {
    ASSERT_FALSE(parseSkywaveMonitorLine("").has_value());
    ASSERT_FALSE(parseSkywaveMonitorLine("W1AW:*:20:WIDE").has_value());
    // Non-numeric frame type must not silently read as 0.
    ASSERT_FALSE(parseSkywaveMonitorLine("W1AW:*:CQ:WIDE:M4:").has_value());
    PASS();
}

TEST(monitor_line_odd_hex_is_not_a_frame) {
    auto line = parseSkywaveMonitorLine("W1AW:*:19:WIDE:M4:ABC");
    ASSERT_TRUE(line.has_value());
    ASSERT_TRUE(line->raw_frame.empty());
    PASS();
}

TEST_MAIN()
