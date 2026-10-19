// LZH compression in the Winlink / FBB "B2" LZHUF format.
//
// Wire format (all integers little-endian), as produced by the classic
// lzhuf.c used by FBB/Winlink and by la5nta/wl2k-go's lzhuf.NewB2Writer:
//
//   [CRC16 : 2 bytes] [uncompressed length : 4 bytes] [LZSS + adaptive
//   Huffman bit stream]
//
// The CRC is CRC-16/XMODEM (poly 0x1021, init 0) over everything after the
// CRC field (the length header and the bit stream). The LZSS ring buffer
// is 2048 bytes with a 60-byte look-ahead (FBB parameters).
//
// Decompression is strict: a short header, a bit stream that runs out
// before the announced length is produced, a CRC mismatch or a length
// above the caller's limit all fail, and no partial output is returned.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace skylink {

// LZSS parameters of the FBB/Winlink LZHUF variant.
constexpr int LZH_N = 2048;          // Size of ring buffer
constexpr int LZH_F = 60;            // Upper limit for match length
constexpr int LZH_THRESHOLD = 2;     // Encode string into position and length if match > THRESHOLD

// Size of the CRC16 + length header in front of the bit stream.
constexpr size_t LZH_HEADER_SIZE = 6;

// Default upper bound for decompressed output (decompression-bomb guard).
constexpr size_t LZH_DEFAULT_MAX_OUTPUT = 16u * 1024u * 1024u;

// CRC-16/XMODEM as used in the B2 LZHUF header.
uint16_t lzhCrc16(const uint8_t* data, size_t len);

class LZHCodec {
public:
    LZHCodec();
    ~LZHCodec();
    LZHCodec(const LZHCodec&) = delete;
    LZHCodec& operator=(const LZHCodec&) = delete;

    // Compress `input` into the B2 LZHUF format. Never fails; an empty
    // input yields the 6-byte header only.
    std::vector<uint8_t> compress(const std::vector<uint8_t>& input);

    // Decompress B2 LZHUF data. Returns false (and leaves `output` empty)
    // on any format, length or CRC error, or when the announced length
    // exceeds `max_output`.
    bool decompress(const std::vector<uint8_t>& input,
                    std::vector<uint8_t>& output,
                    std::string* error = nullptr,
                    size_t max_output = LZH_DEFAULT_MAX_OUTPUT);

    // Convenience form: the decompressed data, or an empty vector on error
    // (an empty vector is also the valid result for an empty original).
    std::vector<uint8_t> decompress(const std::vector<uint8_t>& input);

private:
    struct State;
    std::unique_ptr<State> st_;
};

} // namespace skylink
