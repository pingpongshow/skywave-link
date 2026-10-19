// B2F (Winlink Message Structure) codec tests
#include "test_macros.hpp"
#include "skywave_link/b2f.hpp"

using namespace skylink;
using namespace std::string_literals;

namespace {

std::vector<uint8_t> bytesOf(const std::string& s) { return {s.begin(), s.end()}; }

bool decodeText(const std::string& wire, B2FHeader& h, std::string& body, B2FAttachments& att,
                std::string* err = nullptr) {
    return B2FCodec().decode(bytesOf(wire), h, body, att, err);
}

// A message in the published format (winlink.org/B2F), as the CMS sends it.
const std::string kSample =
    "Mid: 3K9ZQ1TX0ABC\r\n"
    "Body: 13\r\n"
    "Content-Transfer-Encoding: 8bit\r\n"
    "Content-Type: text/plain; charset=ISO-8859-1\r\n"
    "Date: 2026/01/15 12:34\r\n"
    "File: 5 a.txt\r\n"
    "File: 3 b c.bin\r\n"
    "From: SMTP:someone@example.com\r\n"
    "Mbo: SMTP\r\n"
    "Subject: Hello\r\n"
    "To: K1AAA\r\n"
    "Cc: SMTP:other@example.org\r\n"
    "Type: Private\r\n"
    "\r\n"
    "Hi there\r\n!\r\n"
    "\r\n"
    "abcde\r\n"
    "\x01\x02\x03\r\n";

}  // namespace

TEST(b2f_encode_exact_bytes) {
    B2FHeader h;
    h.message_id = "ABC123DEF456";
    h.date_time = "2026/01/15 12:00";
    h.from = "K1AAA";
    h.to = {"user@example.com", "W1AAA@winlink.org", "k2bbb"};
    h.cc = {"N0CALL"};
    h.subject = "Test";
    h.extra = {{"Content-Type", "text/plain; charset=ISO-8859-1"}};
    B2FAttachments att = {{"x.bin", {0x00, 0xFF}}};

    std::vector<uint8_t> out;
    ASSERT_TRUE(B2FCodec().encode(h, "Body\r\n", att, out));
    const std::string expected =
        "Mid: ABC123DEF456\r\n"
        "Date: 2026/01/15 12:00\r\n"
        "Type: Private\r\n"
        "From: K1AAA\r\n"
        "To: SMTP:user@example.com\r\n"
        "To: W1AAA\r\n"
        "To: K2BBB\r\n"
        "Cc: N0CALL\r\n"
        "Subject: Test\r\n"
        "Mbo: K1AAA\r\n"
        "Content-Type: text/plain; charset=ISO-8859-1\r\n"
        "Body: 6\r\n"
        "File: 2 x.bin\r\n"
        "\r\n"
        "Body\r\n\r\n"
        "\x00\xFF\r\n"s;
    ASSERT_EQ(std::string(out.begin(), out.end()), expected);
    PASS();
}

TEST(b2f_decode_sample) {
    B2FHeader h;
    std::string body;
    B2FAttachments att;
    std::string err;
    ASSERT_TRUE(decodeText(kSample, h, body, att, &err));
    ASSERT_EQ(h.message_id, std::string("3K9ZQ1TX0ABC"));
    ASSERT_EQ(h.date_time, std::string("2026/01/15 12:34"));
    ASSERT_EQ(h.from, std::string("SMTP:someone@example.com"));
    ASSERT_EQ(h.from_address, std::string("someone@example.com"));
    ASSERT_EQ(h.from_callsign, std::string("someone"));
    ASSERT_EQ(h.to.size(), size_t(1));
    ASSERT_EQ(h.to[0], std::string("K1AAA"));
    ASSERT_EQ(h.to_callsign, std::string("K1AAA"));
    ASSERT_TRUE(h.to_address.empty());
    ASSERT_EQ(h.cc.size(), size_t(1));
    ASSERT_EQ(h.mbo, std::string("SMTP"));
    ASSERT_EQ(h.subject, std::string("Hello"));
    ASSERT_EQ(h.type_name, std::string("Private"));
    ASSERT_EQ(h.body_size, uint32_t(13));
    ASSERT_EQ(h.extraValue("content-type"), std::string("text/plain; charset=ISO-8859-1"));
    ASSERT_EQ(body, std::string("Hi there\r\n!\r\n"));
    ASSERT_EQ(att.size(), size_t(2));
    ASSERT_EQ(att[0].first, std::string("a.txt"));
    ASSERT_TRUE(att[0].second == bytesOf("abcde"));
    ASSERT_EQ(att[1].first, std::string("b c.bin"));
    ASSERT_TRUE(att[1].second == bytesOf("\x01\x02\x03"));
    PASS();
}

