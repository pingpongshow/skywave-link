// PSK Reporter IPFIX uploader tests. Datagrams are decoded with an
// independent minimal IPFIX (RFC 7011) parser below; nothing here ever talks
// to the real server (every client gets an injected transport and an
// unresolvable .invalid host as a safety net).

#include "test_macros.hpp"
#include "skywave_link/psk_reporter.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace skylink;

namespace {

// ---------------------------------------------------------------------------
// Minimal independent IPFIX decoder
// ---------------------------------------------------------------------------

struct IpfixField {
    uint16_t id = 0;        // without the enterprise bit
    uint16_t length = 0;    // 0xFFFF = variable
    uint32_t enterprise = 0;
};
struct IpfixTemplate {
    uint16_t id = 0;
    bool options = false;
    uint16_t scope_count = 0;
    std::vector<IpfixField> fields;
};
// Value: raw bytes; helpers interpret.
using IpfixRecord = std::map<std::string, std::vector<uint8_t>>;  // "30351.1" / "150"
struct IpfixDataSet {
    uint16_t template_id = 0;
    size_t length = 0;
    std::vector<IpfixRecord> records;
};
struct IpfixMessage {
    uint16_t version = 0;
    uint16_t length = 0;
    uint32_t export_time = 0;
    uint32_t sequence = 0;
    uint32_t domain = 0;
    std::vector<IpfixTemplate> templates;  // templates carried in this message
    std::vector<IpfixDataSet> data;
    std::vector<size_t> set_lengths;
    bool ok = false;
    std::string error;
};

struct IpfixDecoder {
    std::map<uint16_t, IpfixTemplate> cache;  // like the collector: cached per stream

    static uint16_t rd16(const std::vector<uint8_t>& b, size_t at) {
        return static_cast<uint16_t>((b[at] << 8) | b[at + 1]);
    }
    static uint32_t rd32(const std::vector<uint8_t>& b, size_t at) {
        return (static_cast<uint32_t>(rd16(b, at)) << 16) | rd16(b, at + 2);
    }

    IpfixMessage decode(const std::vector<uint8_t>& b) {
        IpfixMessage m;
        if (b.size() < 16) { m.error = "short"; return m; }
        m.version = rd16(b, 0);
        m.length = rd16(b, 2);
        m.export_time = rd32(b, 4);
        m.sequence = rd32(b, 8);
        m.domain = rd32(b, 12);
        if (m.length != b.size()) { m.error = "length mismatch"; return m; }
        size_t pos = 16;
        while (pos < b.size()) {
            if (pos + 4 > b.size()) { m.error = "set header overrun"; return m; }
            uint16_t set_id = rd16(b, pos);
            size_t set_len = rd16(b, pos + 2);
            if (set_len < 4 || pos + set_len > b.size()) { m.error = "set length"; return m; }
            m.set_lengths.push_back(set_len);
            size_t end = pos + set_len;
            size_t p = pos + 4;
            if (set_id == 2 || set_id == 3) {
                while (end - p >= 4) {
                    IpfixTemplate t;
                    t.options = set_id == 3;
                    t.id = rd16(b, p);
                    uint16_t count = rd16(b, p + 2);
                    p += 4;
                    if (t.options) { t.scope_count = rd16(b, p); p += 2; }
                    if (t.id == 0) break;  // padding
                    for (uint16_t i = 0; i < count; ++i) {
                        if (p + 4 > end) { m.error = "template overrun"; return m; }
                        IpfixField f;
                        uint16_t raw = rd16(b, p);
                        f.length = rd16(b, p + 2);
                        p += 4;
                        f.id = raw & 0x7FFF;
                        if (raw & 0x8000) {
                            if (p + 4 > end) { m.error = "PEN overrun"; return m; }
                            f.enterprise = rd32(b, p);
                            p += 4;
                        }
                        t.fields.push_back(f);
                    }
                    cache[t.id] = t;
                    m.templates.push_back(t);
                }
                for (; p < end; ++p)
                    if (b[p] != 0) { m.error = "nonzero template padding"; return m; }
            } else if (set_id >= 256) {
                auto it = cache.find(set_id);
                if (it == cache.end()) { m.error = "unknown template"; return m; }
                const IpfixTemplate& t = it->second;
                IpfixDataSet ds;
                ds.template_id = set_id;
                ds.length = set_len;
                size_t min_rec = 0;
                for (const auto& f : t.fields) min_rec += f.length == 0xFFFF ? 1 : f.length;
                while (end - p >= min_rec && !(allZero(b, p, end))) {
                    IpfixRecord r;
                    for (const auto& f : t.fields) {
                        size_t len = f.length;
                        if (len == 0xFFFF) {
                            len = b[p++];
                            if (len == 255) { len = rd16(b, p); p += 2; }
                        }
                        if (p + len > end) { m.error = "record overrun"; return m; }
                        std::string key = f.enterprise
                            ? std::to_string(f.enterprise) + "." + std::to_string(f.id)
                            : std::to_string(f.id);
                        r[key] = std::vector<uint8_t>(b.begin() + static_cast<long>(p),
                                                      b.begin() + static_cast<long>(p + len));
                        p += len;
                    }
                    ds.records.push_back(std::move(r));
                }
                for (; p < end; ++p)
                    if (b[p] != 0) { m.error = "nonzero data padding"; return m; }
                m.data.push_back(std::move(ds));
            } else {
                m.error = "bad set id";
                return m;
            }
            pos = end;
        }
        m.ok = true;
        return m;
    }

