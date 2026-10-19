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

// BeaconBody::pack(): callsign + snrFloorPlus20 (one byte, 0xFF = unknown).
std::vector<uint8_t> beaconBody(const std::string& call, uint8_t snr_plus_20) {
    std::vector<uint8_t> body = packCall(call);
    body.push_back(snr_plus_20);
    return body;
}

// Golden frames printed by the modem's own packFrame()/Body::pack()
// (skywave/src/link/frame.cpp), so a layout drift on either side fails here.
std::vector<uint8_t> fromHex(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}
const char* kModemBeaconUnknown = "1300000000000000054B31414141FF89FBBABF";  // K1AAA, 0xFF
const char* kModemBeaconFloor = "1300000000000000054B314141410C2F580E9B";    // K1AAA, -8 dB
const char* kModemCq = "1400000000000000074B314242422F50766E0544";           // K1BBB/P
const char* kModemTurnGrant = "1E00341200000000073F3A5D0E";                  // epoch 7
const char* kModemBcastVoice = "1F00EFBE03000000010203DC486FEB";
const char* kModemData = "060634127856BC9AAABBCC4D1CF980";  // ENCRYPTED|MORE_FRAGMENTS
const char* kModemBcastBody = "5B01020012DEAD";             // frag 1 of 3

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
    const uint8_t flags = kSkywaveFlagEncrypted | kSkywaveFlagMoreFragments;
    const auto raw = packFrame(6, flags, 0x1234, 0x5678, 0x9ABC, body);
    ASSERT_TRUE(raw == fromHex(kModemData));  // same bytes as the modem's packFrame

    auto frame = parseSkywaveFrame(raw, false);
    ASSERT_TRUE(frame.has_value());
    ASSERT_TRUE(frame->crc_ok);
    ASSERT_FALSE(frame->truncated);
    ASSERT_EQ(frame->type, (uint8_t)6);
    ASSERT_EQ(frame->type_name, std::string("DATA"));
    ASSERT_EQ(frame->flags, flags);
    ASSERT_EQ(frame->session_id, (uint16_t)0x1234);
    ASSERT_EQ(frame->sequence, (uint16_t)0x5678);
    ASSERT_EQ(frame->ack_base, (uint16_t)0x9ABC);
    ASSERT_EQ(frame->body.size(), (size_t)3);
    ASSERT_EQ(frame->body[0], (uint8_t)0xAA);
    PASS();
}

TEST(reserved_flag_bits_rejected) {
    // Bit 0 (the old "compressed" bit) and bit 3 are reserved in the modem.
    ASSERT_FALSE(parseSkywaveFrame(packFrame(6, 0x01, 0, 0, 0, {0x00}), true).has_value());
    ASSERT_FALSE(parseSkywaveFrame(packFrame(6, 0x08, 0, 0, 0, {0x00}), true).has_value());
    PASS();
}

TEST(turn_grant_and_bcast_voice_frames_known) {
    ASSERT_EQ(skywaveFrameTypeName(30), std::string("TURN_GRANT"));
    ASSERT_EQ(skywaveFrameTypeName(31), std::string("BCAST_VOICE"));

    auto turn = parseSkywaveFrame(fromHex(kModemTurnGrant), false);
    ASSERT_TRUE(turn.has_value());
    ASSERT_EQ(turn->type, (uint8_t)30);
    ASSERT_EQ(turn->session_id, (uint16_t)0x1234);
    auto epoch = parseSkywaveTurnGrant(turn->body);
    ASSERT_TRUE(epoch.has_value());
    ASSERT_EQ(*epoch, (uint8_t)7);

    auto voice = parseSkywaveFrame(fromHex(kModemBcastVoice), false);
    ASSERT_TRUE(voice.has_value());
    ASSERT_EQ(voice->type_name, std::string("BCAST_VOICE"));
    ASSERT_EQ(voice->session_id, (uint16_t)0xBEEF);  // sender tag
    ASSERT_EQ(voice->sequence, (uint16_t)3);
    ASSERT_EQ(voice->body.size(), (size_t)3);

    // 32 is past the modem's table.
    ASSERT_FALSE(parseSkywaveFrame(packFrame(32, 0, 0, 0, 0, {0x00}), true).has_value());
    PASS();
}

