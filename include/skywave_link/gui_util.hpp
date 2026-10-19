// Helpers shared by the GUI windows (and meant for App as well): callsign
// and setting normalisation, the position packet builders, link checks and
// formatting. No ImGui dependency, so the unit tests can use them.
#pragma once

#include "aprs_client.hpp"
#include "config.hpp"
#include "freq_manager.hpp"
#include "modem_client.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace skylink::gui {

inline std::string trimCopy(const std::string& input) {
    const auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = input.find_last_not_of(" \t\r\n");
    return input.substr(first, last - first + 1);
}

inline std::string upperCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

// Trimmed and uppercased ("" for blank input).
inline std::string normalizeCallsign(const std::string& input) {
    return upperCopy(trimCopy(input));
}

// "hamlib" | "kenwood_tcp" | "none".
inline std::string normalizeRadioBackend(const std::string& backend) {
    const std::string upper = upperCopy(trimCopy(backend));
    if (upper == "HAMLIB") return "hamlib";
    if (upper == "KENWOOD_TCP" || upper == "KENWOOD TCP") return "kenwood_tcp";
    return "none";
}

// Skywave bandwidth families (WAVEFORM.md); anything else is WIDE.
inline std::string normalizeModemFamily(const std::string& family) {
    const std::string upper = upperCopy(trimCopy(family));
    if (upper == "MID" || upper == "NARROW" || upper == "ULTRA") return upper;
    return "WIDE";
}

// Occupied bandwidth of a family, for the heard lists ("" when unknown).
inline const char* familyBandwidthLabel(const std::string& family) {
    const std::string upper = upperCopy(trimCopy(family));
    if (upper == "WIDE") return "2.4k";
    if (upper == "MID") return "1k";
    if (upper == "NARROW") return "500";
    if (upper == "ULTRA") return "2.7k";
    return "";
}

// voice.mode setting -> VoiceMode (OFF for anything unknown).
inline VoiceMode parseVoiceMode(const std::string& value) {
    const std::string upper = upperCopy(trimCopy(value));
    if (upper == "CONNECTED") return VoiceMode::Connected;
    if (upper == "BROADCAST") return VoiceMode::Broadcast;
    return VoiceMode::Off;
}

// Codec2 rates the modem accepts for VOICECODEC (VOICE.md).
inline const std::vector<std::string>& voiceCodecChoices() {
    static const std::vector<std::string> kChoices = {"AUTO", "700C", "1300", "2400", "3200"};
    return kChoices;
}

inline std::string normalizeVoiceCodec(const std::string& codec) {
    const std::string upper = upperCopy(trimCopy(codec));
    for (const auto& choice : voiceCodecChoices()) {
        if (upper == choice) return choice;
    }
    return "AUTO";
}

// MODE / MAXMODE accept the band's OFDM modes, M4..M13 on HF
// (INTERFACES.md); MODE also takes AUTO.
constexpr int kMinLadderMode = 4;
constexpr int kMaxLadderMode = 13;

inline std::string ladderModeName(int index) {
    return "M" + std::to_string(index);
}

// ---------------------------------------------------------------------------
// Position packets
// ---------------------------------------------------------------------------

inline void writeDoubleLE(std::vector<uint8_t>& out, double value) {
    uint8_t bytes[sizeof(double)];
    std::memcpy(bytes, &value, sizeof(double));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    for (size_t i = 0; i < sizeof(double) / 2; ++i) {
        std::swap(bytes[i], bytes[sizeof(double) - 1 - i]);
    }
#endif
    out.insert(out.end(), bytes, bytes + sizeof(double));
}

// A position worth sending: finite, in range and not the 0,0 placeholder
// that means "no fix / never set".
inline bool isUsablePosition(double lat, double lon) {
    if (!std::isfinite(lat) || !std::isfinite(lon)) return false;
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return false;
    return !(lat == 0.0 && lon == 0.0);
}

// Maidenhead locators are at most 8 characters on the wire.
constexpr size_t kMaxPositionGridLen = 8;

inline void appendGrid(std::vector<uint8_t>& out, const std::string& grid) {
    std::string g = trimCopy(grid);
    if (g.size() > kMaxPositionGridLen) g.resize(kMaxPositionGridLen);
    out.push_back(static_cast<uint8_t>(g.size()));
    out.insert(out.end(), g.begin(), g.end());
}

// Position beacon: 0xF4 | lat(8) | lon(8) | grid_len | grid | callsign[ comment]
inline std::vector<uint8_t> buildPositionBeaconPacket(double lat, double lon,
                                                      const std::string& grid,
                                                      const std::string& callsign,
                                                      const std::string& comment) {
    std::vector<uint8_t> packet;
    packet.push_back(0xF4);
    writeDoubleLE(packet, lat);
    writeDoubleLE(packet, lon);
    appendGrid(packet, grid);
    std::string text = normalizeCallsign(callsign);
    const std::string note = trimCopy(comment);
    if (!note.empty()) {
        text += " ";
        text += note;
    }
    packet.insert(packet.end(), text.begin(), text.end());
    return packet;
}