TEST(b2f_round_trip_with_attachments) {
    B2FHeader h;
    h.message_id = "RT1";
    h.from = "K1AAA";
    h.to = {"K2BBB"};
    h.subject = "File transfer";
    h.type = B2FMessageType::Bulletin;
    std::vector<uint8_t> png = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    B2FAttachments att = {{"test.png", png}, {"empty.txt", {}}};
    B2FCodec codec;
    const auto wire = codec.encode(h, "See attached file.", att);
    ASSERT_FALSE(wire.empty());

    B2FHeader d;
    std::string body;
    B2FAttachments datt;
    ASSERT_TRUE(codec.decode(wire, d, body, datt));
    ASSERT_EQ(body, std::string("See attached file."));
    ASSERT_EQ(datt.size(), size_t(2));
    ASSERT_TRUE(datt[0].second == png);
    ASSERT_TRUE(datt[1].second.empty());
    ASSERT_EQ(static_cast<char>(d.type), static_cast<char>(B2FMessageType::Bulletin));
    ASSERT_FALSE(d.date_time.empty());  // filled in by encode
    PASS();
}

TEST(b2f_legacy_fields_round_trip) {
    // The shape used by the app's radio envelope.
    B2FHeader h;
    h.message_id = "LEGACY01";
    h.type = B2FMessageType::Traffic;
    h.from_callsign = "K1AAA";
    h.to_callsign = "user";
    h.to_address = "user@example.com";
    h.subject = "Email via radio";
    h.date_time = "2026/01/15 13:00";
    B2FCodec codec;
    const auto wire = codec.encode(h, "Test body", {});
    const std::string text(wire.begin(), wire.end());
    ASSERT_CONTAINS(text, "To: SMTP:user@example.com\r\n");
    ASSERT_CONTAINS(text, "Type: Private\r\n");
    ASSERT_CONTAINS(text, "Mbo: K1AAA\r\n");

    B2FHeader d;
    std::string body;
    B2FAttachments att;
    ASSERT_TRUE(codec.decode(wire, d, body, att));
    ASSERT_EQ(d.from_callsign, std::string("K1AAA"));
    ASSERT_EQ(d.to_address, std::string("user@example.com"));
    ASSERT_EQ(d.to_callsign, std::string("user"));
    ASSERT_EQ(body, std::string("Test body"));
    PASS();
}

TEST(b2f_addresses) {
    ASSERT_EQ(b2fAddress("k1aaa"), std::string("K1AAA"));
    ASSERT_EQ(b2fAddress("k1aaa@Winlink.org"), std::string("K1AAA"));
    ASSERT_EQ(b2fAddress("User@Example.com"), std::string("SMTP:User@Example.com"));
    ASSERT_EQ(b2fAddress("smtp:user@example.com"), std::string("SMTP:user@example.com"));
    ASSERT_EQ(b2fDisplayAddress("SMTP:user@example.com"), std::string("user@example.com"));
    ASSERT_EQ(b2fDisplayAddress("K1AAA"), std::string("K1AAA"));
    PASS();
}

TEST(b2f_header_values_are_single_lines) {
    B2FHeader h;
    h.message_id = "NL1";
    h.from = "K1AAA";
    h.to = {"K2BBB"};
    h.subject = "two\r\nlines";
    const auto wire = B2FCodec().encode(h, "x", {});
    const std::string text(wire.begin(), wire.end());
    ASSERT_CONTAINS(text, "Subject: two  lines\r\n");
    PASS();
}