TEST(corrupt_crc_rejected_unless_truncation_allowed) {
    auto raw = packFrame(6, 0, 1, 2, 3, {0x01, 0x02});
    raw[9] ^= 0xFF;  // flip a body byte, invalidating the CRC

    ASSERT_FALSE(parseSkywaveFrame(raw, false).has_value());

    // A caller that accepts truncation still gets the readable prefix -
    // flagged.
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

TEST(beacon_body_parsed_from_modem_frame) {
    auto frame = parseSkywaveFrame(fromHex(kModemBeaconFloor), false);
    ASSERT_TRUE(frame.has_value());
    ASSERT_EQ(frame->type_name, std::string("BEACON"));
    ASSERT_TRUE(frame->body == beaconBody("K1AAA", 12));
    auto beacon = parseSkywaveBeacon(frame->body);
    ASSERT_TRUE(beacon.has_value());
    ASSERT_EQ(beacon->callsign, std::string("K1AAA"));
    ASSERT_TRUE(beacon->snr_floor_known);
    ASSERT_TRUE(beacon->snr_floor_db < -7.9f && beacon->snr_floor_db > -8.1f);
    PASS();
}

TEST(beacon_unknown_floor_is_not_an_snr) {
    // What the real modem sends: 0xFF, "unknown" (worker.cpp).
    auto frame = parseSkywaveFrame(fromHex(kModemBeaconUnknown), false);
    ASSERT_TRUE(frame.has_value());
    auto beacon = parseSkywaveBeacon(frame->body);
    ASSERT_TRUE(beacon.has_value());
    ASSERT_EQ(beacon->callsign, std::string("K1AAA"));
    ASSERT_FALSE(beacon->snr_floor_known);
    PASS();
}

TEST(beacon_body_rejects_bad_layouts) {
    std::vector<uint8_t> short_call = {2, 'A', 'B', 12};  // length 2 < minimum 3
    ASSERT_FALSE(parseSkywaveBeacon(short_call).has_value());

    std::vector<uint8_t> truncated = {4, 'K', '1'};  // claims 4, has 2
    ASSERT_FALSE(parseSkywaveBeacon(truncated).has_value());

    // The old app-side layout (floor, family, version) is not the modem's.
    std::vector<uint8_t> trailing = beaconBody("K1AAA", 12);
    trailing.push_back(0);
    trailing.push_back(1);
    ASSERT_FALSE(parseSkywaveBeacon(trailing).has_value());

    ASSERT_FALSE(parseSkywaveBeacon(packCall("K1AAA")).has_value());  // byte missing
    ASSERT_FALSE(parseSkywaveBeacon(beaconBody("k1aaa", 12)).has_value());  // not canonical
    PASS();
}

TEST(cq_body_parsed_from_modem_frame) {
    auto frame = parseSkywaveFrame(fromHex(kModemCq), false);
    ASSERT_TRUE(frame.has_value());
    ASSERT_EQ(frame->type_name, std::string("CQ"));
    auto cq = parseSkywaveCq(frame->body);
    ASSERT_TRUE(cq.has_value());
    ASSERT_EQ(cq->callsign, std::string("K1BBB/P"));

    // Anything after the callsign is not a modem CQ body.
    std::vector<uint8_t> extra = packCall("K1BBB");
    extra.push_back(0x34);
    extra.push_back(0x12);
    ASSERT_FALSE(parseSkywaveCq(extra).has_value());
    PASS();
}

TEST(bcast_fragment_parsed) {
    ASSERT_TRUE(bcastBody(1, 3, {0xDE, 0xAD}) == fromHex(kModemBcastBody));
    auto frag = parseSkywaveBcast(fromHex(kModemBcastBody));
    ASSERT_TRUE(frag.has_value());
    ASSERT_EQ(frag->index, (uint8_t)1);
    ASSERT_EQ(frag->count, (uint8_t)3);
    ASSERT_EQ(frag->payload.size(), (size_t)2);
    ASSERT_EQ(frag->payload[0], (uint8_t)0xDE);

    // Length must match exactly and stay within 256 bytes.
    auto longer = fromHex(kModemBcastBody);
    longer.push_back(0x00);
    ASSERT_FALSE(parseSkywaveBcast(longer).has_value());
    ASSERT_FALSE(parseSkywaveBcast(bcastBody(0, 1, std::vector<uint8_t>(257, 1))).has_value());
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

TEST(monitor_line_parsed) {
    // A CQ from K1BBB to everyone, WIDE M4.
    const auto raw = packFrame(20, 0, 0, 0, 0, packCall("K1BBB"));
    std::string hex;
    static const char kHex[] = "0123456789ABCDEF";
    for (uint8_t b : raw) {
        hex.push_back(kHex[(b >> 4) & 0x0F]);
        hex.push_back(kHex[b & 0x0F]);
    }

    auto line = parseSkywaveMonitorLine("K1BBB:*:20:WIDE:M4:" + hex);
    ASSERT_TRUE(line.has_value());
    ASSERT_EQ(line->src, std::string("K1BBB"));
    ASSERT_EQ(line->dst, std::string("*"));
    ASSERT_EQ(line->frame_type, 20);
    ASSERT_EQ(line->family, std::string("WIDE"));
    ASSERT_EQ(line->mode, 4);
    ASSERT_EQ(line->raw_frame.size(), raw.size());

    auto frame = parseSkywaveFrame(line->raw_frame, true);
    ASSERT_TRUE(frame.has_value());
    auto cq = parseSkywaveCq(frame->body);
    ASSERT_TRUE(cq.has_value());
    ASSERT_EQ(cq->callsign, std::string("K1BBB"));
    PASS();
}

TEST(monitor_line_metadata_only_when_payload_empty) {
    // An encrypted frame surfaces with no hex payload at all.
    auto line = parseSkywaveMonitorLine("K1CCC:K1BBB:6:MID:M9:");
    ASSERT_TRUE(line.has_value());
    ASSERT_EQ(line->frame_type, 6);
    ASSERT_EQ(line->mode, 9);
    ASSERT_TRUE(line->raw_frame.empty());
    PASS();
}

TEST(monitor_line_malformed_rejected) {
    ASSERT_FALSE(parseSkywaveMonitorLine("").has_value());
    ASSERT_FALSE(parseSkywaveMonitorLine("K1BBB:*:20:WIDE").has_value());
    // Non-numeric frame type must not silently read as 0.
    ASSERT_FALSE(parseSkywaveMonitorLine("K1BBB:*:CQ:WIDE:M4:").has_value());
    PASS();
}

TEST(monitor_line_odd_hex_is_not_a_frame) {
    auto line = parseSkywaveMonitorLine("K1BBB:*:19:WIDE:M4:ABC");
    ASSERT_TRUE(line.has_value());
    ASSERT_TRUE(line->raw_frame.empty());
    PASS();
}

TEST_MAIN()