    static bool allZero(const std::vector<uint8_t>& b, size_t p, size_t end) {
        for (; p < end; ++p) if (b[p] != 0) return false;
        return true;
    }
};

std::string str(const IpfixRecord& r, const std::string& k) {
    auto it = r.find(k);
    if (it == r.end()) return "<missing>";
    return std::string(it->second.begin(), it->second.end());
}
uint64_t uintField(const IpfixRecord& r, const std::string& k) {
    auto it = r.find(k);
    if (it == r.end()) return 0xDEADBEEFull;
    uint64_t v = 0;
    for (uint8_t c : it->second) v = (v << 8) | c;
    return v;
}
int sint8(const IpfixRecord& r, const std::string& k) {
    auto it = r.find(k);
    if (it == r.end() || it->second.size() != 1) return 9999;
    return static_cast<int8_t>(it->second[0]);
}
size_t fieldLen(const IpfixRecord& r, const std::string& k) {
    auto it = r.find(k);
    return it == r.end() ? size_t(9999) : it->second.size();
}

std::vector<uint8_t> hex(const std::string& s) {
    std::vector<uint8_t> out;
    int nib = -1;
    for (char c : s) {
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else continue;
        if (nib < 0) nib = v;
        else { out.push_back(static_cast<uint8_t>(nib * 16 + v)); nib = -1; }
    }
    return out;
}

std::string dump(const std::vector<uint8_t>& b) {
    static const char* h = "0123456789ABCDEF";
    std::string s;
    for (uint8_t c : b) { s += h[c >> 4]; s += h[c & 15]; s += ' '; }
    return s;
}

PskReporterReceiver sampleRx() { return {"N1DQ", "FN42hn", "Homebrew v5.6", ""}; }

PskReporterSpot spot(const std::string& call, uint64_t f, uint32_t t, const std::string& mode = "SKYWAVE",
                     bool has_snr = false, int snr = 0, const std::string& loc = "") {
    PskReporterSpot s;
    s.callsign = call;
    s.frequency_hz = f;
    s.flow_start_unix = t;
    s.mode = mode;
    s.has_snr = has_snr;
    s.snr_db = snr;
    s.locator = loc;
    return s;
}

// Collects datagrams sent by a client.
struct Capture {
    std::mutex m;
    std::vector<std::vector<uint8_t>> sent;
    std::atomic<bool> fail{false};
    std::function<bool(const std::vector<uint8_t>&)> fn() {
        return [this](const std::vector<uint8_t>& d) {
            std::lock_guard<std::mutex> l(m);
            sent.push_back(d);
            return !fail.load();
        };
    }
    size_t count() { std::lock_guard<std::mutex> l(m); return sent.size(); }
    std::vector<uint8_t> at(size_t i) { std::lock_guard<std::mutex> l(m); return sent.at(i); }
};

const char* kTestHost = "pskreporter.invalid";  // never resolvable (RFC 2606)

// Spec example datagrams (https://pskreporter.info/pskdev.html "Cookie Cutter")
const char* kSpecFull =
    "00 0A 00 AC 47 95 32 72 00 00 00 01 00 00 00 00"
    "00 03 00 24  99 92 00 03 00 01  80 02 FF FF 00 00 76 8F  80 04 FF FF 00 00 76 8F  80 08 FF FF 00 00 76 8F  00 00"
    "00 02 00 2C  99 93 00 05  80 01 FF FF 00 00 76 8F  80 05 00 04 00 00 76 8F  80 0A FF FF 00 00 76 8F  80 0B 00 01 00 00 76 8F  00 96 00 04"
    "99 92 00 20  04 4E 31 44 51  06 46 4E 34 32 68 6E  0D 48 6F 6D 65 62 72 65 77 20 76 35 2E 36  00 00"
    "99 93 00 2C  04 4E 31 44 51  00 D6 B3 27  03 50 53 4B  01  47 95 32 54"
    "             06 4B 42 31 4D 42 58  00 D6 B4 CB  03 50 53 4B  01  47 95 32 68"
    "             00 00";
// Same data later in the session (sequence 4), no templates. The spec's text
// has mode bytes "50 53 4C" ("PSL") in this example; feeding "PSL" reproduces
// it exactly.
const char* kSpecNoTemplates =
    "00 0A 00 5C 47 95 32 72 00 00 00 04 00 00 00 00"
    "99 92 00 20  04 4E 31 44 51  06 46 4E 34 32 68 6E  0D 48 6F 6D 65 62 72 65 77 20 76 35 2E 36  00 00"
    "99 93 00 2C  04 4E 31 44 51  00 D6 B3 27  03 50 53 4C  01  47 95 32 54"
    "             06 4B 42 31 4D 42 58  00 D6 B4 CB  03 50 53 4C  01  47 95 32 68"
    "             00 00";

}  // namespace