// Position relay request to the linked station:
// 0xF5 | lat(8) | lon(8) | grid_len | grid | comment
inline std::vector<uint8_t> buildPositionRelayPacket(double lat, double lon,
                                                     const std::string& grid,
                                                     const std::string& comment) {
    std::vector<uint8_t> packet;
    packet.push_back(0xF5);
    writeDoubleLE(packet, lat);
    writeDoubleLE(packet, lon);
    appendGrid(packet, grid);
    const std::string note = trimCopy(comment);
    packet.insert(packet.end(), note.begin(), note.end());
    return packet;
}

// Writes the configuration to its default path; false with `error` set
// when the file cannot be written.
inline bool saveAppConfig(const AppConfig& config, std::string& error) {
    const std::string path = AppConfig::getDefaultConfigPath();
    if (config.save(path)) {
        return true;
    }
    error = "could not write " + path;
    return false;
}

// ---------------------------------------------------------------------------
// Link / modem state
// ---------------------------------------------------------------------------

// True only while the modem reports a live ARQ session. A remembered chat
// partner does not count: it outlives the link.
inline bool isStationLinked(const ModemClient* modem) {
    return modem && modem->isConnected() &&
           modem->getStatus().link_state == LinkState::Connected;
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

// "HH:MM:SS" for a non-negative number of seconds.
inline std::string formatDuration(int64_t total_secs) {
    if (total_secs < 0) total_secs = 0;
    const int64_t hours = total_secs / 3600;
    const int64_t mins = (total_secs % 3600) / 60;
    const int64_t secs = total_secs % 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld", static_cast<long long>(hours),
                  static_cast<long long>(mins), static_cast<long long>(secs));
    return buf;
}

// Whole-dB SNR text; "-" when not measured (NaN/infinite).
inline std::string formatSnr(float snr_db, bool with_unit = false) {
    if (!std::isfinite(snr_db)) return "-";
    char buf[24];
    std::snprintf(buf, sizeof(buf), with_unit ? "%.0f dB" : "%.0f", static_cast<double>(snr_db));
    return buf;
}

// "14.105.000" style dial frequency.
inline std::string formatDialFrequency(uint64_t hz) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu.%03llu.%03llu",
                  static_cast<unsigned long long>(hz / 1000000ULL),
                  static_cast<unsigned long long>((hz / 1000ULL) % 1000ULL),
                  static_cast<unsigned long long>(hz % 1000ULL));
    return buf;
}

// "+3", "-2", "0" (the calling frequency).
inline std::string slotLabel(int slot) {
    return (slot > 0 ? "+" : "") + std::to_string(slot);
}

// Enabled bands from the FrequencyManager (the one band table), low to high.
inline std::vector<BandConfig> enabledBands(const FrequencyManager& freq_mgr) {
    std::vector<BandConfig> bands;
    for (const auto& band : freq_mgr.getBands()) {
        if (band.enabled) bands.push_back(band);
    }
    std::sort(bands.begin(), bands.end(), [](const BandConfig& a, const BandConfig& b) {
        return a.calling_freq_hz < b.calling_freq_hz;
    });
    return bands;
}

// File type label for the Files window ("" extension -> "File").
inline std::string lowerExtension(const std::string& filename) {
    const auto slash = filename.find_last_of("/\\");
    const auto dot = filename.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return {};
    }
    std::string ext = filename.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

inline bool isImageFile(const std::string& filename) {
    const std::string ext = lowerExtension(filename);
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" ||
           ext == ".bmp" || ext == ".webp";
}

inline std::string fileTypeDescription(const std::string& filename) {
    const std::string ext = lowerExtension(filename);
    if (ext == ".png") return "PNG Image";
    if (ext == ".jpg" || ext == ".jpeg") return "JPEG Image";
    if (ext == ".gif") return "GIF Image";
    if (ext == ".bmp") return "BMP Image";
    if (ext == ".webp") return "WebP Image";
    if (ext == ".txt") return "Text File";
    if (ext == ".log") return "Log File";
    if (ext == ".adi" || ext == ".adif") return "ADIF Log";
    if (ext == ".cbr" || ext == ".cabrillo") return "Cabrillo Log";
    if (ext == ".qsl") return "QSL Card";
    if (ext == ".pdf") return "PDF Document";
    if (ext == ".wav") return "WAV Audio";
    if (ext == ".mp3") return "MP3 Audio";
    if (ext == ".zip") return "ZIP Archive";
    if (ext == ".c2v") return "Voice Message";
    return "File";
}

}  // namespace skylink::gui
