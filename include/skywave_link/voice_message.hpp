// Voice message support - Codec2 compressed voice for HF transfer
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace skylink {

// Supported voice codecs
enum class VoiceCodec : uint8_t {
    Codec2_700C = 0,   // ~87.5 bytes/sec, 9.5 min in 50KB
    Codec2_1300 = 1,   // ~162.5 bytes/sec, 5.1 min in 50KB
    Raw_PCM_8k = 255   // Uncompressed 8kHz 16-bit (for testing)
};

// .c2v file header (wire format: 16 bytes, serialized manually)
// Bytes: [magic:4][codec:1][sample_rate:2LE][duration_ms:4LE][compressed_size:4LE][reserved:1]
struct VoiceMessageHeader {
    char magic[4] = {'C', '2', 'V', '1'};
    uint8_t codec = 0;
    uint16_t sample_rate = 8000;
    uint32_t duration_ms = 0;
    uint32_t compressed_size = 0;
};

static constexpr size_t VOICE_HEADER_WIRE_SIZE = 16;

// Serialize header to bytes (little-endian)
std::vector<uint8_t> serializeVoiceHeader(const VoiceMessageHeader& header);

// Deserialize header from bytes (returns false if invalid)
bool deserializeVoiceHeader(const uint8_t* data, size_t size, VoiceMessageHeader& header);

// Check if a file is a .c2v voice message
bool isVoiceMessage(const std::string& filename);
bool isVoiceMessageData(const uint8_t* data, size_t size);

// Get human-readable codec name
const char* voiceCodecName(VoiceCodec codec);

// Calculate approximate duration from compressed size and codec
uint32_t estimateDurationMs(VoiceCodec codec, uint32_t compressed_size);

// Calculate bytes per second for a codec
float codecBytesPerSecond(VoiceCodec codec);

} // namespace skylink
