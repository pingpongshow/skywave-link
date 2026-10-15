// LZH Compression round-trip tests
#include "test_macros.hpp"
#include "skywave_link/lzh.hpp"
#include <numeric>
#include <random>
#include <memory>

TEST(lzh_empty_input) {
    auto codec = std::make_unique<skylink::LZHCodec>();
    std::vector<uint8_t> input;
    auto compressed = codec->compress(input);
    ASSERT_TRUE(compressed.empty());
    PASS();
}

TEST(lzh_single_byte) {
    auto codec = std::make_unique<skylink::LZHCodec>();
    std::vector<uint8_t> input = {0x42};
    auto compressed = codec->compress(input);
    ASSERT_FALSE(compressed.empty());
    codec.reset();

    auto dec = std::make_unique<skylink::LZHCodec>();
    auto decompressed = dec->decompress(compressed);
    ASSERT_EQ(decompressed.size(), input.size());
    ASSERT_EQ(decompressed[0], 0x42);
    PASS();
}

TEST(lzh_small_string) {
    auto codec = std::make_unique<skylink::LZHCodec>();
    std::string msg = "Hello, HF Radio!";
    std::vector<uint8_t> input(msg.begin(), msg.end());

    auto compressed = codec->compress(input);
    ASSERT_FALSE(compressed.empty());
    codec.reset();

    auto dec = std::make_unique<skylink::LZHCodec>();
    auto decompressed = dec->decompress(compressed);
    ASSERT_EQ(decompressed.size(), input.size());
    std::string result(decompressed.begin(), decompressed.end());
    ASSERT_EQ(result, msg);
    PASS();
}

TEST(lzh_repetitive_data) {
    auto codec = std::make_unique<skylink::LZHCodec>();
    std::vector<uint8_t> input(1000, 'A');

    auto compressed = codec->compress(input);
    ASSERT_FALSE(compressed.empty());
    // Repetitive data should compress well
    ASSERT_LT(compressed.size(), input.size());
    codec.reset();

    auto dec = std::make_unique<skylink::LZHCodec>();
    auto decompressed = dec->decompress(compressed);
    ASSERT_EQ(decompressed.size(), input.size());
    for (size_t i = 0; i < decompressed.size(); i++) {
        ASSERT_EQ(decompressed[i], (uint8_t)'A');
    }
    PASS();
}

TEST(lzh_all_byte_values) {
    auto codec = std::make_unique<skylink::LZHCodec>();
    std::vector<uint8_t> input(256);
    std::iota(input.begin(), input.end(), 0);

    auto compressed = codec->compress(input);
    ASSERT_FALSE(compressed.empty());
    codec.reset();

    auto dec = std::make_unique<skylink::LZHCodec>();
    auto decompressed = dec->decompress(compressed);
    ASSERT_EQ(decompressed.size(), input.size());
    for (int i = 0; i < 256; i++) {
        ASSERT_EQ(decompressed[i], (uint8_t)i);
    }
    PASS();
}

TEST(lzh_binary_with_nulls) {
    auto codec = std::make_unique<skylink::LZHCodec>();
    std::vector<uint8_t> input = {0, 0, 0, 1, 2, 0, 0, 3, 0, 255, 0, 0};

    auto compressed = codec->compress(input);
    ASSERT_FALSE(compressed.empty());
    codec.reset();

    auto dec = std::make_unique<skylink::LZHCodec>();
    auto decompressed = dec->decompress(compressed);
    ASSERT_EQ(decompressed.size(), input.size());
    for (size_t i = 0; i < input.size(); i++) {
        ASSERT_EQ(decompressed[i], input[i]);
    }
    PASS();
}

TEST(lzh_random_4kb) {
    auto codec = std::make_unique<skylink::LZHCodec>();
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> dist(0, 255);

    std::vector<uint8_t> input(4096);
    for (auto& b : input) b = static_cast<uint8_t>(dist(rng));

    auto compressed = codec->compress(input);
    ASSERT_FALSE(compressed.empty());
    codec.reset();

    auto dec = std::make_unique<skylink::LZHCodec>();
    auto decompressed = dec->decompress(compressed);
    ASSERT_EQ(decompressed.size(), input.size());
    for (size_t i = 0; i < input.size(); i++) {
        ASSERT_EQ(decompressed[i], input[i]);
    }
    PASS();
}

TEST(lzh_large_32kb) {
    auto codec = std::make_unique<skylink::LZHCodec>();
    // Mix of repetitive and random data
    std::vector<uint8_t> input;
    input.reserve(32768);
    for (int i = 0; i < 32768; i++) {
        input.push_back(static_cast<uint8_t>((i * 7 + 13) & 0xFF));
    }

    auto compressed = codec->compress(input);
    ASSERT_FALSE(compressed.empty());
    codec.reset();

    auto dec = std::make_unique<skylink::LZHCodec>();
    auto decompressed = dec->decompress(compressed);
    ASSERT_EQ(decompressed.size(), input.size());
    for (size_t i = 0; i < input.size(); i++) {
        ASSERT_EQ(decompressed[i], input[i]);
    }
    PASS();
}

TEST(lzh_truncated_input) {
    auto codec = std::make_unique<skylink::LZHCodec>();
    // Less than 4 bytes (no valid size header)
    std::vector<uint8_t> truncated = {0x10, 0x00};
    auto result = codec->decompress(truncated);
    ASSERT_TRUE(result.empty());
    PASS();
}

TEST(lzh_zero_size_header) {
    // Valid 4-byte header but size=0
    std::vector<uint8_t> input = {0x00, 0x00, 0x00, 0x00};
    auto codec = std::make_unique<skylink::LZHCodec>();
    auto result = codec->decompress(input);
    ASSERT_TRUE(result.empty());
    PASS();
}

TEST_MAIN()