// ---------------------------------------------------------------------------
// Encoder: spec byte layout
// ---------------------------------------------------------------------------

TEST(spec_sample_datagram_byte_exact) {
    std::vector<PskReporterSpot> spots{spot("N1DQ", 14070567, 1200960084, "PSK"),
                                       spot("KB1MBX", 14070987, 1200960104, "PSK")};
    auto d = PskReporterClient::encode(sampleRx(), spots, 1, 1200960114, 0, true);
    ASSERT_EQ(d.size(), size_t(1));
    auto expect = hex(kSpecFull);
    ASSERT_EQ(expect.size(), size_t(0xAC));
    if (d[0] != expect) {
        std::cerr << "  got    " << dump(d[0]) << "\n  expect " << dump(expect) << "\n";
    }
    ASSERT_TRUE(d[0] == expect);
    PASS();
}

TEST(spec_sample_without_templates_byte_exact) {
    std::vector<PskReporterSpot> spots{spot("N1DQ", 14070567, 1200960084, "PSL"),
                                       spot("KB1MBX", 14070987, 1200960104, "PSL")};
    auto d = PskReporterClient::encode(sampleRx(), spots, 4, 1200960114, 0, false);
    ASSERT_EQ(d.size(), size_t(1));
    ASSERT_TRUE(d[0] == hex(kSpecNoTemplates));
    PASS();
}

TEST(spec_template_variants_match_spec_bytes) {
    // Receiver template with antennaInformation (spec's second receiver
    // descriptor, but under its own template id 0x9994).
    PskReporterReceiver rx{"K1AAA", "FN42", "Skywave Link 1.0", "Dipole"};
    auto d = PskReporterClient::encode(rx, {spot("K1BBB", 14100000, 1000, "SKYWAVE", false, 0, "FN31")},
                                       0, 2000, 7, true);
    ASSERT_EQ(d.size(), size_t(1));
    auto rx_tpl = hex("00 03 00 2C 99 94 00 04 00 01 80 02 FF FF 00 00 76 8F 80 04 FF FF 00 00 76 8F"
                      " 80 08 FF FF 00 00 76 8F 80 09 FF FF 00 00 76 8F 00 00");
    ASSERT_TRUE(std::equal(rx_tpl.begin(), rx_tpl.end(), d[0].begin() + 16));
    // Sender with senderLocator: spec's second sender descriptor (id 0x9995)
    auto tx_tpl = hex("00 02 00 34 99 95 00 06 80 01 FF FF 00 00 76 8F 80 05 00 04 00 00 76 8F"
                      " 80 0A FF FF 00 00 76 8F 80 0B 00 01 00 00 76 8F 80 03 FF FF 00 00 76 8F 00 96 00 04");
    ASSERT_TRUE(std::equal(tx_tpl.begin(), tx_tpl.end(), d[0].begin() + 16 + 0x2C));
    PASS();
}

// ---------------------------------------------------------------------------
// Encoder: decode with the independent parser, all fields
// ---------------------------------------------------------------------------

