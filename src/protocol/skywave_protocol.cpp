#include "skywave_link/skywave_protocol.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>

namespace skylink {

namespace {

uint16_t loadU16Le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t loadU32Le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// CRC-32 (IEEE 802.3, reflected) - the modem's fec/crc.cpp polynomial.
uint32_t crc32(const uint8_t* data, size_t len) {
    static std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();

    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// Length-prefixed callsign (frame.cpp putCall/getCall: u8 length, 3..12
// bytes of uppercase ASCII).
bool readCall(const std::vector<uint8_t>& b, size_t& off, std::string* out) {
    if (off >= b.size()) return false;
    const size_t len = b[off];
    if (len < 3 || len > 12 || off + 1 + len > b.size()) return false;
    out->assign(reinterpret_cast<const char*>(b.data() + off + 1), len);
    off += 1 + len;
    return true;
}

} // namespace

std::string skywaveFrameTypeName(uint8_t type) {
    switch (static_cast<SkywaveFrameType>(type)) {
        case SkywaveFrameType::Call:          return "CALL";
        case SkywaveFrameType::CallAck:       return "CALL_ACK";
        case SkywaveFrameType::Start:         return "START";
        case SkywaveFrameType::StartAck:      return "START_ACK";
        case SkywaveFrameType::StartNak:      return "START_NAK";
        case SkywaveFrameType::Data:          return "DATA";
        case SkywaveFrameType::Ack:           return "ACK";
        case SkywaveFrameType::ModeChange:    return "MODE_CHANGE";
        case SkywaveFrameType::ModeAck:       return "MODE_ACK";
        case SkywaveFrameType::Probe:         return "PROBE";
        case SkywaveFrameType::ProbeAck:      return "PROBE_ACK";
        case SkywaveFrameType::MaskUpdate:    return "MASK_UPDATE";
        case SkywaveFrameType::MaskAck:       return "MASK_ACK";
        case SkywaveFrameType::Keepalive:     return "KEEPALIVE";
        case SkywaveFrameType::ResumeReq:     return "RESUME_REQ";
        case SkywaveFrameType::ResumeAck:     return "RESUME_ACK";
        case SkywaveFrameType::Disc:          return "DISC";
        case SkywaveFrameType::DiscAck:       return "DISC_ACK";
        case SkywaveFrameType::Beacon:        return "BEACON";
        case SkywaveFrameType::Cq:            return "CQ";
        case SkywaveFrameType::BcastData:     return "BCAST_DATA";
        case SkywaveFrameType::VoiceOpen:     return "VOICE_OPEN";
        case SkywaveFrameType::VoiceOpenAck:  return "VOICE_OPEN_ACK";
        case SkywaveFrameType::VoiceClose:    return "VOICE_CLOSE";
        case SkywaveFrameType::VoicePttReq:   return "VOICE_PTT_REQ";
        case SkywaveFrameType::VoicePttGrant: return "VOICE_PTT_GRANT";
        case SkywaveFrameType::VoicePttEnd:   return "VOICE_PTT_END";
        case SkywaveFrameType::VoiceData:     return "VOICE_DATA";
        case SkywaveFrameType::KeyUpdate:     return "KEY_UPDATE";
    }
    return "TYPE" + std::to_string(static_cast<int>(type));
}

const char* skywaveFamilyName(uint8_t family) {
    switch (family) {
        case 0: return "WIDE";
        case 1: return "MID";
        case 2: return "NARROW";
        case 3: return "ULTRA";
        default: return "?";
    }
}

std::optional<SkywaveFrame> parseSkywaveFrame(const std::vector<uint8_t>& raw,
                                              bool allow_truncated) {
    if (raw.size() < kSkywaveEnvelopeBytes) return std::nullopt;

    SkywaveFrame f;
    f.type = raw[0];
    if (f.type < static_cast<uint8_t>(SkywaveFrameType::Call) ||
        f.type > static_cast<uint8_t>(SkywaveFrameType::KeyUpdate)) {
        return std::nullopt;
    }
    f.type_name = skywaveFrameTypeName(f.type);
    f.flags = raw[1];
    f.session_id = loadU16Le(raw.data() + 2);
    f.sequence = loadU16Le(raw.data() + 4);
    f.ack_base = loadU16Le(raw.data() + 6);

    if (raw.size() >= kSkywaveFrameOverhead) {
        const size_t crc_off = raw.size() - 4;
        const uint32_t want = loadU32Le(raw.data() + crc_off);
        f.crc_ok = (crc32(raw.data(), crc_off) == want);
        if (f.crc_ok) {
            f.body.assign(raw.begin() + kSkywaveEnvelopeBytes,
                          raw.begin() + static_cast<long>(crc_off));
            return f;
        }
    }

    // No valid CRC. A `MON:` line is capped at 64 hex-decoded bytes by the
    // modem, so a longer frame legitimately arrives without its tail; take
    // the body we can see and mark it.
    if (!allow_truncated) return std::nullopt;
    f.truncated = true;
    f.body.assign(raw.begin() + kSkywaveEnvelopeBytes, raw.end());
    return f;
}

namespace {

std::string trimAscii(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && static_cast<unsigned char>(s[a]) <= ' ') ++a;
    while (b > a && static_cast<unsigned char>(s[b - 1]) <= ' ') --b;
    return s.substr(a, b - a);
}

// "M8" / "AUTO(M8)" -> 8; -1 when no ladder index is present.
int modeIndexOf(const std::string& text) {
    const size_t m = text.find_first_of("mM");
    if (m == std::string::npos) return -1;
    size_t i = m + 1;
    if (i >= text.size() || std::isdigit(static_cast<unsigned char>(text[i])) == 0) {
        return -1;
    }
    int value = 0;
    for (; i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0; ++i) {
        value = value * 10 + (text[i] - '0');
    }
    return value;
}

bool hexToBytes(const std::string& hex, std::vector<uint8_t>* out) {
    out->clear();
    if (hex.empty() || (hex.size() % 2u) != 0u) return false;
    out->reserve(hex.size() / 2u);
    for (size_t i = 0; i < hex.size(); i += 2u) {
        int hi = -1, lo = -1;
        for (int k = 0; k < 2; ++k) {
            const char c = hex[i + static_cast<size_t>(k)];
            int v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else { out->clear(); return false; }
            (k == 0 ? hi : lo) = v;
        }
        out->push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

} // namespace

std::optional<SkywaveMonitorLine> parseSkywaveMonitorLine(
    const std::string& body) {
    // Callsigns are validated A-Z/0-9/'/' by the modem, so ':' is an
    // unambiguous separator for the first five fields; the sixth is the rest.
    std::vector<std::string> fields;
    size_t start = 0;
    while (fields.size() < 5) {
        const size_t sep = body.find(':', start);
        if (sep == std::string::npos) return std::nullopt;
        fields.push_back(body.substr(start, sep - start));
        start = sep + 1;
    }
    fields.push_back(body.substr(start));

    SkywaveMonitorLine line;
    line.src = trimAscii(fields[0]);
    line.dst = trimAscii(fields[1]);
    const std::string type_field = trimAscii(fields[2]);
    if (type_field.empty()) return std::nullopt;
    for (char c : type_field) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0) return std::nullopt;
    }
    line.frame_type = std::atoi(type_field.c_str());
    line.family = trimAscii(fields[3]);
    line.mode = std::max(0, modeIndexOf(trimAscii(fields[4])));
    // An encrypted or undecodable payload arrives as an empty hex field;
    // that is metadata-only, not a parse failure.
    hexToBytes(trimAscii(fields[5]), &line.raw_frame);
    return line;
}

std::optional<SkywaveBeacon> parseSkywaveBeacon(
    const std::vector<uint8_t>& body) {
    size_t off = 0;
    SkywaveBeacon b;
    if (!readCall(body, off, &b.callsign)) return std::nullopt;
    if (off + 3 > body.size()) return std::nullopt;
    b.snr_floor_db = static_cast<float>(body[off]) - 20.0f;
    b.family = body[off + 1];
    b.sw_version = body[off + 2];
    return b;
}

std::optional<SkywaveCq> parseSkywaveCq(const std::vector<uint8_t>& body) {
    size_t off = 0;
    SkywaveCq c;
    if (!readCall(body, off, &c.callsign)) return std::nullopt;
    if (off + 2 > body.size()) return std::nullopt;
    c.caps = loadU16Le(body.data() + off);
    return c;
}

std::optional<SkywaveBcastFragment> parseSkywaveBcast(
    const std::vector<uint8_t>& body) {
    if (body.size() < 5) return std::nullopt;
    if (body[0] != 0x5B || body[1] != 0x01) return std::nullopt;
    const uint16_t len = loadU16Le(body.data() + 2);
    SkywaveBcastFragment f;
    f.index = static_cast<uint8_t>(body[4] >> 4);
    f.count = static_cast<uint8_t>((body[4] & 0x0Fu) + 1u);
    if (f.index >= f.count) return std::nullopt;
    if (body.size() < 5u + len) return std::nullopt;
    f.payload.assign(body.begin() + 5, body.begin() + 5 + len);
    return f;
}

std::optional<std::vector<uint8_t>> BroadcastReassembler::feed(
    const SkywaveBcastFragment& frag) {
    if (frag.count == 0 || frag.index >= frag.count) return std::nullopt;
    if (frag.count != count_) {
        count_ = frag.count;
        frags_.assign(frag.count, {});
    }
    frags_[frag.index] = frag.payload;

    for (const auto& f : frags_) {
        if (f.empty()) return std::nullopt;
    }

    std::vector<uint8_t> whole;
    for (const auto& f : frags_) {
        whole.insert(whole.end(), f.begin(), f.end());
    }
    reset();
    return whole;
}

void BroadcastReassembler::reset() {
    count_ = 0;
    frags_.clear();
}

} // namespace skylink
