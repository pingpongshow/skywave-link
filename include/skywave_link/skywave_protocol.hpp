// Skywave link-layer decoding helpers.
//
// The modem's monitor surfaces (control-port `MON:` lines and the data
// port's 0xFB binary records) carry the RAW LINK FRAME - envelope, body
// and CRC - not a decoded body. This header holds the small amount of
// Skywave wire knowledge the companion app needs to make sense of them:
// the frame-type table, the 12-byte envelope, the broadcast fragment
// chain, and the beacon/CQ bodies.
//
// Reference: Modem/skywave/docs/PROTOCOL.md ("Link Frame Format").

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace skylink {

// PROTOCOL.md frame types (also the PHY header frame_type).
enum class SkywaveFrameType : uint8_t {
    Call = 1,
    CallAck = 2,
    Start = 3,
    StartAck = 4,
    StartNak = 5,
    Data = 6,
    Ack = 7,
    ModeChange = 8,
    ModeAck = 9,
    Probe = 10,
    ProbeAck = 11,
    MaskUpdate = 12,
    MaskAck = 13,
    Keepalive = 14,
    ResumeReq = 15,
    ResumeAck = 16,
    Disc = 17,
    DiscAck = 18,
    Beacon = 19,
    Cq = 20,
    BcastData = 21,
    VoiceOpen = 22,
    VoiceOpenAck = 23,
    VoiceClose = 24,
    VoicePttReq = 25,
    VoicePttGrant = 26,
    VoicePttEnd = 27,
    VoiceData = 28,
    KeyUpdate = 29,
    TurnGrant = 30,     // scheduled turn-taking handover (body: epoch u8)
    BcastVoice = 31,    // connectionless voice: VOICE_DATA body outside a session
};

// Envelope flag bits (PROTOCOL.md byte 1; modem link/frame.hpp). Bit 0 is
// unused (compression is stated by the proto message's marker byte); every
// bit other than these two is reserved and must be zero.
inline constexpr uint8_t kSkywaveFlagEncrypted = 1u << 1;
inline constexpr uint8_t kSkywaveFlagMoreFragments = 1u << 2;
inline constexpr uint8_t kSkywaveFlagsReservedMask =
    static_cast<uint8_t>(~(kSkywaveFlagEncrypted | kSkywaveFlagMoreFragments));

inline constexpr size_t kSkywaveEnvelopeBytes = 8;
inline constexpr size_t kSkywaveFrameOverhead = 12;  // envelope + CRC-32

// Uppercase wire name for a frame type ("CQ", "BEACON", "BCAST_DATA", ...).
// Unknown values render as "TYPE<n>" so nothing silently reads as a match.
std::string skywaveFrameTypeName(uint8_t type);

struct SkywaveFrame {
    uint8_t type = 0;
    std::string type_name;
    uint8_t flags = 0;
    uint16_t session_id = 0;
    uint16_t sequence = 0;
    uint16_t ack_base = 0;
    std::vector<uint8_t> body;
    bool crc_ok = false;   // false when the frame was truncated (MON: lines)
    bool truncated = false;
};

// Split a raw link frame into envelope + body. Like the modem's parseFrame
// it rejects unknown frame types and reserved flag bits. `allow_truncated`
// accepts a frame whose CRC does not check, for a `MON:` payload that may
// have lost its tail.
std::optional<SkywaveFrame> parseSkywaveFrame(const std::vector<uint8_t>& raw,
                                              bool allow_truncated);

// BEACON body (modem link/frame.cpp BeaconBody): length-prefixed callsign
// plus one byte, the sender's SNR-floor estimate as dB + 20. The modem has no
// calibrated floor and sends 0xFF, "unknown". It is the SENDER's advertised
// floor, never a measurement of how well this station hears the beacon.
inline constexpr uint8_t kSkywaveBeaconSnrUnknown = 0xFF;
struct SkywaveBeacon {
    std::string callsign;
    bool snr_floor_known = false;
    float snr_floor_db = 0.0f;   // only meaningful when snr_floor_known
};
std::optional<SkywaveBeacon> parseSkywaveBeacon(const std::vector<uint8_t>& body);

// CQ body (modem CqBody): the length-prefixed callsign and nothing else.
struct SkywaveCq {
    std::string callsign;
};
std::optional<SkywaveCq> parseSkywaveCq(const std::vector<uint8_t>& body);

// TURN_GRANT body (modem TurnGrantBody): one epoch byte.
std::optional<uint8_t> parseSkywaveTurnGrant(const std::vector<uint8_t>& body);

// BCAST_DATA body: 0x5B 0x01, u16le length, packed 4-bit fragment index and
// (count - 1), then the fragment bytes (at most 256 per frame).
inline constexpr size_t kSkywaveBcastMaxPayload = 256;
struct SkywaveBcastFragment {
    uint8_t index = 0;
    uint8_t count = 1;
    std::vector<uint8_t> payload;
};
std::optional<SkywaveBcastFragment> parseSkywaveBcast(
    const std::vector<uint8_t>& body);

// Reassembles one broadcast fragment chain at a time, mirroring the modem's
// own Monitor: v1 has no per-sender demultiplexing on frequency.
class BroadcastReassembler {
public:
    // Returns the whole message once its final fragment lands.
    std::optional<std::vector<uint8_t>> feed(const SkywaveBcastFragment& frag);
    void reset();

private:
    uint8_t count_ = 0;
    std::vector<std::vector<uint8_t>> frags_;
};

// Family index as carried in 0xFB monitor records.
const char* skywaveFamilyName(uint8_t family);

// A control-port monitor notification, minus its `MON:` prefix:
//   <src>:<dst|*>:<frame_type>:<family>:<Mx>:<hex_payload>
// The hex payload is the whole raw link frame (envelope + body + CRC).
struct SkywaveMonitorLine {
    std::string src;
    std::string dst;
    int frame_type = 0;
    std::string family;
    int mode = 0;
    std::vector<uint8_t> raw_frame;   // empty when the payload was undecodable
};
std::optional<SkywaveMonitorLine> parseSkywaveMonitorLine(const std::string& body);

} // namespace skylink