TEST(decode_all_fields_all_variants) {
    PskReporterReceiver rx{"K1AAA", "FN42ab", "Skywave Link 1.2.3", "Vertical 10m"};
    std::vector<PskReporterSpot> spots{
        spot("K1BBB", 14100000, 1700000001, "SKYWAVE"),
        spot("K1CCC", 7050000, 1700000002, "SKYWAVE", true, -12),
        spot("K1DDD", 3600000, 1700000003, "SKYWAVE", false, 0, "EM12"),
        spot("K1EEE", 28123456, 1700000004, "SKYWAVE", true, 25, "IO91wm"),
        spot("K1FFF", 21000000, 1700000005, "SKYWAVE", true, -128),
    };
    auto d = PskReporterClient::encode(rx, spots, 42, 1700000100, 0xA1B2C3D4, true);
    ASSERT_EQ(d.size(), size_t(1));
    IpfixDecoder dec;
    auto m = dec.decode(d[0]);
    ASSERT_TRUE(m.ok);
    ASSERT_EQ(m.version, uint16_t(10));
    ASSERT_EQ(size_t(m.length), d[0].size());
    ASSERT_EQ(m.export_time, uint32_t(1700000100));
    ASSERT_EQ(m.sequence, uint32_t(42));
    ASSERT_EQ(m.domain, uint32_t(0xA1B2C3D4));
    for (size_t l : m.set_lengths) ASSERT_EQ(l % 4, size_t(0));

    // Templates: receiver (options) + the 4 sender variants used
    ASSERT_EQ(m.templates.size(), size_t(5));
    ASSERT_TRUE(m.templates[0].options);
    ASSERT_EQ(m.templates[0].id, PskReporterClient::kTplReceiverAntenna);
    ASSERT_EQ(m.templates[0].scope_count, uint16_t(1));
    ASSERT_EQ(m.templates[0].fields.size(), size_t(4));
    const uint16_t rx_ids[] = {2, 4, 8, 9};
    for (size_t i = 0; i < 4; ++i) {
        ASSERT_EQ(m.templates[0].fields[i].id, rx_ids[i]);
        ASSERT_EQ(m.templates[0].fields[i].enterprise, uint32_t(30351));
        ASSERT_EQ(m.templates[0].fields[i].length, uint16_t(0xFFFF));
    }
    for (size_t i = 1; i < m.templates.size(); ++i) {
        const auto& t = m.templates[i];
        ASSERT_FALSE(t.options);
        ASSERT_EQ(t.fields.front().id, uint16_t(1));     // senderCallsign first
        ASSERT_EQ(t.fields[1].id, uint16_t(5));          // frequency, 4 bytes
        ASSERT_EQ(t.fields[1].length, uint16_t(4));
        ASSERT_EQ(t.fields.back().id, uint16_t(150));    // flowStartSeconds, IANA
        ASSERT_EQ(t.fields.back().enterprise, uint32_t(0));
        ASSERT_EQ(t.fields.back().length, uint16_t(4));
    }

    // Data: receiver first
    ASSERT_GE(m.data.size(), size_t(2));
    ASSERT_EQ(m.data[0].template_id, PskReporterClient::kTplReceiverAntenna);
    ASSERT_EQ(m.data[0].records.size(), size_t(1));
    const auto& r = m.data[0].records[0];
    ASSERT_EQ(str(r, "30351.2"), std::string("K1AAA"));
    ASSERT_EQ(str(r, "30351.4"), std::string("FN42ab"));
    ASSERT_EQ(str(r, "30351.8"), std::string("Skywave Link 1.2.3"));
    ASSERT_EQ(str(r, "30351.9"), std::string("Vertical 10m"));

    std::map<std::string, IpfixRecord> by_call;
    std::map<std::string, uint16_t> tpl_of;
    for (size_t i = 1; i < m.data.size(); ++i)
        for (const auto& rec : m.data[i].records) {
            by_call[str(rec, "30351.1")] = rec;
            tpl_of[str(rec, "30351.1")] = m.data[i].template_id;
        }
    ASSERT_EQ(by_call.size(), spots.size());
    for (const auto& s : spots) {
        ASSERT_TRUE(by_call.count(s.callsign) == 1);
        const auto& rec = by_call[s.callsign];
        ASSERT_EQ(tpl_of[s.callsign], PskReporterClient::senderTemplateFor(s));
        ASSERT_EQ(uintField(rec, "30351.5"), s.frequency_hz);
        ASSERT_EQ(fieldLen(rec, "30351.5"), size_t(4));
        ASSERT_EQ(str(rec, "30351.10"), s.mode);
        ASSERT_EQ(uintField(rec, "30351.11"), uint64_t(1));   // informationSource = automatic
        ASSERT_EQ(fieldLen(rec, "30351.11"), size_t(1));
        ASSERT_EQ(uintField(rec, "150"), uint64_t(s.flow_start_unix));
        if (s.has_snr) ASSERT_EQ(sint8(rec, "30351.6"), s.snr_db);
        else ASSERT_TRUE(rec.count("30351.6") == 0);
        if (!s.locator.empty()) ASSERT_EQ(str(rec, "30351.3"), s.locator);
        else ASSERT_TRUE(rec.count("30351.3") == 0);
        ASSERT_TRUE(rec.count("30351.7") == 0);  // iMD never reported
    }
    PASS();
}

