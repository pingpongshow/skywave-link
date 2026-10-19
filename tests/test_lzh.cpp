// LZH (Winlink B2 LZHUF) tests: known answers, round trips, strict decoding.
#include "test_macros.hpp"
#include "skywave_link/lzh.hpp"

#include <numeric>
#include <random>
#include <string>
#include <vector>

using skylink::LZHCodec;

namespace {

std::vector<uint8_t> bytesOf(const std::string& s) { return {s.begin(), s.end()}; }

std::vector<uint8_t> compress(const std::vector<uint8_t>& in) { return LZHCodec().compress(in); }

bool decompress(const std::vector<uint8_t>& in, std::vector<uint8_t>* out, std::string* err = nullptr) {
    return LZHCodec().decompress(in, *out, err);
}

// Re-computes the CRC of a (modified) stream so a test reaches the decoder.
void fixCrc(std::vector<uint8_t>& v) {
    const uint16_t crc = skylink::lzhCrc16(v.data() + 2, v.size() - 2);
    v[0] = static_cast<uint8_t>(crc & 0xFF);
    v[1] = static_cast<uint8_t>(crc >> 8);
}

struct Vector {
    std::string plain;
    std::vector<uint8_t> compressed;
};

// Known answers. The first three are the samples of la5nta/wl2k-go's
// lzhuf_test.go (MIT licence); the others were computed with wl2k-go's
// lzhuf.NewB2Writer, which reproduces the reference Winlink output.
const std::vector<Vector>& vectors() {
    static const std::vector<Vector> v = {
        {"\n", {0x0e, 0x8f, 0x01, 0x00, 0x00, 0x00, 0xcb, 0x00}},
        {"foo", {0xb6, 0x47, 0x03, 0x00, 0x00, 0x00, 0xf9, 0x7e, 0xf1, 0x00}},
        {"bar", {0xc7, 0xef, 0x03, 0x00, 0x00, 0x00, 0xf7, 0x7b, 0x7f, 0xc0}},
        {"", {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
        {"A", {0x3c, 0x6e, 0x01, 0x00, 0x00, 0x00, 0xe6, 0x80}},
        {"Skywave Link test message\r\nSkywave Link test message\r\nSkywave Link test message\r\n",
         {0x5d, 0x5f, 0x51, 0x00, 0x00, 0x00, 0xef, 0xfd, 0xc1, 0x40, 0xfd, 0xa0, 0x5f,
          0x1d, 0x67, 0x63, 0xeb, 0xfa, 0xc4, 0xbe, 0x00, 0xbf, 0xff, 0xdd, 0x5b, 0xfc,
          0xda, 0xb7, 0xaf, 0x83, 0xf3, 0xb5, 0x99, 0xcb, 0x7c, 0x43, 0x40}},
    };
    return v;
}

std::string repeated(const std::string& s, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

}  // namespace

TEST(lzh_crc16_xmodem_check_value) {
    const std::string check = "123456789";
    ASSERT_EQ(skylink::lzhCrc16(reinterpret_cast<const uint8_t*>(check.data()), check.size()),
              static_cast<uint16_t>(0x31C3));
    PASS();
}

TEST(lzh_known_answers_compress) {
    for (const auto& v : vectors()) {
        const auto out = compress(bytesOf(v.plain));
        ASSERT_TRUE(out == v.compressed);
    }
    PASS();
}

TEST(lzh_known_answers_decompress) {
    for (const auto& v : vectors()) {
        std::vector<uint8_t> out{1, 2, 3};
        ASSERT_TRUE(decompress(v.compressed, &out));
        ASSERT_TRUE(out == bytesOf(v.plain));
    }
    PASS();
}

TEST(lzh_known_answer_repetitive_text) {
    // 600 bytes of a repeated line (exercises long matches).
    const std::vector<uint8_t> expected = {
        0xde, 0x8f, 0x58, 0x02, 0x00, 0x00, 0xe7, 0xf7, 0x63, 0x40, 0x5d, 0x0e, 0x8e,
        0xb3, 0xaf, 0xbd, 0xe6, 0xde, 0xdd, 0xe7, 0x83, 0xae, 0x81, 0xd7, 0x01, 0xd7,
        0x01, 0xd6, 0x83, 0xad, 0x07, 0x5a, 0x0e, 0xb4, 0x1d, 0x50, 0x75, 0xcc, 0x1c};
    const auto plain = bytesOf(repeated("CQ CQ DE K1AAA ", 40));
    ASSERT_TRUE(compress(plain) == expected);
    std::vector<uint8_t> out;
    ASSERT_TRUE(decompress(expected, &out));
    ASSERT_TRUE(out == plain);
    PASS();
}

TEST(lzh_known_answer_binary_5000) {
    // Pseudo-random binary data (exercises adaptive-tree rebuilds); size,
    // header and tail computed with wl2k-go.
    std::vector<uint8_t> plain;
    for (int i = 0; i < 5000; ++i) plain.push_back(static_cast<uint8_t>((i * i * 37 + i * 11 + (i >> 7)) & 0xFF));
    const auto out = compress(plain);
    ASSERT_EQ(out.size(), size_t(5111));
    const std::vector<uint8_t> head = {0x87, 0x21, 0x88, 0x13, 0x00, 0x00, 0xc6, 0x6f,
                                       0x0d, 0xbf, 0x41, 0x0c, 0x00, 0x5e, 0xe2, 0x4d};
    const std::vector<uint8_t> tail = {0xe2, 0x95, 0xad, 0xa3, 0xfc, 0x76, 0x3e, 0xfc};
    ASSERT_TRUE(std::vector<uint8_t>(out.begin(), out.begin() + 16) == head);
    ASSERT_TRUE(std::vector<uint8_t>(out.end() - 8, out.end()) == tail);
    std::vector<uint8_t> back;
    ASSERT_TRUE(decompress(out, &back));
    ASSERT_TRUE(back == plain);
    PASS();
}

TEST(lzh_round_trips) {
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> dist(0, 255);
    std::vector<std::vector<uint8_t>> inputs;
    inputs.push_back({0x42});
    inputs.push_back(std::vector<uint8_t>(1000, 'A'));
    std::vector<uint8_t> all(256);
    std::iota(all.begin(), all.end(), 0);
    inputs.push_back(all);
    inputs.push_back({0, 0, 0, 1, 2, 0, 0, 3, 0, 255, 0, 0});
    std::vector<uint8_t> random(4096);
    for (auto& b : random) b = static_cast<uint8_t>(dist(rng));
    inputs.push_back(random);
    std::vector<uint8_t> big;
    for (int i = 0; i < 200000; ++i) big.push_back(static_cast<uint8_t>((i * 7 + 13) & 0xFF));
    inputs.push_back(big);
    // Exactly the look-ahead size and one more.
    inputs.push_back(std::vector<uint8_t>(60, 'x'));
    inputs.push_back(std::vector<uint8_t>(61, 'y'));

    for (const auto& in : inputs) {
        const auto c = compress(in);
        ASSERT_GE(c.size(), skylink::LZH_HEADER_SIZE);
        std::vector<uint8_t> out;
        ASSERT_TRUE(decompress(c, &out));
        ASSERT_TRUE(out == in);
    }
    PASS();
}

TEST(lzh_empty_input_has_header_only) {
    const auto c = compress({});
    ASSERT_EQ(c.size(), skylink::LZH_HEADER_SIZE);
    std::vector<uint8_t> out{9};
    ASSERT_TRUE(decompress(c, &out));
    ASSERT_TRUE(out.empty());
    PASS();
}

TEST(lzh_short_header_fails) {
    std::vector<uint8_t> out;
    ASSERT_FALSE(decompress({}, &out));
    ASSERT_FALSE(decompress({0x10, 0x00}, &out));
    ASSERT_FALSE(decompress({0x00, 0x00, 0x00, 0x00, 0x00}, &out));
    PASS();
}

TEST(lzh_crc_mismatch_fails) {
    auto c = vectors()[5].compressed;
    c[0] ^= 0x01;
    std::vector<uint8_t> out;
    std::string err;
    ASSERT_FALSE(decompress(c, &out, &err));
    ASSERT_TRUE(out.empty());
    ASSERT_CONTAINS(err, "CRC");

    c = vectors()[5].compressed;
    c[10] ^= 0x40;  // corrupt the bit stream
    ASSERT_FALSE(decompress(c, &out));
    ASSERT_TRUE(out.empty());
    PASS();
}

TEST(lzh_truncated_stream_fails_without_partial_output) {
    const auto full = compress(bytesOf(repeated("Truncation test line. ", 20)));
    for (size_t cut = 1; cut < full.size() - skylink::LZH_HEADER_SIZE; cut += 3) {
        std::vector<uint8_t> c(full.begin(), full.end() - static_cast<std::ptrdiff_t>(cut));
        std::vector<uint8_t> out;
        ASSERT_FALSE(decompress(c, &out));  // CRC no longer matches
        fixCrc(c);
        std::string err;
        ASSERT_FALSE(decompress(c, &out, &err));  // bit stream runs out
        ASSERT_TRUE(out.empty());
    }
    PASS();
}

TEST(lzh_length_header_larger_than_stream_fails) {
    auto c = vectors()[1].compressed;  // "foo"
    c[2] = 4;                          // claims 4 bytes
    fixCrc(c);
    std::vector<uint8_t> out;
    ASSERT_FALSE(decompress(c, &out));
    ASSERT_TRUE(out.empty());
    PASS();
}

TEST(lzh_output_limit_enforced) {
    const auto c = compress(std::vector<uint8_t>(5000, 'z'));
    std::vector<uint8_t> out;
    ASSERT_FALSE(LZHCodec().decompress(c, out, nullptr, 4999));
    ASSERT_TRUE(out.empty());
    ASSERT_TRUE(LZHCodec().decompress(c, out, nullptr, 5000));
    ASSERT_EQ(out.size(), size_t(5000));
    PASS();
}

TEST(lzh_legacy_decompress_returns_empty_on_error) {
    auto c = vectors()[4].compressed;
    c[7] ^= 0xFF;
    ASSERT_TRUE(LZHCodec().decompress(c).empty());
    ASSERT_TRUE(LZHCodec().decompress(vectors()[4].compressed) == bytesOf("A"));
    PASS();
}

TEST(lzh_codec_is_reusable) {
    LZHCodec codec;
    const auto a = codec.compress(bytesOf("first message"));
    const auto b = codec.compress(bytesOf("second, different message"));
    ASSERT_TRUE(a == compress(bytesOf("first message")));
    ASSERT_TRUE(b == compress(bytesOf("second, different message")));
    ASSERT_TRUE(codec.decompress(a) == bytesOf("first message"));
    ASSERT_TRUE(codec.decompress(b) == bytesOf("second, different message"));
    PASS();
}

TEST_MAIN()
