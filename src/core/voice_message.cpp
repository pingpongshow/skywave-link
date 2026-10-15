// Voice message - header serialization and codec utilities

#include "skywave_link/voice_message.hpp"
#include <algorithm>
#include <cstring>

namespace skylink {

std::vector<uint8_t> serializeVoiceHeader(const VoiceMessageHeader& header) {
    std::vector<uint8_t> out(16);

    // Magic
    out[0] = header.magic[0];
    out[1] = header.magic[1];
    out[2] = header.magic[2];
    out[3] = header.magic[3];

    // Codec
    out[4] = header.codec;

    // Sample rate (little-endian 16-bit)
    out[5] = static_cast<uint8_t>(header.sample_rate & 0xFF);
    out[6] = static_cast<uint8_t>((header.sample_rate >> 8) & 0xFF);

    // Duration ms (little-endian 32-bit)
    out[7] = static_cast<uint8_t>(header.duration_ms & 0xFF);
    out[8] = static_cast<uint8_t>((header.duration_ms >> 8) & 0xFF);
    out[9] = static_cast<uint8_t>((header.duration_ms >> 16) & 0xFF);
    out[10] = static_cast<uint8_t>((header.duration_ms >> 24) & 0xFF);

    // Compressed size (little-endian 32-bit)
    out[11] = static_cast<uint8_t>(header.compressed_size & 0xFF);
    out[12] = static_cast<uint8_t>((header.compressed_size >> 8) & 0xFF);
    out[13] = static_cast<uint8_t>((header.compressed_size >> 16) & 0xFF);
    out[14] = static_cast<uint8_t>((header.compressed_size >> 24) & 0xFF);

    // Reserved
    out[15] = 0;

    return out;
}

bool deserializeVoiceHeader(const uint8_t* data, size_t size, VoiceMessageHeader& header) {
    if (size < 16) return false;

    // Check magic
    if (data[0] != 'C' || data[1] != '2' || data[2] != 'V' || data[3] != '1')
        return false;

    header.magic[0] = data[0];
    header.magic[1] = data[1];
    header.magic[2] = data[2];
    header.magic[3] = data[3];

    header.codec = data[4];

    header.sample_rate = static_cast<uint16_t>(data[5]) |
                         (static_cast<uint16_t>(data[6]) << 8);

    header.duration_ms = static_cast<uint32_t>(data[7]) |
                         (static_cast<uint32_t>(data[8]) << 8) |
                         (static_cast<uint32_t>(data[9]) << 16) |
                         (static_cast<uint32_t>(data[10]) << 24);

    header.compressed_size = static_cast<uint32_t>(data[11]) |
                             (static_cast<uint32_t>(data[12]) << 8) |
                             (static_cast<uint32_t>(data[13]) << 16) |
                             (static_cast<uint32_t>(data[14]) << 24);

    // Validate codec is a known value
    if (header.codec != static_cast<uint8_t>(VoiceCodec::Codec2_700C) &&
        header.codec != static_cast<uint8_t>(VoiceCodec::Codec2_1300) &&
        header.codec != static_cast<uint8_t>(VoiceCodec::Raw_PCM_8k)) {
        return false;
    }

    // Validate sample rate
    if (header.sample_rate == 0) return false;

    return true;
}

bool isVoiceMessage(const std::string& filename) {
    auto dot = filename.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = filename.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext == ".c2v";
}

bool isVoiceMessageData(const uint8_t* data, size_t size) {
    if (size < 16) return false;
    return data[0] == 'C' && data[1] == '2' && data[2] == 'V' && data[3] == '1';
}

const char* voiceCodecName(VoiceCodec codec) {
    switch (codec) {
        case VoiceCodec::Codec2_700C: return "Codec2 700C";
        case VoiceCodec::Codec2_1300: return "Codec2 1300";
        case VoiceCodec::Raw_PCM_8k:  return "Raw PCM 8kHz";
        default: return "Unknown";
    }
}

float codecBytesPerSecond(VoiceCodec codec) {
    switch (codec) {
        case VoiceCodec::Codec2_700C: return 87.5f;     // 700 bps / 8
        case VoiceCodec::Codec2_1300: return 162.5f;     // 1300 bps / 8
        case VoiceCodec::Raw_PCM_8k:  return 16000.0f;   // 8000 Hz * 16-bit
        default: return 0.0f;
    }
}

uint32_t estimateDurationMs(VoiceCodec codec, uint32_t compressed_size) {
    double bps = static_cast<double>(codecBytesPerSecond(codec));
    if (bps <= 0.0) return 0;
    double ms = (static_cast<double>(compressed_size) / bps) * 1000.0;
    if (ms > static_cast<double>(UINT32_MAX)) return UINT32_MAX;
    return static_cast<uint32_t>(ms);
}

} // namespace skylink