TEST(snr_clamped_and_strings_truncated) {
    PskReporterReceiver rx{"K1AAA", "FN42", std::string(300, 'S'), ""};
    auto d = PskReporterClient::encode(rx, {spot("K1BBB", 14100000, 1, "SKYWAVE", true, -500),
                                            spot("K1CCC", 14100000, 1, "SKYWAVE", true, 400)},
                                       0, 1, 1, true);
    IpfixDecoder dec;
    auto m = dec.decode(d.at(0));
    ASSERT_TRUE(m.ok);
    ASSERT_EQ(str(m.data[0].records[0], "30351.8").size(), size_t(254));
    ASSERT_EQ(sint8(m.data[1].records[0], "30351.6"), -128);
    ASSERT_EQ(sint8(m.data[1].records[1], "30351.6"), 127);
    PASS();
}

TEST(receiver_only_datagram_when_no_spots) {
    auto d = PskReporterClient::encode(sampleRx(), {}, 0, 5, 6, false);
    ASSERT_EQ(d.size(), size_t(1));
    ASSERT_EQ(d[0].size(), size_t(16 + 0x20));
    PASS();
}

TEST(mtu_splitting) {
    std::vector<PskReporterSpot> spots;
    for (int i = 0; i < 300; ++i)
        spots.push_back(spot("K" + std::to_string(i) + "XYZ", 14000000 + uint64_t(i), 1000u + uint32_t(i),
                             "SKYWAVE", i % 2 == 0, -i % 30, i % 3 == 0 ? "FN42" : ""));
    auto enc = PskReporterClient::encodeDetailed(sampleRx(), spots, 100, 2000, 9, true, 1400);
    ASSERT_GT(enc.datagrams.size(), size_t(1));
    ASSERT_TRUE(enc.skipped.empty());
    IpfixDecoder dec;
    std::set<std::string> seen;
    uint32_t expect_seq = 100;
    for (size_t k = 0; k < enc.datagrams.size(); ++k) {
        const auto& d = enc.datagrams[k];
        ASSERT_LE(d.size(), size_t(1400));
        auto m = dec.decode(d);
        ASSERT_TRUE(m.ok);
        ASSERT_EQ(m.sequence, expect_seq);
        ASSERT_EQ(m.domain, uint32_t(9));
        ASSERT_FALSE(m.templates.empty());            // templates in every datagram
        ASSERT_EQ(m.data[0].records.size(), size_t(1));  // receiver record in every datagram
        size_t n = 0;
        for (size_t i = 1; i < m.data.size(); ++i)
            for (const auto& r : m.data[i].records) {
                ASSERT_TRUE(seen.insert(str(r, "30351.1")).second);
                ++n;
            }
        ASSERT_EQ(n, enc.spot_indices[k].size());
        expect_seq += static_cast<uint32_t>(n);
        if (k + 1 < enc.datagrams.size()) ASSERT_GT(d.size(), size_t(1400 - 100));  // well filled
    }
    ASSERT_EQ(seen.size(), spots.size());

    // A spot that cannot fit even in an empty datagram is skipped, not looped on.
    auto tiny = PskReporterClient::encodeDetailed(sampleRx(), {spot("K1BBB", 1, 1)}, 0, 0, 0, true, 120);
    ASSERT_EQ(tiny.skipped.size(), size_t(1));
    PASS();
}

// ---------------------------------------------------------------------------
// Client behaviour
// ---------------------------------------------------------------------------