TEST(b2f_encode_requires_mid_and_recipient) {
    B2FCodec codec;
    B2FHeader h;
    h.from = "K1AAA";
    h.to = {"K2BBB"};
    ASSERT_TRUE(codec.encode(h, "x", {}).empty());
    h.message_id = "M1";
    h.to.clear();
    ASSERT_TRUE(codec.encode(h, "x", {}).empty());
    h.to = {"K2BBB"};
    ASSERT_FALSE(codec.encode(h, "x", {}).empty());
    PASS();
}

TEST(b2f_body_at_end_without_crlf_is_accepted) {
    // wl2k-go omits the CRLF after the body when there are no attachments.
    const std::string wire = "Mid: X1\r\nTo: K1AAA\r\nBody: 3\r\n\r\nabc";
    B2FHeader h;
    std::string body;
    B2FAttachments att;
    ASSERT_TRUE(decodeText(wire, h, body, att));
    ASSERT_EQ(body, std::string("abc"));
    PASS();
}

TEST(b2f_truncated_input_fails) {
    // Every proper prefix of the sample, except the one that only lacks the
    // final CRLF, must fail.
    const size_t full = kSample.size();
    for (size_t len = 0; len + 2 < full; ++len) {
        B2FHeader h;
        std::string body;
        B2FAttachments att;
        ASSERT_FALSE(decodeText(kSample.substr(0, len), h, body, att));
        ASSERT_TRUE(body.empty());
        ASSERT_TRUE(att.empty());
    }
    PASS();
}

TEST(b2f_malformed_input_fails) {
    const std::vector<std::string> bad = {
        "",
        "\r\n",
        "Mid: test\r\nFrom: K1AAA\r\nno blank line here",
        "From: K1AAA\r\nMid: X\r\nBody: 1\r\n\r\na",            // Mid not first
        "Mid: X\r\nTo: K1AAA\r\n\r\nbody",                      // no Body field
        "Mid: X\r\nBody: 1x\r\n\r\na",                          // bad length
        "Mid: X\r\nBody: -1\r\n\r\na",
        "Mid: X\r\nBody: 99999999999\r\n\r\na",
        "Mid: X\r\nBody: 2\r\nBody: 2\r\n\r\nab",               // duplicate Body
        "Mid: \r\nBody: 1\r\n\r\na",                            // empty Mid
        "Mid: X\r\nGarbage line\r\nBody: 1\r\n\r\na",           // not "Key: value"
        "Mid: X\r\nBody: 1\r\nFile: 3\r\n\r\na\r\nabc\r\n",     // File without name
        "Mid: X\r\nBody: 1\r\nFile: x a.txt\r\n\r\na\r\nabc\r\n",
        "Mid: X\r\nBody: 1\r\n\r\naXY",                         // body not CRLF-terminated
        "Mid: X\r\nBody: 1\r\nFile: 3 a.txt\r\n\r\na\r\nab",    // attachment short
        "Mid: X\r\nBody: 1\r\nFile: 3 a.txt\r\n\r\na\r\nabcde", // attachment unterminated
        "Mid: X\r\nBody: 1\r\nFile: 3 a.txt\r\n\r\na",          // attachment missing
        "Mid: X\r\nBody: 1\r\n\r\na\r\ntrailing",               // trailing data
    };
    for (const auto& wire : bad) {
        B2FHeader h;
        std::string body;
        B2FAttachments att;
        ASSERT_FALSE(decodeText(wire, h, body, att));
    }
    PASS();
}

TEST(b2f_type_names) {
    ASSERT_EQ(std::string(b2fTypeName(B2FMessageType::Private)), std::string("Private"));
    ASSERT_EQ(std::string(b2fTypeName(B2FMessageType::PositionReport)), std::string("Position Report"));
    ASSERT_EQ(static_cast<char>(b2fTypeFromName("bulletin")), static_cast<char>(B2FMessageType::Bulletin));
    ASSERT_EQ(static_cast<char>(b2fTypeFromName("Unknown")), static_cast<char>(B2FMessageType::Private));
    PASS();
}

TEST_MAIN()