TEST(timed_upload_via_injected_transport) {
    Capture cap;
    PskReporterClient c({"K1AAA", "FN42", "Skywave Link test", ""}, kTestHost);
    c.setTransportForTest(cap.fn());
    c.setStartJitterForTest(17);
    const uint64_t t0 = 1700000000;
    c.poll(t0);
    ASSERT_EQ(c.nextUploadDueUnix(), t0 + 300 + 17);
    c.addSpot(spot("k1bbb", 14100000, uint32_t(t0 + 5), "SKYWAVE", true, -3, "EM00"));
    ASSERT_EQ(c.queuedSpots(), size_t(1));
    c.poll(t0 + 316);
    c.waitIdleForTest();
    ASSERT_EQ(cap.count(), size_t(0));
    c.poll(t0 + 317);
    c.waitIdleForTest();
    ASSERT_EQ(cap.count(), size_t(1));
    ASSERT_EQ(c.queuedSpots(), size_t(0));
    ASSERT_EQ(c.spotsSent(), uint64_t(1));
    ASSERT_EQ(c.lastUploadUnix(), t0 + 317);
    ASSERT_EQ(c.lastError(), std::string());
    ASSERT_EQ(c.nextUploadDueUnix(), t0 + 617);

    IpfixDecoder dec;
    auto m = dec.decode(cap.at(0));
    ASSERT_TRUE(m.ok);
    ASSERT_EQ(m.export_time, uint32_t(t0 + 317));
    ASSERT_EQ(m.sequence, uint32_t(0));
    ASSERT_EQ(m.domain, c.observationDomainId());
    ASSERT_EQ(str(m.data[1].records[0], "30351.1"), std::string("K1BBB"));  // upper-cased
    ASSERT_EQ(sint8(m.data[1].records[0], "30351.6"), -3);

    // Nothing queued -> grid advances, nothing sent.
    c.poll(t0 + 617);
    c.waitIdleForTest();
    ASSERT_EQ(cap.count(), size_t(1));
    ASSERT_EQ(c.nextUploadDueUnix(), t0 + 917);
    PASS();
}

TEST(rate_limiting_and_upload_now) {
    Capture cap;
    PskReporterClient c({"K1AAA", "FN42", "sw", ""}, kTestHost);
    c.setTransportForTest(cap.fn());
    c.setStartJitterForTest(0);
    const uint64_t t0 = 1000000;
    c.poll(t0);
    ASSERT_FALSE(c.flushNow(t0 + 1));  // nothing queued
    ASSERT_CONTAINS(c.lastError(), "no spots");
    c.addSpot(spot("K1BBB", 14100000, uint32_t(t0)));
    ASSERT_TRUE(c.flushNow(t0 + 10));  // first upload may go immediately
    c.waitIdleForTest();
    ASSERT_EQ(cap.count(), size_t(1));

    c.addSpot(spot("K1CCC", 14100000, uint32_t(t0 + 20)));
    ASSERT_FALSE(c.flushNow(t0 + 100));
    ASSERT_CONTAINS(c.lastError(), "5 minutes");
    ASSERT_CONTAINS(c.lastError(), "210 s");
    // Timer grid point at t0+300 is only 290 s after the upload: skipped.
    c.poll(t0 + 300);
    c.waitIdleForTest();
    ASSERT_EQ(cap.count(), size_t(1));
    ASSERT_EQ(c.queuedSpots(), size_t(1));
    ASSERT_EQ(c.nextUploadDueUnix(), t0 + 600);
    c.poll(t0 + 599);
    c.waitIdleForTest();
    ASSERT_EQ(cap.count(), size_t(1));
    c.poll(t0 + 600);
    c.waitIdleForTest();
    ASSERT_EQ(cap.count(), size_t(2));

    // Upload Now 5 minutes later is allowed again.
    c.addSpot(spot("K1DDD", 14100000, uint32_t(t0 + 650)));
    ASSERT_FALSE(c.flushNow(t0 + 899));
    ASSERT_TRUE(c.flushNow(t0 + 900));
    c.waitIdleForTest();
    ASSERT_EQ(cap.count(), size_t(3));
    ASSERT_EQ(c.spotsSent(), uint64_t(3));
    PASS();
}

TEST(dedup_rules) {
    Capture cap;
    PskReporterClient c({"K1AAA", "FN42", "sw", ""}, kTestHost);
    c.setTransportForTest(cap.fn());
    const uint32_t t = 2000000;
    c.addSpot(spot("K1BBB", 14100000, t));
    c.addSpot(spot("k1bbb", 14100500, t + 60));      // same call/band, 1 min: dup
    c.addSpot(spot("K1BBB", 7050000, t + 120));      // other band but < 5 min: dup (spec)
    ASSERT_EQ(c.queuedSpots(), size_t(1));
    c.addSpot(spot("K1BBB", 14100000, t + 400));     // same band, unchanged, < 1 h: dup
    ASSERT_EQ(c.queuedSpots(), size_t(1));
    c.addSpot(spot("K1BBB", 7050000, t + 400));      // band changed, > 5 min: reported
    ASSERT_EQ(c.queuedSpots(), size_t(2));
    c.addSpot(spot("K1BBB", 7050000, t + 800, "OTHER"));  // mode changed, > 5 min
    ASSERT_EQ(c.queuedSpots(), size_t(3));
    c.addSpot(spot("K1BBB", 7050000, t + 800 + 3599, "OTHER"));  // unchanged < 1 h
    ASSERT_EQ(c.queuedSpots(), size_t(3));
    c.addSpot(spot("K1BBB", 7050000, t + 800 + 3600, "OTHER"));  // unchanged, 1 h later
    ASSERT_EQ(c.queuedSpots(), size_t(4));
    ASSERT_EQ(c.duplicateSpots(), uint64_t(4));
    c.addSpot(spot("K1CCC", 14100000, t));           // other call: independent
    ASSERT_EQ(c.queuedSpots(), size_t(5));
    // Invalid spots rejected
    c.addSpot(spot("  ", 14100000, t));
    c.addSpot(spot("K1ZZZ", 0, t));
    ASSERT_EQ(c.queuedSpots(), size_t(5));
    PASS();
}

TEST(template_resend_cadence_and_sequence) {
    Capture cap;
    PskReporterClient c({"K1AAA", "FN42", "sw", ""}, kTestHost);
    c.setTransportForTest(cap.fn());
    uint64_t t = 3000000;
    auto upload = [&](const std::string& call, bool snr) {
        c.addSpot(spot(call, 14100000, uint32_t(t), "SKYWAVE", snr, 1));
        c.addSpot(spot(call + "X", 14100000, uint32_t(t), "SKYWAVE", snr, 1));
        bool ok = c.flushNow(t);
        c.waitIdleForTest();
        t += 300;
        return ok;
    };
    IpfixDecoder dec;
    std::vector<IpfixMessage> msgs;
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(upload("K" + std::to_string(i) + "AA", false));
        msgs.push_back(dec.decode(cap.at(cap.count() - 1)));
        ASSERT_TRUE(msgs.back().ok);
    }
    ASSERT_FALSE(msgs[0].templates.empty());
    ASSERT_FALSE(msgs[1].templates.empty());
    ASSERT_FALSE(msgs[2].templates.empty());
    ASSERT_TRUE(msgs[3].templates.empty());
    ASSERT_TRUE(msgs[4].templates.empty());
    // Sequence counts reports, not packets: 2 spots per datagram.
    for (size_t i = 0; i < msgs.size(); ++i) ASSERT_EQ(msgs[i].sequence, uint32_t(2 * i));
    for (const auto& m : msgs) ASSERT_EQ(m.domain, c.observationDomainId());

    // A sender variant not yet announced brings its template along (and only
    // the new sender template plus the receiver template is required).
    ASSERT_TRUE(upload("K9AA", true));
    auto m6 = dec.decode(cap.at(cap.count() - 1));
    ASSERT_TRUE(m6.ok);
    ASSERT_EQ(m6.templates.size(), size_t(2));
    ASSERT_EQ(m6.templates[1].id, PskReporterClient::kTplSenderSnr);

    // Hourly re-send.
    const uint64_t first_tpl = 3000000 + 600;  // third template datagram
    t = first_tpl + 3599;
    ASSERT_TRUE(upload("K7AA", false));
    ASSERT_TRUE(dec.decode(cap.at(cap.count() - 1)).templates.empty());
    t = first_tpl + 3600 + 300;
    ASSERT_TRUE(upload("K8AA", false));
    ASSERT_FALSE(dec.decode(cap.at(cap.count() - 1)).templates.empty());
    PASS();
}

TEST(failed_templates_do_not_count) {
    Capture cap;
    PskReporterClient c({"K1AAA", "FN42", "sw", ""}, kTestHost);
    c.setTransportForTest(cap.fn());
    cap.fail = true;
    uint64_t t = 4000000;
    c.addSpot(spot("K1BBB", 14100000, uint32_t(t)));
    ASSERT_TRUE(c.flushNow(t));
    c.waitIdleForTest();
    ASSERT_EQ(c.queuedSpots(), size_t(1));  // re-queued
    ASSERT_EQ(c.spotsSent(), uint64_t(0));
    ASSERT_FALSE(c.lastError().empty());
    ASSERT_EQ(c.lastUploadUnix(), uint64_t(0));
    t += 300;
    ASSERT_TRUE(c.flushNow(t));
    c.waitIdleForTest();
    ASSERT_EQ(c.queuedSpots(), size_t(1));
    t += 300;
    ASSERT_TRUE(c.flushNow(t));  // third failure: given up
    c.waitIdleForTest();
    ASSERT_EQ(c.queuedSpots(), size_t(0));
    ASSERT_EQ(c.droppedSpots(), uint64_t(1));

    cap.fail = false;
    IpfixDecoder dec;
    for (int i = 0; i < 3; ++i) {
        t += 300;
        c.addSpot(spot("K2B" + std::to_string(i), 14100000, uint32_t(t)));
        ASSERT_TRUE(c.flushNow(t));
        c.waitIdleForTest();
        ASSERT_FALSE(dec.decode(cap.at(cap.count() - 1)).templates.empty());
    }
    ASSERT_EQ(c.lastError(), std::string());
    PASS();
}

TEST(bounded_queue_drops_oldest) {
    PskReporterClient c({"K1AAA", "FN42", "sw", ""}, kTestHost);
    Capture cap;
    c.setTransportForTest(cap.fn());
    const size_t n = PskReporterClient::kMaxQueuedSpots + 5;
    for (size_t i = 0; i < n; ++i) c.addSpot(spot("K" + std::to_string(i) + "Q", 14100000, 5000));
    ASSERT_EQ(c.queuedSpots(), PskReporterClient::kMaxQueuedSpots);
    ASSERT_EQ(c.droppedSpots(), uint64_t(5));
    ASSERT_CONTAINS(c.lastError(), "queue full");
    PASS();
}

TEST(full_datagram_sent_early) {
    Capture cap;
    PskReporterClient c({"K1AAA", "FN42", "sw", ""}, kTestHost);
    c.setTransportForTest(cap.fn());
    c.setMaxDatagramBytesForTest(200);
    const uint64_t t0 = 6000000;
    c.poll(t0);
    for (int i = 0; i < 20; ++i) c.addSpot(spot("K" + std::to_string(i) + "FUL", 14100000, uint32_t(t0)));
    c.poll(t0 + 1);  // far from due, but datagrams are full
    c.waitIdleForTest();
    ASSERT_GT(cap.count(), size_t(0));
    ASSERT_GT(c.queuedSpots(), size_t(0));   // the partial last datagram waits
    ASSERT_LT(c.queuedSpots(), size_t(20));
    for (size_t i = 0; i < cap.count(); ++i) ASSERT_LE(cap.at(i).size(), size_t(200));
    ASSERT_EQ(c.spotsSent() + c.queuedSpots(), uint64_t(20));
    PASS();
}

TEST(no_receiver_callsign_no_upload) {
    Capture cap;
    PskReporterClient c({"", "FN42", "sw", ""}, kTestHost);
    c.setTransportForTest(cap.fn());
    c.addSpot(spot("K1BBB", 14100000, 7000000));
    ASSERT_FALSE(c.flushNow(7000000));
    ASSERT_CONTAINS(c.lastError(), "callsign");
    c.setReceiver({"K1AAA", "FN42", "sw", ""});
    ASSERT_TRUE(c.flushNow(7000001));
    c.waitIdleForTest();
    ASSERT_EQ(cap.count(), size_t(1));
    PASS();
}

TEST(poll_never_blocks_on_slow_transport) {
    std::atomic<int> calls{0};
    PskReporterClient c({"K1AAA", "FN42", "sw", ""}, kTestHost);
    c.setTransportForTest([&](const std::vector<uint8_t>&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        ++calls;
        return true;
    });
    c.setStartJitterForTest(0);
    const uint64_t t0 = 8000000;
    c.poll(t0);
    c.addSpot(spot("K1BBB", 14100000, uint32_t(t0)));
    auto start = std::chrono::steady_clock::now();
    c.poll(t0 + 300);
    for (int i = 0; i < 100; ++i) c.poll(t0 + 300);
    ASSERT_TRUE(c.uploadInProgress());
    c.addSpot(spot("K1CCC", 14100000, uint32_t(t0 + 301)));
    ASSERT_FALSE(c.flushNow(t0 + 900));  // busy
    ASSERT_CONTAINS(c.lastError(), "in progress");
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    ASSERT_LT(ms, 200);
    ASSERT_EQ(calls.load(), 0);
    c.waitIdleForTest();
    ASSERT_EQ(calls.load(), 1);
    ASSERT_EQ(c.spotsSent(), uint64_t(1));
    PASS();
}

TEST(destructor_does_not_hang_with_idle_worker) {
    { PskReporterClient c({"K1AAA", "FN42", "sw", ""}, kTestHost); }
    PASS();
}

TEST_MAIN()
