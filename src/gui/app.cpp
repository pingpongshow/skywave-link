// Skywave Link Application - Main GUI implementation

#include "skywave_link/app.hpp"
#include "skywave_link/smtp_client.hpp"
#include "skywave_link/pop3_client.hpp"
#include "skywave_link/winlink_telnet_client.hpp"
#include "skywave_link/b2f.hpp"
#include "skywave_link/lzh.hpp"
#include "skywave_link/chat_window.hpp"
#include "skywave_link/file_window.hpp"
#include "skywave_link/message_window.hpp"
#include "skywave_link/position_window.hpp"
#include "skywave_link/settings_window.hpp"
#include "skywave_link/weather_window.hpp"
#include "skywave_link/mail_window.hpp"
#include "skywave_link/bbs_window.hpp"
#include "skywave_link/frequency_window.hpp"
#include "skywave_link/beacon_panel.hpp"
#include "skywave_link/ping_panel.hpp"
#include "skywave_link/main_content.hpp"
#include "skywave_link/station_map_window.hpp"
#include "skywave_link/modern_style.hpp"
#include <iostream>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <limits>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <termios.h>
#endif

#include <SDL.h>
#include <SDL_opengl.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl2.h"

#ifdef SKYWAVE_LINK_HAS_OPENSSL
    #include <openssl/ssl.h>
    #include <openssl/err.h>
    #include <openssl/x509v3.h>
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif
#include <hamlib/riglist.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace skylink {

#ifdef __APPLE__
namespace {

std::string openNativeFileDialog() {
    // Native macOS file chooser via AppleScript.
    const char* cmd =
        "osascript "
        "-e 'try' "
        "-e 'POSIX path of (choose file with prompt \"Select a file\")' "
        "-e 'on error number -128' "
        "-e 'return \"\"' "
        "-e 'end try'";

    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        return "";
    }

    std::string output;
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    (void)pclose(pipe);

    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
}

std::string openNativeFolderDialog() {
    // Native macOS folder chooser via AppleScript.
    const char* cmd =
        "osascript "
        "-e 'try' "
        "-e 'POSIX path of (choose folder with prompt \"Select download folder\")' "
        "-e 'on error number -128' "
        "-e 'return \"\"' "
        "-e 'end try'";

    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        return "";
    }

    std::string output;
    char buffer[512];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    (void)pclose(pipe);

    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
}

}  // namespace
#endif

namespace {

std::string trimSpaces(const std::string& input);
std::string normalizeCallsign(const std::string& input);

VoiceMode configVoiceMode(const std::string& value) {
    const std::string upper = trimSpaces(value);
    std::string norm = upper;
    for (char& ch : norm) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    if (norm == "CONNECTED") return VoiceMode::Connected;
    if (norm == "BROADCAST") return VoiceMode::Broadcast;
    return VoiceMode::Off;
}

bool voiceLocalAudioEnabled(const VoiceConfig& config) {
    return configVoiceMode(config.mode) != VoiceMode::Off;
}

std::string normalizeRadioBackend(const std::string& value) {
    std::string norm = trimSpaces(value);
    for (char& ch : norm) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (norm == "hamlib") {
        return "hamlib";
    }
    if (norm == "kenwood tcp" || norm == "kenwood_tcp") {
        return "kenwood_tcp";
    }
    return "none";
}

// Skywave bandwidth families (WAVEFORM.md): WIDE 2.4 kHz (default),
// MID 1 kHz, NARROW 500 Hz, plus opt-in ULTRA 2.7 kHz.
std::string normalizeModemFamily(const std::string& value) {
    std::string norm = trimSpaces(value);
    for (char& ch : norm) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    if (norm == "MID" || norm == "NARROW" || norm == "ULTRA") {
        return norm;
    }
    return "WIDE";
}

RadioConfig materializeRadioConfigForController(const RadioConfig& source) {
    RadioConfig cfg = source;
    const std::string backend = normalizeRadioBackend(source.backend);

    cfg.backend = backend;
    if (backend == "hamlib") {
        cfg.enabled = true;
        return cfg;
    }

    if (backend == "kenwood_tcp") {
        cfg.enabled = true;
        cfg.hamlib_model = RIG_MODEL_F6K;
        const std::string host = trimSpaces(source.kenwood_host);
        if (host.empty()) {
            cfg.device.clear();
        } else {
            cfg.device = host + ":" + std::to_string(source.kenwood_port);
        }
        cfg.baud = 0;
        return cfg;
    }

    cfg.enabled = false;
    cfg.hamlib_model = 0;
    cfg.device.clear();
    return cfg;
}

const char* liveVoiceCodecDisplayLabel(const std::string& codec) {
    // Codec2 rates offered by Skywave (VOICE.md).
    if (codec == "700C") return "700C";
    if (codec == "1300") return "1300";
    if (codec == "2400") return "2400";
    if (codec == "3200") return "3200";
    return codec.c_str();
}

const char* voiceModeLabel(VoiceMode mode) {
    switch (mode) {
        case VoiceMode::Connected: return "CONN";
        case VoiceMode::Broadcast: return "BCAST";
        case VoiceMode::Off:
        default: return "OFF";
    }
}

const char* voiceStateLabel(VoiceState state) {
    switch (state) {
        case VoiceState::Opening: return "OPENING";
        case VoiceState::Ready: return "READY";
        case VoiceState::TxRequested: return "TX_REQ";
        case VoiceState::TxActive: return "TX";
        case VoiceState::RxActive: return "RX";
        case VoiceState::Closing: return "CLOSING";
        case VoiceState::Idle:
        default: return "IDLE";
    }
}

void renderVoiceActivityIndicator(const VoiceStatus& voice) {
    const ImVec4 base_button = ImGui::GetStyleColorVec4(ImGuiCol_Button);

    const bool tx_active = (voice.state == VoiceState::TxActive);
    const bool rx_active = (voice.state == VoiceState::RxActive);
    const bool flash_on = (static_cast<int>(ImGui::GetTime() * 4.0) % 2) == 0;

    ImVec4 button_color = base_button;
    const char* label = "VOICE";

    if (tx_active) {
        label = "VOICE TX";
        if (flash_on) {
            button_color = ImVec4(0.84f, 0.26f, 0.24f, 1.0f);
        }
    } else if (rx_active) {
        label = "VOICE RX";
        if (flash_on) {
            button_color = ImVec4(0.20f, 0.70f, 0.28f, 1.0f);
        }
    }

    ImGui::PushStyleColor(ImGuiCol_Button, button_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, button_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, button_color);
    ImGui::Button(label, ImVec2(-1, 0));
    ImGui::PopStyleColor(3);
}

SDL_Scancode configuredVoiceHotkeyScancode(const std::string& hotkey) {
    constexpr char kScancodePrefix[] = "SCANCODE:";
    const std::string trimmed = trimSpaces(hotkey);
    if (trimmed.empty()) {
        return SDL_SCANCODE_UNKNOWN;
    }
    if (trimmed.rfind(kScancodePrefix, 0) == 0) {
        const char* value = trimmed.c_str() + (sizeof(kScancodePrefix) - 1);
        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        if (end != value && *end == '\0' &&
            parsed > 0 && parsed < SDL_NUM_SCANCODES) {
            return static_cast<SDL_Scancode>(parsed);
        }
    }
    return SDL_GetScancodeFromName(trimmed.c_str());
}

bool isVoiceHotkeyModifier(SDL_Scancode scancode) {
    switch (scancode) {
        case SDL_SCANCODE_LCTRL:
        case SDL_SCANCODE_RCTRL:
        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT:
        case SDL_SCANCODE_LALT:
        case SDL_SCANCODE_RALT:
        case SDL_SCANCODE_LGUI:
        case SDL_SCANCODE_RGUI:
        case SDL_SCANCODE_MODE:
            return true;
        default:
            return false;
    }
}

bool isCapturableVoiceHotkey(SDL_Scancode scancode) {
    if (scancode == SDL_SCANCODE_UNKNOWN) {
        return false;
    }
    if (isVoiceHotkeyModifier(scancode)) {
        return false;
    }
    const char* name = SDL_GetScancodeName(scancode);
    return name && *name;
}

std::string serializeVoiceHotkey(SDL_Scancode scancode) {
    if (scancode == SDL_SCANCODE_UNKNOWN) {
        return "";
    }
    return "SCANCODE:" + std::to_string(static_cast<int>(scancode));
}

bool voiceSessionReadyForPtt(const VoiceStatus& voice) {
    return voice.state == VoiceState::Ready ||
           voice.state == VoiceState::TxRequested ||
           voice.state == VoiceState::TxActive ||
           voice.state == VoiceState::RxActive;
}

bool voiceStartReady(VoiceMode mode, LinkState link_state) {
    switch (mode) {
        case VoiceMode::Connected:
            return link_state == LinkState::Connected;
        case VoiceMode::Broadcast:
            return link_state == LinkState::Idle;
        case VoiceMode::Off:
        default:
            return false;
    }
}

// Configs written against the ancestor app's modem (ports 8300-8302, in
// either command/data order) are migrated onto the Skywave defaults
// 9100/9101/9102 (INTERFACES.md). Anything else the operator set is left
// alone.
bool normalizeLegacyModemPorts(AppConfig* config) {
    if (!config) {
        return false;
    }

    const bool legacy_ports =
        config->modem.voice_port == 8302 &&
        ((config->modem.command_port == 8301 && config->modem.data_port == 8300) ||
         (config->modem.command_port == 8300 && config->modem.data_port == 8301));
    if (legacy_ports) {
        config->modem.command_port = kDefaultCommandPort;
        config->modem.data_port = kDefaultDataPort;
        config->modem.voice_port = kDefaultVoicePort;
        return true;
    }

    return false;
}

std::string aprsPasscodeForConfig(const AppConfig& config) {
    if (!config.aprs.passcode.empty()) {
        return config.aprs.passcode;
    }
    if (config.station.callsign.empty()) {
        return "-1";
    }
    return std::to_string(APRSClient::calculatePasscode(config.station.callsign));
}

constexpr uint8_t kPositionRelayAckMarker = 0xF3;
constexpr uint8_t kPositionRelayAckStatusRelayed = 0x01;
constexpr uint8_t kPositionRelayAckStatusReceivedOnly = 0x00;
constexpr uint8_t kQsySuggestMarker = 0xF2;
constexpr uint8_t kQsyConfirmMarker = 0xF1;
constexpr uint8_t kQsyConfirmDeclined = 0x00;
constexpr uint8_t kQsyConfirmAccepted = 0x01;
constexpr size_t kPositionRelayAckTextMax = 180;
constexpr char kBeaconEmailGatewayMarker[] = "[EG]";
constexpr char kMailBroadcastTarget[] = "ALL";
constexpr size_t kMailMaxAddressLen = 255;
constexpr uint8_t kMailPrivateReliableMarker = 0xE6;
constexpr uint8_t kMailBroadcastReliableMarker = 0xE7;
constexpr uint8_t kMailAckMarker = 0xE8;
constexpr uint8_t kMailAckStatusAck = 0x01;
constexpr uint8_t kMailAckStatusNack = 0x00;
constexpr uint8_t kMailFlagAckRequired = 0x01;
constexpr uint8_t kMailFlagTemplatePayload = 0x02;
constexpr uint8_t kMailFlagCompressedPayload = 0x04;
constexpr uint8_t kMailFlagWeakSignalProfile = 0x08;
constexpr uint8_t kMailFlagWinlinkEnvelope = 0x10;
constexpr uint8_t kMailFlagPriorityPriority = 0x20;
constexpr uint8_t kMailFlagPriorityEmergency = 0x40;
constexpr uint8_t kMailFlagPriorityMask = kMailFlagPriorityPriority | kMailFlagPriorityEmergency;
constexpr size_t kMailMaxMessageIdLen = 63;
constexpr size_t kMailMaxAckReasonLen = 140;
constexpr uint8_t kBbsAnnounceMarker = 0xD0;
constexpr uint8_t kBbsRemoveMarker = 0xD1;
constexpr uint8_t kBbsFileRequestMarker = 0xD2;
constexpr size_t kBbsMaxFilenameLen = 180;
constexpr size_t kBbsMaxDescriptionLen = 220;

using app_detail::isDisconnectedBroadcastPayload;

std::string resolveStorePath(const std::string& config_path, const std::string& db_path) {
    std::filesystem::path path(db_path);
    if (!path.is_relative()) {
        return path.string();
    }

    std::filesystem::path base_dir = std::filesystem::path(config_path).parent_path();
    if (base_dir.empty()) {
        base_dir = std::filesystem::current_path();
    }
    return (base_dir / path).string();
}

// Endian-safe serialization of IEEE 754 doubles.
// Writes/reads in little-endian byte order regardless of host endianness.
void writeDoubleLE(std::vector<uint8_t>& vec, double value) {
    uint8_t bytes[sizeof(double)];
    std::memcpy(bytes, &value, sizeof(double));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    for (size_t i = 0; i < sizeof(double) / 2; ++i) {
        std::swap(bytes[i], bytes[sizeof(double) - 1 - i]);
    }
#endif
    vec.insert(vec.end(), bytes, bytes + sizeof(double));
}

double readDoubleLE(const uint8_t* data, size_t offset) {
    uint8_t bytes[sizeof(double)];
    std::memcpy(bytes, data + offset, sizeof(double));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    for (size_t i = 0; i < sizeof(double) / 2; ++i) {
        std::swap(bytes[i], bytes[sizeof(double) - 1 - i]);
    }
#endif
    double value;
    std::memcpy(&value, bytes, sizeof(double));
    return value;
}

void writeUint32LE(std::vector<uint8_t>& vec, uint32_t value) {
    vec.push_back(static_cast<uint8_t>(value & 0xFF));
    vec.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    vec.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    vec.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

uint32_t readUint32LE(const uint8_t* data, size_t offset) {
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

std::vector<std::string> enumerateSerialPorts() {
    std::vector<std::string> ports;
#ifdef _WIN32
    for (int i = 1; i <= 256; ++i) {
        std::string name = "COM" + std::to_string(i);
        HANDLE h = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            ports.push_back(name);
            CloseHandle(h);
        }
    }
#elif defined(__APPLE__)
    for (const auto& entry : std::filesystem::directory_iterator("/dev")) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("cu.", 0) == 0 || name.rfind("tty.usb", 0) == 0 ||
            name.rfind("tty.Bluetooth", 0) == 0) {
            ports.push_back(entry.path().string());
        }
    }
#else
    // Linux: /dev/ttyUSB*, /dev/ttyACM*, /dev/ttyS*
    for (const auto& entry : std::filesystem::directory_iterator("/dev")) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("ttyUSB", 0) == 0 || name.rfind("ttyACM", 0) == 0 ||
            name.rfind("ttyS", 0) == 0) {
            ports.push_back(entry.path().string());
        }
    }
#endif
    std::sort(ports.begin(), ports.end());
    return ports;
}

std::string bbsRemoteKey(const std::string& from, const std::string& filename) {
    const std::string owner = normalizeCallsign(from);
    const std::string base = std::filesystem::path(filename).filename().string();
    return owner + "|" + base;
}

bool isLikelyHashId(const std::string& text) {
    return text.size() == 6 &&
           std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::string trimSpaces(const std::string& input) {
    const size_t start = input.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    const size_t end = input.find_last_not_of(" \t\r\n");
    return input.substr(start, end - start + 1);
}

std::string normalizeCallsign(const std::string& input) {
    std::string out = trimSpaces(input);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return out;
}

bool looksLikeEmailAddress(const std::string& input) {
    const std::string addr = trimSpaces(input);
    const size_t at = addr.find('@');
    if (at == std::string::npos || at == 0 || at + 1 >= addr.size()) {
        return false;
    }
    if (addr.find('@', at + 1) != std::string::npos) {
        return false;
    }
    if (addr.find(' ') != std::string::npos || addr.find('\t') != std::string::npos) {
        return false;
    }
    return true;
}

std::string lowerAscii(std::string input) {
    std::transform(input.begin(), input.end(), input.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return input;
}

int localHourFromUnix(int64_t unix_seconds) {
    std::time_t ts = static_cast<std::time_t>(unix_seconds);
    std::tm local_tm = *std::localtime(&ts);
    return std::clamp(local_tm.tm_hour, 0, 23);
}

std::string gatewayBandHourKey(const std::string& callsign,
                               const std::string& band,
                               int hour_local) {
    return normalizeCallsign(callsign) + "|B:" + trimSpaces(band) +
           "|H:" + std::to_string((hour_local + 24) % 24);
}

std::string extractAddressForRelay(const std::string& raw_to) {
    std::string addr = trimSpaces(raw_to);
    const size_t lt = addr.find('<');
    if (lt != std::string::npos) {
        const size_t gt = addr.find('>', lt + 1);
        if (gt != std::string::npos && gt > lt + 1) {
            addr = addr.substr(lt + 1, gt - (lt + 1));
        }
    }
    const size_t comma = addr.find(',');
    if (comma != std::string::npos) {
        addr = addr.substr(0, comma);
    }
    return trimSpaces(addr);
}

bool isLikelyCallsignAddress(const std::string& token) {
    if (token.size() < 3 || token.size() > 15) {
        return false;
    }
    bool has_alpha = false;
    for (unsigned char c : token) {
        if (std::isalpha(c)) {
            has_alpha = true;
            continue;
        }
        if (std::isdigit(c) || c == '/' || c == '-') {
            continue;
        }
        return false;
    }
    return has_alpha;
}

std::string deriveRadioRelayTarget(const std::string& raw_to) {
    const std::string addr = extractAddressForRelay(raw_to);
    if (addr.empty()) {
        return "";
    }

    const size_t at = addr.find('@');
    if (at == std::string::npos) {
        const std::string callsign = normalizeCallsign(addr);
        return isLikelyCallsignAddress(callsign) ? callsign : "";
    }

    const std::string local = normalizeCallsign(addr.substr(0, at));
    const std::string domain = lowerAscii(trimSpaces(addr.substr(at + 1)));
    if (local.empty()) {
        return "";
    }

    if (domain == "radio" || domain == "hf" ||
        domain.find(".radio") != std::string::npos ||
        domain.find(".hf") != std::string::npos ||
        domain == "ria.local" || domain == "mesh.local") {
        return isLikelyCallsignAddress(local) ? local : "";
    }

    return "";
}

bool isHighPriorityGatewayMail(const MailMessage& mail) {
    return mail.retry_count > 0 ||
           !mail.template_name.empty() ||
           mail.priority != MailPriority::Routine;
}

MailPriority clampMailPriority(int value) {
    if (value <= static_cast<int>(MailPriority::Routine)) {
        return MailPriority::Routine;
    }
    if (value >= static_cast<int>(MailPriority::Emergency)) {
        return MailPriority::Emergency;
    }
    return MailPriority::Priority;
}

uint8_t encodeMailPriorityFlag(MailPriority priority) {
    switch (priority) {
        case MailPriority::Priority:
            return kMailFlagPriorityPriority;
        case MailPriority::Emergency:
            return kMailFlagPriorityEmergency;
        case MailPriority::Routine:
        default:
            return 0;
    }
}

MailPriority decodeMailPriorityFlag(uint8_t flags) {
    const uint8_t priority_bits = flags & kMailFlagPriorityMask;
    if ((priority_bits & kMailFlagPriorityEmergency) != 0) {
        return MailPriority::Emergency;
    }
    if ((priority_bits & kMailFlagPriorityPriority) != 0) {
        return MailPriority::Priority;
    }
    return MailPriority::Routine;
}

const char* mailPriorityLabel(MailPriority priority) {
    switch (priority) {
        case MailPriority::Emergency: return "Emergency";
        case MailPriority::Priority: return "Priority";
        case MailPriority::Routine:
        default: return "Routine";
    }
}

std::vector<std::string> splitEscapedFields(const std::string& input) {
    std::vector<std::string> fields;
    std::string cur;
    bool escape = false;
    for (unsigned char c : input) {
        if (escape) {
            if (c == 'n') cur.push_back('\n');
            else if (c == 'r') cur.push_back('\r');
            else cur.push_back(static_cast<char>(c));
            escape = false;
            continue;
        }
        if (c == '\\') {
            escape = true;
            continue;
        }
        if (c == '|') {
            fields.push_back(cur);
            cur.clear();
            continue;
        }
        cur.push_back(static_cast<char>(c));
    }
    fields.push_back(cur);
    return fields;
}

bool decodeTrafficTemplatePayload(const std::string& wire_body,
                                  std::string& template_name,
                                  std::string& pretty_body) {
    if (wire_body.rfind("~TPL|", 0) != 0) {
        return false;
    }
    auto fields = splitEscapedFields(wire_body);
    if (fields.size() < 2) {
        return false;
    }

    if (fields[1] == "I" && fields.size() >= 7) {
        template_name = "ICS-213";
        pretty_body = "ICS-213 Message\n";
        pretty_body += "To: " + fields[2] + "\n";
        pretty_body += "From: " + fields[3] + "\n";
        pretty_body += "Incident: " + fields[4] + "\n";
        pretty_body += "Priority: " + fields[5] + "\n";
        pretty_body += "Message:\n" + fields[6];
        return true;
    }

    if (fields[1] == "R" && fields.size() >= 11) {
        template_name = "Radiogram";
        pretty_body = "Radiogram\n";
        pretty_body += "Precedence: " + fields[2] + "\n";
        pretty_body += "Handling: " + fields[3] + "\n";
        pretty_body += "Station of Origin: " + fields[4] + "\n";
        pretty_body += "Check: " + fields[5] + "\n";
        pretty_body += "Place of Origin: " + fields[6] + "\n";
        pretty_body += "Time Filed: " + fields[7] + "\n";
        pretty_body += "Date Filed: " + fields[8] + "\n";
        pretty_body += "To: " + fields[9] + "\n";
        pretty_body += "Text:\n" + fields[10];
        return true;
    }

    return false;
}

bool isTrafficTemplatePayload(const std::string& body) {
    return body.rfind("~TPL|", 0) == 0;
}

int64_t unixNowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string utcDateTimeStringFromUnix(int64_t unix_seconds) {
    const std::time_t now = static_cast<std::time_t>(unix_seconds);
    std::tm tm_utc{};
#ifdef _WIN32
    gmtime_s(&tm_utc, &now);
#else
    gmtime_r(&now, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%SZ", &tm_utc);
    return std::string(buf);
}

std::string utcDateTimeString() {
    return utcDateTimeStringFromUnix(unixNowSeconds());
}

std::string formatFrequencyLabel(uint64_t freq_hz) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f MHz", static_cast<double>(freq_hz) / 1000000.0);
    return std::string(buffer);
}

float scoreAleObservation(const NoiseFloorReading& reading, bool measurement_valid) {
    if (!measurement_valid) {
        return -1000.0f;
    }

    // Skywave's NOISEFLOOR reports the min-tracked floor plus a busy flag
    // (worker.cpp); there is no per-channel bandwidth or threshold field.
    const float noise_component = std::clamp((-reading.noise_dbfs) - 55.0f, 0.0f, 100.0f);
    const float busy_penalty = reading.busy ? 35.0f : 0.0f;
    return noise_component - busy_penalty;
}

int aleLqaFromScore(float score) {
    const float normalized = std::clamp((score + 5.0f) / 70.0f, 0.0f, 1.0f);
    return static_cast<int>(std::lround(normalized * 100.0f));
}

std::pair<std::string, std::string> splitEmailAddress(const std::string& addr) {
    const std::string trimmed = trimSpaces(addr);
    const size_t at = trimmed.find('@');
    if (at == std::string::npos || at == 0 || at + 1 >= trimmed.size()) {
        return {"", ""};
    }
    return {trimmed.substr(0, at), trimmed.substr(at + 1)};
}

std::vector<uint8_t> encodeWinlinkEnvelopePayload(const MailMessage& mail) {
    B2FHeader header;
    header.message_id = mail.message_id;
    header.type = B2FMessageType::Traffic;
    header.from_callsign = normalizeCallsign(mail.from);
    header.subject = mail.subject;
    header.date_time = utcDateTimeString();

    if (looksLikeEmailAddress(mail.to)) {
        auto [local, domain] = splitEmailAddress(mail.to);
        header.to_callsign = local.empty() ? "EMAIL" : local;
        header.to_address = local.empty() || domain.empty() ? trimSpaces(mail.to) : (local + "@" + domain);
    } else {
        header.to_callsign = normalizeCallsign(mail.to);
    }

    B2FCodec codec;
    return codec.encode(header, mail.body, {});
}

bool decodeWinlinkEnvelopePayload(const std::string& payload,
                                  std::string& from_callsign,
                                  std::string& to_field,
                                  std::string& subject,
                                  std::string& body) {
    B2FCodec codec;
    B2FHeader header;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> attachments;
    std::vector<uint8_t> wire(payload.begin(), payload.end());
    if (!codec.decode(wire, header, body, attachments)) {
        return false;
    }

    if (!header.from_callsign.empty()) {
        from_callsign = normalizeCallsign(header.from_callsign);
    }
    if (!header.to_address.empty()) {
        to_field = trimSpaces(header.to_address);
    } else if (!header.to_callsign.empty()) {
        to_field = normalizeCallsign(header.to_callsign);
    }
    if (!header.subject.empty()) {
        subject = header.subject;
    }
    return true;
}

Message makeStoreMailMessage(const MailMessage& mail, const std::string& my_callsign, bool outgoing) {
    Message msg;
    if (looksLikeEmailAddress(mail.to)) {
        msg.type = MessageType::Email;
    } else {
        msg.type = mail.is_broadcast ? MessageType::Bulletin : MessageType::Private;
    }
    msg.status = mail.status;
    if (msg.status == MessageStatus::Draft) {
        msg.status = outgoing ? MessageStatus::Pending : MessageStatus::Delivered;
    }

    const std::string normalized_my_call = normalizeCallsign(my_callsign);
    const std::string normalized_from = normalizeCallsign(mail.from);
    const std::string normalized_to = normalizeCallsign(mail.to);

    msg.from_callsign = outgoing ? normalized_my_call : normalized_from;
    if (msg.from_callsign.empty()) {
        msg.from_callsign = normalized_from.empty() ? "UNKNOWN" : normalized_from;
    }

    if (mail.is_broadcast) {
        msg.to_callsign = kMailBroadcastTarget;
    } else if (outgoing) {
        msg.to_callsign = normalized_to;
    } else {
        msg.to_callsign = normalized_my_call.empty() ? normalized_to : normalized_my_call;
    }
    if (msg.to_callsign.empty()) {
        msg.to_callsign = mail.is_broadcast ? kMailBroadcastTarget : "UNKNOWN";
    }

    msg.subject = mail.subject;
    msg.body = mail.body;
    msg.message_id = mail.message_id;
    msg.priority = static_cast<int>(mail.priority);
    msg.retry_count = mail.retry_count;
    msg.created_at = mail.timestamp > 0 ? mail.timestamp : unixNowSeconds();
    if (outgoing) {
        if (msg.status == MessageStatus::Sent || msg.status == MessageStatus::Delivered) {
            msg.sent_at = msg.created_at;
        }
    } else {
        if (msg.status == MessageStatus::Delivered) {
            msg.delivered_at = msg.created_at;
        }
    }
    return msg;
}

MailMessage makeUiMailMessage(const Message& msg, bool read) {
    MailMessage mail;
    mail.store_id = msg.id;
    mail.from = msg.from_callsign;
    mail.to = msg.to_callsign;
    mail.subject = msg.subject;
    mail.body = msg.body;
    mail.display_body = msg.body;
    mail.message_id = msg.message_id;
    mail.priority = clampMailPriority(msg.priority);
    mail.timestamp = msg.created_at > 0 ? msg.created_at
                    : (msg.sent_at > 0 ? msg.sent_at : msg.delivered_at);
    if (mail.timestamp <= 0) {
        mail.timestamp = unixNowSeconds();
    }
    mail.read = read;
    mail.status = msg.status;
    mail.retry_count = msg.retry_count;
    mail.is_broadcast = (msg.type == MessageType::Bulletin) ||
                        (normalizeCallsign(msg.to_callsign) == kMailBroadcastTarget);
    decodeTrafficTemplatePayload(mail.body, mail.template_name, mail.display_body);
    return mail;
}

bool isEmailGatewayConfigured(const AppConfig& config) {
    if (!config.email.gateway_enabled) {
        return false;
    }
    const bool smtp_ready = !trimSpaces(config.email.smtp_host).empty() &&
                            !trimSpaces(config.email.smtp_username).empty();
    const bool winlink_telnet_ready =
        config.email.winlink_telnet_enabled &&
        !trimSpaces(config.email.winlink_telnet_host).empty() &&
        !normalizeCallsign(config.email.winlink_telnet_callsign).empty() &&
        !trimSpaces(config.email.winlink_telnet_password).empty();
    return smtp_ready || winlink_telnet_ready;
}

bool senderAllowedForGateway(const AppConfig& config, const std::string& from_callsign) {
    if (config.email.allowed_relays.empty()) {
        return true;
    }
    const std::string from = normalizeCallsign(from_callsign);
    for (const auto& allowed : config.email.allowed_relays) {
        if (normalizeCallsign(allowed) == from) {
            return true;
        }
    }
    return false;
}

bool stripBeaconCapability(std::string& text, const std::string& marker) {
    const size_t pos = text.find(marker);
    if (pos == std::string::npos) {
        return false;
    }
    text.erase(pos, marker.size());
    text = trimSpaces(text);
    return true;
}

bool isLikelyCallsignToken(const std::string& token) {
    if (token.size() < 3 || token.size() > 15) {
        return false;
    }
    bool has_alpha = false;
    for (unsigned char ch : token) {
        if (std::isalpha(ch)) {
            has_alpha = true;
        } else if (!std::isdigit(ch) && ch != '/' && ch != '-') {
            return false;
        }
    }
    return has_alpha;
}

std::pair<std::string, std::string> extractCallsignPrefix(const std::string& text) {
    std::string trimmed = trimSpaces(text);
    if (trimmed.empty()) {
        return {"", ""};
    }

    size_t split = trimmed.find_first_of(" \t\r\n");
    std::string token = (split == std::string::npos) ? trimmed : trimmed.substr(0, split);
    if (!isLikelyCallsignToken(token)) {
        return {"", trimmed};
    }

    std::string rest = (split == std::string::npos) ? "" : trimSpaces(trimmed.substr(split + 1));
    return {normalizeCallsign(token), rest};
}

std::string toUpperAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return text;
}

std::string extractWeatherAlertField(const std::string& report, const std::string& key) {
    if (report.empty() || key.empty()) {
        return "";
    }

    const std::string upper_report = toUpperAscii(report);
    const std::string upper_key = toUpperAscii(key);
    const size_t key_pos = upper_report.find(upper_key);
    if (key_pos == std::string::npos) {
        return "";
    }

    size_t start = key_pos + key.size();
    while (start < report.size()) {
        const unsigned char ch = static_cast<unsigned char>(report[start]);
        if (std::isspace(ch) || report[start] == ':' || report[start] == '-') {
            ++start;
            continue;
        }
        break;
    }
    if (start >= report.size()) {
        return "";
    }

    size_t end = start;
    while (end < report.size()) {
        const char ch = report[end];
        if (ch == '\n' || ch == '\r' || ch == ';') {
            break;
        }
        if (ch == '.' && (end - start) >= 8) {
            break;
        }
        if ((end - start) >= 72) {
            break;
        }
        ++end;
    }

    return trimSpaces(report.substr(start, end - start));
}

std::string inferWeatherAlertSeverity(const std::string& report) {
    std::string severity = extractWeatherAlertField(report, "severity");
    if (!severity.empty()) {
        return toUpperAscii(severity);
    }

    const std::string upper_report = toUpperAscii(report);
    if (upper_report.find("EXTREME") != std::string::npos) return "EXTREME";
    if (upper_report.find("SEVERE") != std::string::npos) return "SEVERE";
    if (upper_report.find("MODERATE") != std::string::npos) return "MODERATE";
    if (upper_report.find("MINOR") != std::string::npos) return "MINOR";
    if (upper_report.find("WARNING") != std::string::npos) return "WARNING";
    if (upper_report.find("WATCH") != std::string::npos) return "WATCH";
    if (upper_report.find("ADVISORY") != std::string::npos) return "ADVISORY";
    return "UNKNOWN";
}

std::string extractWeatherAlertExpiryUtc(const std::string& report) {
    std::string expiry = extractWeatherAlertField(report, "expires");
    if (expiry.empty()) {
        expiry = extractWeatherAlertField(report, "effective until");
    }
    if (expiry.empty()) {
        expiry = extractWeatherAlertField(report, "until");
    }
    if (expiry.empty()) {
        return "N/A";
    }
    if (expiry.size() > 48) {
        expiry.resize(48);
    }
    return expiry;
}

bool buildReliableMailWirePacket(const MailMessage& mail,
                                 const AppConfig& config,
                                 std::vector<uint8_t>* packet_out) {
    if (!packet_out) {
        return false;
    }
    packet_out->clear();

    if (mail.is_broadcast) {
        packet_out->push_back(kMailBroadcastReliableMarker);
    } else {
        packet_out->push_back(kMailPrivateReliableMarker);
    }

    std::string mid = mail.message_id;
    if (mid.size() > kMailMaxMessageIdLen) {
        mid.resize(kMailMaxMessageIdLen);
    }
    if (mid.empty()) {
        return false;
    }
    packet_out->push_back(static_cast<uint8_t>(mid.size()));
    packet_out->insert(packet_out->end(), mid.begin(), mid.end());

    uint8_t flags = 0;
    if (!mail.is_broadcast) {
        flags |= kMailFlagAckRequired;
    }
    if (isTrafficTemplatePayload(mail.body)) {
        flags |= kMailFlagTemplatePayload;
    }
    if (mail.weak_signal_profile) {
        flags |= kMailFlagWeakSignalProfile;
    }
    flags |= encodeMailPriorityFlag(mail.priority);

    std::vector<uint8_t> payload(mail.body.begin(), mail.body.end());
    if (mail.winlink_payload && !mail.is_broadcast) {
        auto wrapped = encodeWinlinkEnvelopePayload(mail);
        if (wrapped.empty()) {
            return false;
        }
        payload = std::move(wrapped);
        flags |= kMailFlagWinlinkEnvelope;
    }

    if (mail.weak_signal_profile &&
        config.email.weak_signal_enable_compression &&
        payload.size() >= 64) {
        LZHCodec codec;
        auto compressed = codec.compress(payload);
        if (!compressed.empty() && compressed.size() + 8 < payload.size()) {
            payload = std::move(compressed);
            flags |= kMailFlagCompressedPayload;
        }
    }

    packet_out->push_back(flags);

    std::string subj = mail.subject.substr(0, 127);
    if (mail.is_broadcast) {
        packet_out->push_back(static_cast<uint8_t>(subj.size()));
        packet_out->insert(packet_out->end(), subj.begin(), subj.end());
        packet_out->insert(packet_out->end(), payload.begin(), payload.end());
        return true;
    }

    const bool to_email = looksLikeEmailAddress(mail.to);
    std::string to_field = mail.to;
    if (!to_email) {
        to_field = normalizeCallsign(to_field);
    }
    if (to_field.empty() || to_field.size() > kMailMaxAddressLen) {
        return false;
    }

    packet_out->push_back(static_cast<uint8_t>(to_field.size()));
    packet_out->insert(packet_out->end(), to_field.begin(), to_field.end());
    packet_out->push_back(static_cast<uint8_t>(subj.size()));
    packet_out->insert(packet_out->end(), subj.begin(), subj.end());
    packet_out->insert(packet_out->end(), payload.begin(), payload.end());
    return true;
}

}

App::App() = default;

App::~App() {
    shutdown();
}

bool App::initialize() {
    // Load configuration, falling back to the legacy Skywave path if needed.
    std::string loaded_config_path = AppConfig::getDefaultConfigPath();
    if (!config_.load(loaded_config_path)) {
        const std::string legacy_config_path = AppConfig::getLegacyConfigPath();
        if (legacy_config_path != loaded_config_path &&
            std::filesystem::exists(legacy_config_path)) {
            if (config_.load(legacy_config_path)) {
                loaded_config_path = legacy_config_path;
            }
        }
    }
    normalizeLegacyModemPorts(&config_);
    config_.voice.codec = "AUTO";
    config_.radio.backend = normalizeRadioBackend(config_.radio.backend);

    // Initialize SDL
    if (!initSDL()) {
        return false;
    }

    // Initialize ImGui
    if (!initImGui()) {
        return false;
    }

    // Create core components
    modem_ = std::make_unique<ModemClient>();
    // User actions and settings must never wait on the modem's reply on the
    // GUI thread; replies are matched as poll() reads them.
    modem_->setNonBlockingCommands(true);
    radio_ = std::make_unique<RadioController>();
    store_ = std::make_unique<MessageStore>();
    freq_mgr_ = std::make_unique<FrequencyManager>();
    aprs_ = std::make_unique<APRSClient>();
    file_transfer_mgr_ = std::make_unique<FileTransferManager>();
    mesh_relay_ = std::make_unique<MeshRelay>();
    aprs_->onPositionReceived([this](const APRSPosition& pos) {
        onAprsPositionReceived(pos);
    });

    file_transfer_mgr_->setDownloadPath(config_.files.download_path);
    file_transfer_mgr_->setAutoAccept(config_.files.auto_accept);
    file_transfer_mgr_->setTrustedCallsigns(config_.files.trusted_callsigns);
    file_transfer_mgr_->onProgress([this](const TransferProgress&) {
        syncTransfersFromManager();
    });

    applyRadioConfig();
    file_transfer_mgr_->onComplete([this](const std::string& transfer_id, bool success, const std::string& path) {
        bool incoming_success = false;
        TransferProgress completed_progress;
        bool found_progress = false;
        if (success) {
            for (const auto& progress : file_transfer_mgr_->getCompletedTransfers()) {
                if (progress.transfer_id == transfer_id) {
                    incoming_success = (progress.direction == TransferDirection::Download);
                    completed_progress = progress;
                    found_progress = true;
                    break;
                }
            }
        }
        std::string completed_name = std::filesystem::path(path).filename().string();
        if (!completed_name.empty()) {
            for (auto it = pending_remote_bbs_downloads_.begin();
                 it != pending_remote_bbs_downloads_.end();) {
                if (it->second.filename == completed_name) {
                    it = pending_remote_bbs_downloads_.erase(it);
                    continue;
                }
                ++it;
            }
            syncPendingBbsDownloadsForUi();
        }
        if (incoming_success) {
            unread_file_notifications_++;
        }

        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = success ? ("File transfer complete: " + path)
                           : ("File transfer failed: " + path);
        msg.is_outgoing = false;
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(msg);
        if (incoming_success) {
            ChatMessage notice;
            notice.from = found_progress && !completed_progress.remote_callsign.empty()
                ? normalizeCallsign(completed_progress.remote_callsign)
                : std::string("FILE");
            notice.to = normalizeCallsign(config_.station.callsign);
            notice.text = "[FILE] Received " +
                          (completed_name.empty() ? std::string(path) : completed_name);
            notice.is_outgoing = false;
            notice.timestamp = msg.timestamp;
            chat_history_.push_back(notice);
        }
        chat_scroll_to_bottom_ = true;
        syncTransfersFromManager();
    });
    file_transfer_mgr_->onFileOffer([this](const std::string& /*transfer_id*/, const std::string& from,
                                           const std::string& filename, size_t size) {
        // Accept by policy: auto-accept, a trusted callsign, or a BBS download
        // this station asked for. Being the linked peer is not consent;
        // anything else waits for the operator (Incoming File prompt).
        bool accept = config_.files.auto_accept;
        const std::string normalized_from = normalizeCallsign(from);
        if (!accept && !normalized_from.empty()) {
            accept = std::any_of(config_.files.trusted_callsigns.begin(),
                                 config_.files.trusted_callsigns.end(),
                                 [&](const std::string& trusted) {
                                     return normalizeCallsign(trusted) == normalized_from;
                                 });
        }
        if (!accept) {
            accept = shouldAcceptRemoteBbsOffer(normalized_from, filename);
        }
        const char* verdict = accept ? " [accepted]" : " [awaiting your decision]";

        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Incoming file offer from " + (normalized_from.empty() ? std::string("UNKNOWN") : normalized_from) +
                   ": " + filename + " (" +
                   std::to_string(size) + " bytes compressed)" + verdict;
        msg.is_outgoing = false;
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(msg);
        ChatMessage visible;
        visible.from = normalized_from.empty() ? std::string("UNKNOWN") : normalized_from;
        visible.to = normalizeCallsign(config_.station.callsign);
        visible.text = "[FILE OFFER] " + filename + " (" + std::to_string(size) +
                       " bytes compressed)" + verdict;
        visible.is_outgoing = false;
        visible.timestamp = msg.timestamp;
        chat_history_.push_back(visible);
        chat_scroll_to_bottom_ = true;
        return accept ? FileOfferDecision::Accept : FileOfferDecision::Defer;
    });

    // Open message store relative to the active config directory when configured path is relative.
    store_->open(resolveStorePath(loaded_config_path, config_.store.db_path));
    loadMailFromStore();
    // Internet-relay mail that was still waiting at the last exit.
    for (auto& msg : store_->loadInternetRelayQueue()) {
        msg.status = MessageStatus::Pending;
        email_outbox_.push_back(std::move(msg));
    }

    applyMeshRelaySettings();

    // Set up modem callbacks
    modem_->onStateChange([this](const std::string& state) {
        onModemStateChange(state);
    });
    modem_->onDataReceived([this](const std::vector<uint8_t>& data) {
        onDataReceived(data);
    });
    modem_->onMonitorFrame([this](const MonitorFrame& frame) {
        onMonitorFrame(frame);
    });
    modem_->onBroadcast([this](const std::string& sender,
                               const std::vector<uint8_t>& payload) {
        onBroadcastMessage(sender, payload);
    });
    modem_->onConnected([this](const std::string& callsign) {
        onConnected(callsign);
    });
    modem_->onDisconnected([this]() {
        onDisconnected();
    });
    modem_->onVoiceStatus([this](const VoiceStatus& status) {
        voice_status_generation_++;
        if (status.mode != VoiceMode::Off && status.state != VoiceState::Idle) {
            voice_start_pending_ = false;
            voice_start_inflight_ = false;
        }
        if (!status.ptt) {
            voice_local_ptt_ = false;
        }
    });

    // Initialize UI state from config
    std::strncpy(callsign_input_, config_.station.callsign.c_str(), sizeof(callsign_input_) - 1);
    std::strncpy(modem_host_input_, config_.modem.host.c_str(), sizeof(modem_host_input_) - 1);
    std::strncpy(voice_capture_device_input_, config_.voice.capture_device.c_str(),
                 sizeof(voice_capture_device_input_) - 1);
    std::strncpy(voice_playback_device_input_, config_.voice.playback_device.c_str(),
                 sizeof(voice_playback_device_input_) - 1);
    modem_port_input_ = config_.modem.command_port;
    voice_port_input_ = config_.modem.voice_port;
    current_band_ = trimSpaces(config_.radio.default_band);
    if (current_band_.empty()) {
        current_band_ = "40m";
    }
    if (freq_mgr_) {
        const uint64_t calling_freq_hz = freq_mgr_->getCallingFrequency(current_band_);
        if (calling_freq_hz > 0) {
            current_frequency_hz_ = static_cast<uint32_t>(calling_freq_hz);
        }
    }
    my_lat_ = config_.station.latitude;
    my_lon_ = config_.station.longitude;
    std::strncpy(position_comment_, config_.aprs.comment.c_str(), sizeof(position_comment_) - 1);
    seedStationLocationsFromHistory();
    last_voice_status_poll_ = last_poll_;

    if (!initVoiceAudio()) {
        std::cerr << "[App] Voice audio initialization failed\n";
    }

    // Auto-connect to modem if configured
    if (config_.modem.auto_connect) {
        connectToModem();
    }

    // Connect to APRS-IS if configured
    if (config_.aprs.enabled) {
        const std::string aprs_passcode = aprsPasscodeForConfig(config_);
        // Connect + login run in the background (pollAPRSConnect()).
        aprs_->beginConnect(config_.aprs.server, config_.aprs.port,
                            config_.station.callsign, aprs_passcode);
        aprs_->setMyPosition(config_.station.latitude, config_.station.longitude,
                             config_.station.altitude_m);
        aprs_->setMyComment(config_.aprs.comment);
        aprs_->setMySymbol(config_.aprs.symbol_table, config_.aprs.symbol);
        aprs_->setBeaconInterval(config_.aprs.beacon_interval_sec);
        aprs_->enableBeaconing(true);
    }

    // Start GPS serial if configured so position reporting can use live coordinates.
    if (config_.aprs.gps_enabled && !config_.aprs.gps_port.empty()) {
        if (!openGPSPort(config_.aprs.gps_port, config_.aprs.gps_baud)) {
            std::cerr << "[GPS] Failed to open configured GPS port: " << config_.aprs.gps_port << "\n";
        }
    }

    if (config_.email.gateway_enabled) {
        startEmailGateway();
    }

    last_poll_ = std::chrono::steady_clock::now();
    last_status_update_ = last_poll_;
    app_start_time_ = last_poll_;

    const char* auto_exit_env = std::getenv("SKYWAVE_LINK_AUTO_EXIT_MS");
    if (auto_exit_env) {
        try {
            int parsed = std::stoi(auto_exit_env);
            if (parsed > 0) {
                auto_exit_after_ms_ = parsed;
            }
        } catch (...) {
            // Ignore invalid values.
        }
    }

    // Initialize shared state and modular windows
    initializeSharedState();
    initializeWindows();

    loadTestScript();
    return true;
}

bool App::initSDL() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) {
        std::cerr << "[App] SDL_Init error: " << SDL_GetError() << "\n";
        return false;
    }

    // Setup window
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);

    // Size: the saved size, or ~80% of the usable display on first run;
    // never larger than the display, never smaller than the layout needs.
    constexpr int kMinWindowWidth = 960;
    constexpr int kMinWindowHeight = 640;
    SDL_Rect usable{0, 0, 1280, 800};
    if (SDL_GetDisplayUsableBounds(0, &usable) != 0) {
        usable = SDL_Rect{0, 0, 1280, 800};
    }
    int win_w = config_.ui.window_width;
    int win_h = config_.ui.window_height;
    if (win_w <= 0 || win_h <= 0) {
        win_w = usable.w * 4 / 5;
        win_h = usable.h * 4 / 5;
    }
    win_w = std::clamp(win_w, std::min(kMinWindowWidth, usable.w), usable.w);
    win_h = std::clamp(win_h, std::min(kMinWindowHeight, usable.h), usable.h);

    window_ = SDL_CreateWindow(
        "Skywave Link - HF Digital Communications",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI
    );

    if (!window_) {
        std::cerr << "[App] SDL_CreateWindow error: " << SDL_GetError() << "\n";
        return false;
    }
    SDL_SetWindowMinimumSize(window_, std::min(kMinWindowWidth, usable.w),
                             std::min(kMinWindowHeight, usable.h));

    gl_context_ = SDL_GL_CreateContext(window_);
    if (!gl_context_) {
        std::cerr << "[App] SDL_GL_CreateContext error: " << SDL_GetError() << "\n";
        return false;
    }

    SDL_GL_MakeCurrent(window_, gl_context_);
    // VSync, except under a test script: with the display asleep or the
    // window occluded the swap blocks, and the update loop (and the
    // script) would stall. run() caps the frame rate with SDL_Delay anyway.
    SDL_GL_SetSwapInterval(std::getenv("SKYLINK_TEST_SCRIPT") ? 0 : 1);

    return true;
}

bool App::openVoiceAudioDevices() {
    closeVoiceAudioDevices();

    if (!voiceLocalAudioEnabled(config_.voice)) {
        voice_audio_ready_ = false;
        return true;
    }

    SDL_AudioSpec desired{};
    SDL_AudioSpec obtained{};
    desired.freq = 8000;
    desired.format = AUDIO_S16LSB;
    desired.channels = 1;
    desired.samples = 160;
    desired.callback = nullptr;

    const char* capture_name =
        config_.voice.capture_device.empty() ? nullptr : config_.voice.capture_device.c_str();
    const char* playback_name =
        config_.voice.playback_device.empty() ? nullptr : config_.voice.playback_device.c_str();

    voice_capture_device_id_ = SDL_OpenAudioDevice(capture_name, SDL_TRUE, &desired, &obtained, 0);
    if (voice_capture_device_id_ == 0) {
        std::cerr << "[App] Failed to open voice capture device: " << SDL_GetError() << "\n";
        closeVoiceAudioDevices();
        return false;
    }

    voice_playback_device_id_ =
        SDL_OpenAudioDevice(playback_name, SDL_FALSE, &desired, &obtained, 0);
    if (voice_playback_device_id_ == 0) {
        std::cerr << "[App] Failed to open voice playback device: " << SDL_GetError() << "\n";
        closeVoiceAudioDevices();
        return false;
    }

    SDL_PauseAudioDevice(static_cast<SDL_AudioDeviceID>(voice_capture_device_id_), 0);
    SDL_PauseAudioDevice(static_cast<SDL_AudioDeviceID>(voice_playback_device_id_), 0);
    voice_audio_ready_ = true;
    return true;
}

void App::closeVoiceAudioDevices() {
    if (voice_capture_device_id_ != 0) {
        SDL_CloseAudioDevice(static_cast<SDL_AudioDeviceID>(voice_capture_device_id_));
        voice_capture_device_id_ = 0;
    }
    if (voice_playback_device_id_ != 0) {
        SDL_ClearQueuedAudio(static_cast<SDL_AudioDeviceID>(voice_playback_device_id_));
        SDL_CloseAudioDevice(static_cast<SDL_AudioDeviceID>(voice_playback_device_id_));
        voice_playback_device_id_ = 0;
    }
    voice_audio_ready_ = false;
    voice_playback_samples_queued_ = 0;
    voice_playback_started_ = false;
    voice_playback_buffer_.clear();
}

bool App::initVoiceAudio() {
    return openVoiceAudioDevices();
}

void App::shutdownVoiceAudio() {
    closeVoiceAudioDevices();
}

void App::applyVoiceSettingsToModem(bool restart_if_open) {
    if (!modem_ || !modem_->isConnected()) {
        return;
    }

    const VoiceStatus current = modem_->getVoiceStatus();
    const VoiceMode desired_mode = configVoiceMode(config_.voice.mode);
    const bool was_open = current.state != VoiceState::Idle && current.mode != VoiceMode::Off;
    const bool mode_changed = current.mode != desired_mode;
    bool changed = false;

    config_.voice.codec = "AUTO";
    if (modem_->setVoiceCodec("AUTO")) {
        changed = true;
    }
    if (mode_changed && modem_->setVoiceMode(desired_mode)) {
        changed = true;
    }

    if (restart_if_open && was_open && mode_changed) {
        voice_start_pending_ = true;
        voice_start_inflight_ = false;
    }
    if (changed) {
        // Answer folded into the voice status by poll(); do not wait for it.
        modem_->sendCommand("VOICESTATUS");
    }
}

void App::applyModemSettingsToModem() {
    if (!modem_ || !modem_->isConnected()) {
        return;
    }

    config_.modem.family = normalizeModemFamily(config_.modem.family);
    // FAMILY is IDLE/LISTENING-only on the modem; a mid-session change is
    // answered with ERROR:STATE and simply does not take effect until the
    // next session, which is the documented behaviour (INTERFACES.md).
    (void)modem_->setFamily(config_.modem.family);
}

void App::pumpVoiceAudio() {
    if (!modem_ || !modem_->isConnected()) {
        return;
    }

    const VoiceStatus voice = modem_->getVoiceStatus();
    const bool local_voice_tx =
        voice_local_ptt_ &&
        voice.ptt &&
        (voice.state == VoiceState::Ready ||
         voice.state == VoiceState::TxRequested ||
         voice.state == VoiceState::TxActive);
    bool queued_local_pcm = false;

    if (local_voice_tx &&
        voice_audio_ready_ &&
        voice_capture_device_id_ != 0) {
        uint8_t capture_buf[4096];
        const Uint32 got = SDL_DequeueAudio(
            static_cast<SDL_AudioDeviceID>(voice_capture_device_id_),
            capture_buf,
            sizeof(capture_buf));
        if (got > 0) {
            const size_t sample_count = static_cast<size_t>(got / sizeof(int16_t));
            std::vector<int16_t> pcm(sample_count);
            const float gain = std::clamp(config_.voice.capture_gain_percent, 0, 400) / 100.0f;
            const int16_t* src = reinterpret_cast<const int16_t*>(capture_buf);
            for (size_t i = 0; i < sample_count; ++i) {
                float scaled = static_cast<float>(src[i]) * gain;
                scaled = std::clamp(scaled, -32768.0f, 32767.0f);
                pcm[i] = static_cast<int16_t>(scaled);
            }
            (void)modem_->sendVoicePcm(pcm.data(), pcm.size());
            queued_local_pcm = true;
        }
    } else if (voice_audio_ready_ && voice_capture_device_id_ != 0) {
        SDL_ClearQueuedAudio(static_cast<SDL_AudioDeviceID>(voice_capture_device_id_));
    }

    if (local_voice_tx &&
        !queued_local_pcm &&
        voice.tx_queued < 320) {
        static const int16_t silence_frame[320] = { 0 };
        (void)modem_->sendVoicePcm(silence_frame, 320);
    }

    if (voice_audio_ready_ && voice_playback_device_id_ != 0) {
        const SDL_AudioDeviceID playback_device =
            static_cast<SDL_AudioDeviceID>(voice_playback_device_id_);
        std::vector<int16_t> rx = modem_->receiveVoicePcm(1600);
        if (!rx.empty()) {
            const float gain = std::clamp(config_.voice.playback_gain_percent, 0, 400) / 100.0f;
            for (int16_t& sample : rx) {
                float scaled = static_cast<float>(sample) * gain;
                scaled = std::clamp(scaled, -32768.0f, 32767.0f);
                sample = static_cast<int16_t>(scaled);
            }
            voice_playback_buffer_.insert(voice_playback_buffer_.end(), rx.begin(), rx.end());
        }

        const bool remote_voice_active =
            voice.state == VoiceState::RxActive ||
            (voice.active && !voice.local) ||
            voice.rx_queued > 0;
        Uint32 queued_bytes = SDL_GetQueuedAudioSize(playback_device);
        voice_playback_samples_queued_ = queued_bytes / sizeof(int16_t);

        if (!voice_playback_started_) {
            const bool ready_to_start =
                voice_playback_buffer_.size() >= VOICE_PLAYBACK_PREBUFFER_SAMPLES ||
                (!remote_voice_active && !voice_playback_buffer_.empty());
            if (ready_to_start) {
                voice_playback_started_ = true;
            }
        }

        if (voice_playback_started_) {
            const size_t queued_samples = queued_bytes / sizeof(int16_t);
            const bool underrun_rebuffer =
                remote_voice_active &&
                queued_samples == 0 &&
                voice_playback_buffer_.size() < VOICE_PLAYBACK_MIN_CHUNK_SAMPLES;
            if (underrun_rebuffer) {
                voice_playback_started_ = false;
            } else {
                const bool should_queue =
                    voice_playback_buffer_.size() >= VOICE_PLAYBACK_MIN_CHUNK_SAMPLES ||
                    (!remote_voice_active && !voice_playback_buffer_.empty()) ||
                    (queued_samples < VOICE_PLAYBACK_REBUFFER_SAMPLES &&
                     voice_playback_buffer_.size() >= (VOICE_PLAYBACK_MIN_CHUNK_SAMPLES / 2));
                if (should_queue) {
                    std::vector<int16_t> chunk(voice_playback_buffer_.begin(),
                                               voice_playback_buffer_.end());
                    SDL_QueueAudio(playback_device,
                                   chunk.data(),
                                   static_cast<Uint32>(chunk.size() * sizeof(int16_t)));
                    voice_playback_buffer_.clear();
                    queued_bytes = SDL_GetQueuedAudioSize(playback_device);
                    voice_playback_samples_queued_ = queued_bytes / sizeof(int16_t);
                }
            }
        }

        if (!remote_voice_active && voice_playback_buffer_.empty() &&
            SDL_GetQueuedAudioSize(playback_device) == 0) {
            voice_playback_started_ = false;
            voice_playback_samples_queued_ = 0;
        }
    }
}

bool App::initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;  // Allow windows outside main viewport

    // Window layout lives next to config.toml, not in whatever directory the
    // app was started from (that is "/" for a Finder launch).
    {
        std::error_code ec;
        const std::filesystem::path config_dir =
            std::filesystem::path(AppConfig::getDefaultConfigPath()).parent_path();
        if (!config_dir.empty()) {
            std::filesystem::create_directories(config_dir, ec);
            imgui_ini_path_ = (config_dir / "imgui.ini").string();
            io.IniFilename = imgui_ini_path_.c_str();
        }
    }

    ui::loadFonts();
    setupStyle();

    ImGui_ImplSDL2_InitForOpenGL(window_, gl_context_);
    ImGui_ImplOpenGL2_Init();
    imgui_initialized_ = true;

    return true;
}

void App::setupStyle() {
    // Windows dragged out of the main window become OS windows; ImGui drops
    // their rounding itself, and the theme's WindowBg is already opaque.
    ui::applyModernStyle(config_.ui.dark_mode, static_cast<float>(config_.ui.font_size));
}

void App::initializeSharedState() {
    shared_state_.config = &config_;
    shared_state_.modem = modem_.get();
    shared_state_.radio = radio_.get();
    shared_state_.store = store_.get();
    shared_state_.freq_mgr = freq_mgr_.get();
    shared_state_.aprs = aprs_.get();
    shared_state_.file_transfer_mgr = file_transfer_mgr_.get();

    shared_state_.current_tab = &current_tab_;
    shared_state_.show_settings = &show_settings_;
    shared_state_.running = &running_;

    shared_state_.chat_history = &chat_history_;
    shared_state_.chat_input = chat_input_;
    shared_state_.chat_input_size = sizeof(chat_input_);
    shared_state_.current_chat_partner = &current_chat_partner_;
    shared_state_.chat_scroll_to_bottom = &chat_scroll_to_bottom_;

    shared_state_.remote_is_typing = &remote_is_typing_;
    shared_state_.last_typing_indicator = &last_typing_indicator_;
    shared_state_.last_typing_sent = &last_typing_sent_;
    shared_state_.last_input_was_empty = &last_input_was_empty_;

    shared_state_.outgoing_fragment_counter = &outgoing_fragment_counter_;

    shared_state_.transfers = &transfers_;
    shared_state_.unread_file_notifications = &unread_file_notifications_;
    shared_state_.file_path_input = file_path_input_;
    shared_state_.file_path_input_size = sizeof(file_path_input_);

    shared_state_.inbox_messages = &inbox_messages_;
    shared_state_.outbox_messages = &outbox_messages_;
    shared_state_.selected_message = &selected_message_;

    shared_state_.my_lat = &my_lat_;
    shared_state_.my_lon = &my_lon_;
    shared_state_.position_comment = position_comment_;
    shared_state_.position_comment_size = sizeof(position_comment_);

    shared_state_.callsign_input = callsign_input_;
    shared_state_.callsign_input_size = sizeof(callsign_input_);
    shared_state_.modem_host_input = modem_host_input_;
    shared_state_.modem_host_input_size = sizeof(modem_host_input_);
    shared_state_.modem_port_input = &modem_port_input_;

    shared_state_.cq_active = &cq_active_;
    shared_state_.cq_repeat_count = &cq_repeat_count_;
    shared_state_.cq_current_repeat = &cq_current_repeat_;
    shared_state_.broadcast_message = broadcast_message_;
    shared_state_.broadcast_message_size = sizeof(broadcast_message_);
    shared_state_.heard_stations = &heard_stations_;

    shared_state_.channel_sniffer_active = &channel_sniffer_active_;
    shared_state_.sniffer_current_slot = &sniffer_current_slot_;
    shared_state_.slot_status = &slot_status_;

    shared_state_.gps_fix_valid = &gps_fix_valid_;
    shared_state_.gps_port = &gps_port_;
    shared_state_.gps_baud = &gps_baud_;

    shared_state_.email_outbox = &email_outbox_;
    shared_state_.radio_outbox = &radio_outbox_;
    shared_state_.email_gateway_active = &email_gateway_active_;
    shared_state_.relay_diagnostics = &relay_diagnostics_;

    shared_state_.bbs_files = &bbs_files_;
    shared_state_.pending_bbs_downloads = &pending_bbs_downloads_ui_;
    shared_state_.selected_bbs_file = &selected_bbs_file_;
    shared_state_.bbs_upload_path = bbs_upload_path_;
    shared_state_.bbs_upload_path_size = sizeof(bbs_upload_path_);
    shared_state_.bbs_file_desc = bbs_file_desc_;
    shared_state_.bbs_file_desc_size = sizeof(bbs_file_desc_);

    shared_state_.current_band = &current_band_;
    shared_state_.current_slot = &current_slot_;
    shared_state_.qsy_suggestion_pending = &qsy_suggestion_pending_;
    shared_state_.qsy_suggestion_from = &qsy_suggestion_from_;
    shared_state_.qsy_suggestion_band = &qsy_suggestion_band_;
    shared_state_.qsy_suggestion_slot = &qsy_suggestion_slot_;
    shared_state_.qsy_suggestion_frequency_hz = &qsy_suggestion_frequency_hz_;

    shared_state_.heard_beacons = &heard_beacons_;
    shared_state_.beacon_enabled = &beacon_enabled_;
    shared_state_.station_locations = &station_locations_;

    shared_state_.qso_active = &qso_active_;
    shared_state_.remote_snr_db = &remote_snr_db_;
    shared_state_.local_snr_db = &local_snr_db_;
    shared_state_.snr_exchanged = &snr_exchanged_;
    shared_state_.qso_start_time = &qso_start_time_;

    shared_state_.away_mode_active = &away_mode_active_;

    shared_state_.ping_results = &ping_results_;
    shared_state_.ping_in_progress = &ping_in_progress_;
    shared_state_.ping_target = &ping_target_;

    shared_state_.mail_inbox = &mail_inbox_;
    shared_state_.mail_outbox = &mail_outbox_;
    shared_state_.mail_to_input = mail_to_input_;
    shared_state_.mail_to_input_size = sizeof(mail_to_input_);
    shared_state_.mail_subject_input = mail_subject_input_;
    shared_state_.mail_subject_input_size = sizeof(mail_subject_input_);
    shared_state_.mail_body_input = mail_body_input_;
    shared_state_.mail_body_input_size = sizeof(mail_body_input_);
    shared_state_.mail_priority_input = &mail_priority_input_;
    shared_state_.selected_mail = &selected_mail_;

    shared_state_.weather_reports = &weather_reports_;
    shared_state_.weather_location_input = weather_location_input_;
    shared_state_.weather_location_input_size = sizeof(weather_location_input_);
    shared_state_.weather_airport_input = weather_airport_input_;
    shared_state_.weather_airport_input_size = sizeof(weather_airport_input_);
    shared_state_.weather_zone_input = weather_zone_input_;
    shared_state_.weather_zone_input_size = sizeof(weather_zone_input_);
    shared_state_.weather_request_pending = &weather_request_pending_;
    shared_state_.weather_request_time = &weather_request_time_;
    shared_state_.weather_fetch_mutex = &weather_fetch_mutex_;
    shared_state_.pending_weather_fetch = &pending_weather_fetch_;

    shared_state_.psk_reporter_queue = &psk_reporter_queue_;
    shared_state_.psk_reporter_mutex = &psk_reporter_mutex_;
    shared_state_.current_frequency_hz = &current_frequency_hz_;
}

void App::initializeWindows() {
    // Create all modular windows
    chat_window_ = std::make_unique<ChatWindow>(shared_state_);
    file_window_ = std::make_unique<FileWindow>(shared_state_);
    message_window_ = std::make_unique<MessageWindow>(shared_state_);
    position_window_ = std::make_unique<PositionWindow>(shared_state_);
    settings_window_ = std::make_unique<SettingsWindow>(shared_state_);
    weather_window_ = std::make_unique<WeatherWindow>(shared_state_);
    mail_window_ = std::make_unique<MailWindow>(shared_state_);
    bbs_window_ = std::make_unique<BBSWindow>(shared_state_);
    frequency_window_ = std::make_unique<FrequencyWindow>(shared_state_);
    beacon_panel_ = std::make_unique<BeaconPanel>(shared_state_);
    ping_panel_ = std::make_unique<PingPanel>(shared_state_);
    main_content_ = std::make_unique<MainContentRenderer>(shared_state_);
    station_map_window_ = std::make_unique<StationMapWindow>(shared_state_);

    // Wire up callbacks for ChatWindow
    chat_window_->onSendMessage = [this](const std::string& text) { sendChatMessage(text); };
    chat_window_->onSendTypingIndicator = [this]() { sendTypingIndicator(); };
    chat_window_->onStartCQ = [this]() { startCQ(); };
    chat_window_->onStopCQ = [this]() { stopCQ(); };
    chat_window_->onSendBroadcast = [this](const std::string& msg) { sendBroadcast(msg); };
    chat_window_->onConnectToStation = [this](const std::string& cs) { connectToStation(cs); };
    chat_window_->onSendPing = [this](const std::string& cs) { sendPing(cs); };

    // Wire up callbacks for FileWindow
    file_window_->onSendFile = [this](const std::string& path) { sendFile(path); };
#ifdef __APPLE__
    file_window_->onOpenFileDialog = [this](PathPickedCallback done) {
        startPathPicker(false, std::move(done));
    };
#endif

    // Wire up callbacks for MessageWindow
    message_window_->onRefreshMessages = [this]() { refreshMessages(); };
    message_window_->onSendEmail = [this](const std::string& to, const std::string& subj,
                                          const std::string& body, const std::string& from) {
        Message email;
        email.type = MessageType::Email;
        email.status = MessageStatus::Pending;
        email.from_callsign = from;
        email.to_callsign = to;
        email.subject = subj;
        email.body = body;
        email.created_at = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (store_) {
            email.id = store_->createMessage(email);
            relayRadioToEmail(email);
        }
    };
    message_window_->onDeleteMessage = [this](int64_t message_store_id) {
        if (message_store_id <= 0) {
            return;
        }
        if (!deleteStoredMessage(message_store_id, "")) {
            return;
        }

        refreshMessages();
        loadMailFromStore();
        if (selected_message_ >= static_cast<int>(inbox_messages_.size())) {
            selected_message_ = static_cast<int>(inbox_messages_.size()) - 1;
        }

        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Deleted message " + std::to_string(message_store_id);
        msg.is_outgoing = false;
        msg.timestamp = unixNowSeconds();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
    };

    // Wire up callbacks for PositionWindow
    position_window_->onUpdatePosition = [this](double lat, double lon) { updatePosition(lat, lon); };

    // Wire up callbacks for SettingsWindow
    settings_window_->onConnectModem = [this]() { connectToModem(); };
    settings_window_->onDisconnectModem = [this]() { disconnectFromModem(); };
    settings_window_->onApplyModemConfig = [this]() { applyModemSettingsToModem(); };
    settings_window_->onSaveConfig = [this]() { config_.save(AppConfig::getDefaultConfigPath()); };
    settings_window_->onOpenGPSPort = [this](const std::string& port, int baud) {
        return openGPSPort(port, baud);
    };
    settings_window_->onCloseGPSPort = [this]() { closeGPSPort(); };
    settings_window_->onStartEmailGateway = [this]() { startEmailGateway(); };
    settings_window_->onStopEmailGateway = [this]() { stopEmailGateway(); };
    settings_window_->onApplyRadioConfig = [this]() { applyRadioConfig(); };
    settings_window_->onApplyVoiceConfig = [this]() {
        closeVoiceAudioDevices();
        (void)openVoiceAudioDevices();
        applyVoiceSettingsToModem(true);
    };
    settings_window_->onVoiceStart = [this]() {
        applyVoiceSettingsToModem(false);
        voice_start_pending_ = true;
        voice_start_inflight_ = false;
    };
    settings_window_->onVoiceStop = [this]() {
        voice_start_pending_ = false;
        voice_start_inflight_ = false;
        voice_ptt_after_start_ = false;
        voice_ui_ptt_active_ = false;
        if (modem_ && modem_->isConnected()) {
            (void)modem_->voiceStop();
        }
        voice_local_ptt_ = false;
        voice_hotkey_ptt_active_ = false;
    };
    settings_window_->onVoicePtt = [this](bool enabled) {
        voice_ui_ptt_active_ = enabled;
        (void)setVoicePttState(voice_ui_ptt_active_ || voice_hotkey_ptt_active_);
    };
    settings_window_->onBeginVoiceHotkeyCapture = [this]() {
        voice_hotkey_capture_active_ = true;
    };
    settings_window_->onCancelVoiceHotkeyCapture = [this]() {
        voice_hotkey_capture_active_ = false;
    };
    settings_window_->onClearVoiceHotkey = [this]() {
        voice_hotkey_capture_active_ = false;
        config_.voice.ptt_hotkey.clear();
    };
    settings_window_->isVoiceHotkeyCaptureActive = [this]() {
        return voice_hotkey_capture_active_;
    };
    settings_window_->onRefreshVoiceAudio = [this]() {
        closeVoiceAudioDevices();
        (void)openVoiceAudioDevices();
    };
#ifdef __APPLE__
    settings_window_->onBrowseDownloadPath = [this](PathPickedCallback done) {
        startPathPicker(true, std::move(done));
    };
#endif
    settings_window_->last_beacon_sent = &last_beacon_sent_;

    // Wire up callbacks for WeatherWindow
    weather_window_->onRequestForecast = [this](const std::string& loc) { requestWeatherForecast(loc); };
    weather_window_->onRequestMETAR = [this](const std::string& code) { requestMETAR(code); };
    weather_window_->onRequestTAF = [this](const std::string& code) { requestTAF(code); };
    weather_window_->onRequestMarine = [this](const std::string& zone) { requestMarineWeather(zone); };
    weather_window_->onCheckStationLink = [this](bool refresh) { return isStationLinkConnected(refresh); };

    // Wire up callbacks for MailWindow
    mail_window_->onSendMail = [this](const std::string& to, const std::string& subj, const std::string& body) {
        sendMail(to, subj, body);
    };
    mail_window_->onSendBroadcastMail = [this](const std::string& subj, const std::string& body) {
        sendBroadcastMail(subj, body);
    };
    mail_window_->onMarkMailRead = [this](const std::string& message_id, bool read) {
        if (store_ && store_->isOpen() && !message_id.empty()) {
            store_->setMailReadState(message_id, read);
        }
    };
    mail_window_->onDeleteMail = [this](int64_t store_id, const std::string& message_id, bool outgoing) {
        bool deleted = deleteStoredMessage(store_id, message_id);

        auto& mailbox = outgoing ? mail_outbox_ : mail_inbox_;
        if (store_id > 0) {
            mailbox.erase(std::remove_if(mailbox.begin(), mailbox.end(),
                                         [&](const MailMessage& mail) {
                                             return mail.store_id == store_id;
                                         }),
                          mailbox.end());
        } else if (!message_id.empty()) {
            mailbox.erase(std::remove_if(mailbox.begin(), mailbox.end(),
                                         [&](const MailMessage& mail) {
                                             return mail.message_id == message_id;
                                         }),
                          mailbox.end());
        }

        refreshMessages();
        if (!outgoing && selected_mail_ >= static_cast<int>(mail_inbox_.size())) {
            selected_mail_ = static_cast<int>(mail_inbox_.size()) - 1;
        }

        if (deleted) {
            ChatMessage msg;
            msg.from = "SYSTEM";
            msg.text = std::string("Deleted ") + (outgoing ? "sent" : "received") + " mail";
            msg.is_outgoing = false;
            msg.timestamp = unixNowSeconds();
            chat_history_.push_back(msg);
            chat_scroll_to_bottom_ = true;
        }
    };

    // Wire up callbacks for BBSWindow
    bbs_window_->onRefreshFiles = [this]() { refreshBBSFiles(); };
    bbs_window_->onUpload = [this](const std::string& path, const std::string& desc) { uploadToBBS(path, desc); };
    bbs_window_->onDownload = [this](const BBSFile& file) { downloadFromBBS(file); };
    bbs_window_->onDelete = [this](const std::string& name) { deleteBBSFile(name); };
#ifdef __APPLE__
    bbs_window_->onOpenFileDialog = [this](PathPickedCallback done) {
        startPathPicker(false, std::move(done));
    };
#endif

    // Wire up callbacks for FrequencyWindow
    frequency_window_->onStartSniffer = [this]() { startChannelSniffer(); };
    frequency_window_->onStopSniffer = [this]() { stopChannelSniffer(); };
    frequency_window_->onSendQsySuggestion = [this](const std::string& band, int slot, uint32_t frequency_hz) {
        sendQsySuggestion(band, slot, frequency_hz);
    };
    frequency_window_->onRespondQsySuggestion = [this](bool accept) {
        respondToQsySuggestion(accept);
    };

    // Wire up callbacks for BeaconPanel and PingPanel
    beacon_panel_->onSendBeacon = [this]() { sendBeacon(); };
    beacon_panel_->onConnectToStation = [this](const std::string& cs) { connectToStation(cs); };
    ping_panel_->onSendPing = [this](const std::string& cs) { sendPing(cs); };

    // Wire up callbacks for MainContentRenderer
    main_content_->onConnectModem = [this]() { connectToModem(); };
    main_content_->onDisconnectModem = [this]() { disconnectFromModem(); };
    main_content_->onConnectToStation = [this](const std::string& cs) { connectToStation(cs); };
    main_content_->onSendPing = [this](const std::string& cs) { sendPing(cs); };
    main_content_->onAbort = [this]() { if (modem_) modem_->abort(); };
    main_content_->onStartCQ = [this]() { startCQ(); };
    main_content_->onStopCQ = [this]() { stopCQ(); };
    main_content_->onSendBeacon = [this]() { sendBeacon(); };
    main_content_->onVoicePtt = [this](bool enabled) {
        voice_ui_ptt_active_ = enabled;
        (void)setVoicePttState(voice_ui_ptt_active_ || voice_hotkey_ptt_active_);
    };
    main_content_->onSendChatMessage = [this](const std::string& text) { sendChatMessage(text); };
    main_content_->onSendTypingIndicator = [this]() { sendTypingIndicator(); };
    main_content_->onSendBroadcast = [this](const std::string& msg) { sendBroadcast(msg); };
    main_content_->onSendData = [this](const std::vector<uint8_t>& data) {
        if (modem_) modem_->sendData(data);
    };
    main_content_->onSetFrequency = [this](uint64_t freq) {
        if (modem_ && modem_->isConnected()) {
            tuneOperatingFrequency(freq);
        }
    };
}

void App::run() {
    constexpr Uint32 kFrameBudgetMs = 16;  // ~60 FPS cap
    while (running_) {
        const Uint32 frame_start = SDL_GetTicks();
        processEvents();
        update();
        render();

        // Cap the frame rate without stacking a fixed sleep on top of the
        // VSync wait (that halved the frame rate and added input lag). Only
        // the unused part of the budget is slept, e.g. when VSync is off or
        // the window is occluded and the swap returns immediately.
        const Uint32 elapsed = SDL_GetTicks() - frame_start;
        if (elapsed < kFrameBudgetMs) {
            SDL_Delay(kFrameBudgetMs - elapsed);
        }
    }
}

void App::shutdown() {
    if (shutdown_done_) {
        return;
    }
    shutdown_done_ = true;

    // Remember the window size (so it reopens the same), then hide the
    // window so quitting feels immediate while the bounded modem teardown
    // and saves finish.
    if (window_) {
        if (!(SDL_GetWindowFlags(window_) &
              (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MINIMIZED))) {
            SDL_GetWindowSize(window_, &config_.ui.window_width, &config_.ui.window_height);
        }
        SDL_HideWindow(window_);
    }

    // A running email gateway job is stopped (no new fetch or delivery)
    // and finished: fetched mail is stored before the server deletes it and
    // delivery results are recorded. The job is bounded by the socket
    // timeouts (5 s connect, 15 s per read/write), so this waits at most for
    // the operation in progress plus the commit. Abandoning it after that
    // is still lossless: an abandoned job never sends POP3 QUIT or a Winlink
    // kill, and unconfirmed outgoing mail stays in the store's relay queue.
    if (email_worker_.busy()) {
        email_send_requested_ = false;
        email_worker_.requestStop();
        constexpr auto kEmailShutdownBound = std::chrono::seconds(45);
        const auto deadline = std::chrono::steady_clock::now() + kEmailShutdownBound;
        while (email_worker_.busy() && std::chrono::steady_clock::now() < deadline) {
            (void)email_worker_.waitFor(100);
            pollEmailGatewayJob();
        }
        if (email_worker_.busy()) {
            email_worker_.abandon();
            std::cerr << "[Email] gateway job did not finish at exit; abandoned "
                         "(server copies and relay queue kept)\n";
        }
    }

    saveMailToStore();

    // Save config
    config_.save(AppConfig::getDefaultConfigPath());

    // Gracefully tear down active radio link before dropping TCP sockets.
    gracefulDisconnectModem();

    // Disconnect from services
    closeGPSPort();
    if (modem_ && modem_->isConnected()) {
        modem_->disconnect();
    }
    if (aprs_ && aprs_->isConnected()) {
        aprs_->disconnect();
    }
    shutdownVoiceAudio();

    // Cleanup ImGui
    if (imgui_initialized_) {
        ImGui_ImplOpenGL2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        imgui_initialized_ = false;
    }

    // Cleanup SDL
    if (gl_context_) {
        SDL_GL_DeleteContext(gl_context_);
        gl_context_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    SDL_Quit();

}

void App::gracefulDisconnectModem() {
    if (!modem_ || !modem_->isConnected()) {
        return;
    }

    // The cached link state is at most one STATUS poll (500 ms) old and
    // link changes also arrive as events, so no refresh round-trip.
    const auto link_state = modem_->getStatus().link_state;
    if (link_state != LinkState::Connected && link_state != LinkState::Connecting &&
        link_state != LinkState::Disconnecting) {
        return;
    }

    // Quitting must stay prompt: the whole teardown is bounded to ~0.7 s.
    // Once the modem has started its disconnect it completes the handshake
    // on its own after our sockets close; only a modem that has not reacted
    // at all gets an ABORT.
    auto wait_for = [this](int ms, auto done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < deadline) {
            modem_->poll();
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };
    auto is_idle = [this]() { return modem_->getStatus().link_state == LinkState::Idle; };

    if (link_state != LinkState::Disconnecting) {
        modem_->sendCommand("DISCONNECT");
    }
    if (wait_for(600, is_idle)) {
        return;
    }
    if (modem_->getStatus().link_state != LinkState::Disconnecting) {
        // Fallback in case remote or PHY state machine is stuck.
        modem_->sendCommand("ABORT");
        (void)wait_for(100, is_idle);
    }
}

void App::processEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);

        if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) {
            if (handleVoiceHotkeyEvent(event.key, event.type == SDL_KEYDOWN)) {
                continue;
            }
        }

        if (event.type == SDL_QUIT) {
            running_ = false;
        }
        if (event.type == SDL_WINDOWEVENT &&
            event.window.event == SDL_WINDOWEVENT_CLOSE &&
            event.window.windowID == SDL_GetWindowID(window_)) {
            running_ = false;
        }
        if (event.type == SDL_WINDOWEVENT &&
            event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            voice_hotkey_capture_active_ = false;
            if (voice_hotkey_ptt_active_ || voice_ui_ptt_active_) {
                voice_hotkey_ptt_active_ = false;
                voice_ui_ptt_active_ = false;
                (void)setVoicePttState(false);
            }
        }
    }
}

bool App::setVoicePttState(bool enabled) {
    if (!modem_ || !modem_->isConnected()) {
        if (!enabled) {
            voice_local_ptt_ = false;
            voice_ptt_after_start_ = false;
            voice_ui_ptt_active_ = false;
            voice_hotkey_ptt_active_ = false;
        }
        return false;
    }

    const VoiceStatus voice = modem_->getVoiceStatus();
    const VoiceMode desired_mode = configVoiceMode(config_.voice.mode);
    const LinkState link_state = modem_->getStatus().link_state;

    if (!enabled) {
        voice_ptt_after_start_ = false;
        if (!voiceSessionReadyForPtt(voice) && !voice.ptt) {
            voice_local_ptt_ = false;
            return true;
        }
        if (!modem_->setVoicePtt(false)) {
            return false;
        }
        voice_local_ptt_ = false;
        return true;
    }

    if (desired_mode == VoiceMode::Off) {
        return false;
    }

    if (voice_start_pending_ && voice.state == VoiceState::Idle) {
        voice_ptt_after_start_ = true;
        voice_local_ptt_ = true;
        return true;
    }

    if (voiceSessionReadyForPtt(voice)) {
        if (voice.ptt) {
            voice_local_ptt_ = true;
            voice_ptt_after_start_ = false;
            return true;
        }
        if (!modem_->setVoicePtt(true)) {
            return false;
        }
        voice_local_ptt_ = true;
        voice_ptt_after_start_ = false;
        return true;
    }

    if (voice.state == VoiceState::Opening) {
        voice_ptt_after_start_ = true;
        return true;
    }

    if (!voiceStartReady(desired_mode, link_state)) {
        return false;
    }

    applyVoiceSettingsToModem(false);
    if (!modem_->voiceStart()) {
        return false;
    }

    voice_start_pending_ = true;
    voice_start_inflight_ = true;
    voice_start_attempted_at_ = std::chrono::steady_clock::now();
    voice_ptt_after_start_ = true;
    voice_local_ptt_ = true;
    return true;
}

bool App::handleVoiceHotkeyEvent(const SDL_KeyboardEvent& key, bool pressed) {
    if (key.repeat != 0) {
        return false;
    }

    const SDL_Scancode scancode = key.keysym.scancode;

    if (voice_hotkey_capture_active_) {
        if (!pressed) {
            return true;
        }
        if (scancode == SDL_SCANCODE_ESCAPE) {
            voice_hotkey_capture_active_ = false;
            return true;
        }
        if (scancode == SDL_SCANCODE_BACKSPACE || scancode == SDL_SCANCODE_DELETE) {
            config_.voice.ptt_hotkey.clear();
            voice_hotkey_capture_active_ = false;
            return true;
        }
        if (isCapturableVoiceHotkey(scancode)) {
            config_.voice.ptt_hotkey = serializeVoiceHotkey(scancode);
            voice_hotkey_capture_active_ = false;
        }
        return true;
    }

    const SDL_Scancode configured = configuredVoiceHotkeyScancode(config_.voice.ptt_hotkey);
    if (configured == SDL_SCANCODE_UNKNOWN || scancode != configured) {
        return false;
    }

    if (pressed) {
        voice_hotkey_ptt_active_ = true;
        (void)setVoicePttState(voice_ui_ptt_active_ || voice_hotkey_ptt_active_);
        return true;
    }

    if (voice_hotkey_ptt_active_) {
        voice_hotkey_ptt_active_ = false;
        (void)setVoicePttState(voice_ui_ptt_active_ || voice_hotkey_ptt_active_);
        return true;
    }

    return false;
}

void App::update() {
    auto now = std::chrono::steady_clock::now();
    runTestScript();
    pollModemConnect();
    pollAPRSConnect();
    pollPathPicker();

    // Poll modem and APRS at regular intervals
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_poll_);
    if (elapsed.count() >= 20) {  // 50 Hz polling
        if (modem_) modem_->poll();
        if (aprs_) aprs_->poll();
        last_poll_ = now;
    }

    // Keep link-state fresh for UI/actions that depend on station connection.
    if (modem_ && modem_->isConnected()) {
        auto state_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_status_update_);
        if (state_elapsed.count() >= 500) {
            // STATUS carries state, mode, family, SNR, busy and TX backlog in
            // one exchange. EVT:SNR arrives on change as well, so this poll is
            // a floor rather than the only source.
            modem_->sendCommand("STATUS");
            last_status_update_ = now;
        }

        // Keep the modem's channel-memory buckets keyed to the rig's real
        // dial frequency (INTERFACES.md "Companion App Split"): the modem
        // cannot know it, and without it every band shares one memory of
        // each peer.
        if (current_frequency_hz_ != last_reported_dial_hz_) {
            if (modem_->reportDialFrequency(
                    static_cast<double>(current_frequency_hz_))) {
                last_reported_dial_hz_ = current_frequency_hz_;
            }
        }
        auto voice_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_voice_status_poll_);
        if (voice_elapsed.count() >= 250) {
            // Fire-and-forget: the VOICESTATUS reply is folded into the
            // status snapshot by poll(). queryVoiceStatus() would block the
            // GUI thread (>= 10 ms, up to 500 ms) four times a second.
            modem_->sendCommand("VOICESTATUS");
            last_voice_status_poll_ = now;
        }

        const VoiceStatus voice = modem_->getVoiceStatus();
        const VoiceMode desired_mode = configVoiceMode(config_.voice.mode);
        const LinkState link_state = modem_->getStatus().link_state;
        const bool voice_open =
            desired_mode != VoiceMode::Off &&
            voice.mode == desired_mode &&
            voice.state != VoiceState::Idle;
        const bool auto_requested =
            config_.voice.auto_start &&
            desired_mode != VoiceMode::Off &&
            !voice_auto_started_;
        const bool start_requested = voice_start_pending_ || auto_requested;
        if (voice_start_inflight_ && voice.state == VoiceState::Idle) {
            const auto attempt_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - voice_start_attempted_at_);
            if (attempt_elapsed.count() >= 750) {
                voice_start_inflight_ = false;
            }
        }

        if (voice_open) {
            voice_start_pending_ = false;
            voice_start_inflight_ = false;
            if (config_.voice.auto_start) {
                voice_auto_started_ = true;
            }
        } else if (start_requested &&
                   voiceStartReady(desired_mode, link_state) &&
                   !voice_start_inflight_) {
            applyVoiceSettingsToModem(false);
            if (modem_->voiceStart()) {
                voice_start_pending_ = true;
                voice_start_inflight_ = true;
                voice_start_attempted_at_ = now;
                if (config_.voice.auto_start) {
                    voice_auto_started_ = true;
                }
            }
        }
        if (voice_ptt_after_start_ && voiceSessionReadyForPtt(voice) && !voice.ptt) {
            if (setVoicePttState(true)) {
                voice_ptt_after_start_ = false;
            }
        }
        if (!config_.voice.auto_start || desired_mode == VoiceMode::Off) {
            voice_auto_started_ = false;
        }
        if (desired_mode == VoiceMode::Off) {
            voice_start_pending_ = false;
            voice_start_inflight_ = false;
            voice_ptt_after_start_ = false;
        }
    } else {
        last_status_update_ = now;
        last_voice_status_poll_ = now;
        voice_auto_started_ = false;
        voice_start_pending_ = false;
        voice_start_inflight_ = false;
        voice_local_ptt_ = false;
        voice_ptt_after_start_ = false;
        voice_ui_ptt_active_ = false;
        voice_hotkey_capture_active_ = false;
        voice_hotkey_ptt_active_ = false;
    }

    applyMeshRelaySettings();
    flushMeshRelayQueue();
    updateQsyWorkflow();

    // Send pending messages if connected
    if (modem_ && modem_->getStatus().link_state == LinkState::Connected) {
        sendPendingMessages();
        std::string linked_peer = normalizeCallsign(modem_->getStatus().remote_callsign);
        if (linked_peer.empty()) {
            linked_peer = normalizeCallsign(current_chat_partner_);
        }
        if (!linked_peer.empty()) {
            // Keep BBS download requests moving even if the onConnected callback was missed.
            flushPendingRemoteBbsDownloadsForPeer(linked_peer, false);
        }
    }

    // Flush queued email relay traffic when a suitable gateway is available.
    processQueuedGatewayMail();
    pumpVoiceAudio();

    // Mail ACK/NACK timeout and retry handling.
    updateReliableMail();

    // Offers waiting for the operator belong to the radio session.
    if (file_transfer_mgr_ && !isLinkConnected()) {
        dropPendingFileOffers();
    }

    // File transfer state machine (offer/ack/chunk/retry).
    if (file_transfer_mgr_ && modem_ && modem_->isConnected()) {
        file_transfer_mgr_->setDownloadPath(config_.files.download_path);
        file_transfer_mgr_->setAutoAccept(config_.files.auto_accept);
        file_transfer_mgr_->setTrustedCallsigns(config_.files.trusted_callsigns);
        file_transfer_mgr_->update(*modem_);
    }

    if (current_tab_ == AppTab::Files) {
        unread_file_notifications_ = 0;
    }

    // Update CQ if active
    updateCQ();

    // Update beacon if enabled
    updateBeacon();

    // Update ping timeout
    updatePing();

    // Keep SNR telemetry fresh for adaptive link decisions (currently disabled).
    if (SNR_KEEPALIVE_ENABLED) {
        updateSNRKeepAlive();
    }

    // Update channel sniffer if active
    updateChannelSniffer();

    // Update GPS if port is open
    updateGPS();

    // Update email gateway if active
    pollEmailGatewayJob();
    updateEmailGateway();

    // Update PSK Reporter uploads
    updatePSKReporter();

    // Process completed async weather fetches
    processWeatherFetchResult();

    // Check weather request timeout
    if (weather_request_pending_) {
        auto weather_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - weather_request_time_);
        if (weather_elapsed.count() > WEATHER_TIMEOUT_MS) {
            weather_request_pending_ = false;

            ChatMessage msg;
            msg.from = "SYSTEM";
            msg.text = "Weather request timed out";
            msg.is_outgoing = false;
            msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            chat_history_.push_back(msg);
            chat_scroll_to_bottom_ = true;
        }
    }

    pruneFragmentBuffers();

    // Bound in-memory chat history; the oldest lines go first.
    if (chat_history_.size() > MAX_CHAT_HISTORY) {
        chat_history_.erase(chat_history_.begin(),
                            chat_history_.begin() +
                                static_cast<std::ptrdiff_t>(chat_history_.size() - MAX_CHAT_HISTORY));
    }

    if (auto_exit_after_ms_ > 0) {
        auto alive_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - app_start_time_);
        if (alive_ms.count() >= auto_exit_after_ms_) {
            running_ = false;
        }
    }
}

namespace {

// First-use placement for the detachable tool windows: sizes scale with the
// font (they were tuned for a 13 px font), are clamped to the main window,
// and each window gets its own small offset from the centre so windows opened
// together do not land exactly on top of each other. Later runs restore the
// user's own placement from imgui.ini.
void prepareToolWindow(float width, float height, int slot) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float scale = ImGui::GetFontSize() / 13.0f;
    const ImVec2 size(std::min(width * scale, vp->WorkSize.x * 0.9f),
                      std::min(height * scale, vp->WorkSize.y * 0.9f));
    const float offset = static_cast<float>(slot % 8 - 4) * 22.0f;
    const ImVec2 centre(vp->WorkPos.x + vp->WorkSize.x * 0.5f + offset,
                        vp->WorkPos.y + vp->WorkSize.y * 0.5f + offset);
    ImGui::SetNextWindowSize(size, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(centre, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
}

} // namespace

void App::render() {
    // Settings -> UI -> Font Size applies live.
    ImGuiStyle& frame_style = ImGui::GetStyle();
    const float wanted_font_size = static_cast<float>(std::clamp(config_.ui.font_size, 10, 24));
    if (frame_style.FontSizeBase != wanted_font_size) {
        frame_style._NextFrameFontSizeBase = wanted_font_size;
    }

    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // Main window fills viewport
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_MenuBar;

    // The main window sits a shade darker than floating tool windows so
    // those read as raised panels.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::palette().app_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("MainWindow", nullptr, window_flags);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    renderMenuBar();
    if (main_content_) main_content_->render();
    renderStatusBar();

    ImGui::End();

    // ========================================================================
    // DETACHABLE WINDOWS (delegated to modular window classes)
    // ========================================================================

    // Settings window
    if (show_settings_) {
        prepareToolWindow(620, 520, 0);
        if (ImGui::Begin("Settings", &show_settings_)) {
            if (settings_window_) settings_window_->render();
        }
        ImGui::End();
    }

    // Mail window
    static bool show_mail = false;
    if (current_tab_ == AppTab::Mail) show_mail = true;
    if (show_mail) {
        prepareToolWindow(600, 500, 1);
        if (ImGui::Begin("Mail", &show_mail)) {
            if (mail_window_) mail_window_->render();
        }
        ImGui::End();
        if (!show_mail) current_tab_ = AppTab::Chat;
    }

    // File transfer window
    static bool show_files = false;
    if (current_tab_ == AppTab::Files) show_files = true;
    if (show_files) {
        prepareToolWindow(600, 420, 2);
        if (ImGui::Begin("File Transfer", &show_files)) {
            if (file_window_) file_window_->render();
        }
        ImGui::End();
        if (!show_files) current_tab_ = AppTab::Chat;
    }

    renderPendingFileOffers();

    // Weather window
    static bool show_weather = false;
    if (current_tab_ == AppTab::Weather) show_weather = true;
    if (show_weather) {
        prepareToolWindow(500, 400, 3);
        if (ImGui::Begin("Weather", &show_weather)) {
            if (weather_window_) weather_window_->render();
        }
        ImGui::End();
        if (!show_weather) current_tab_ = AppTab::Chat;
    }

    // Position window
    static bool show_position = false;
    if (current_tab_ == AppTab::Position) show_position = true;
    if (show_position) {
        prepareToolWindow(450, 350, 4);
        if (ImGui::Begin("Position", &show_position)) {
            if (position_window_) position_window_->render();
        }
        ImGui::End();
        if (!show_position) current_tab_ = AppTab::Chat;
    }

    // Station map window
    static bool show_station_map = false;
    if (current_tab_ == AppTab::StationMap) show_station_map = true;
    if (show_station_map) {
        prepareToolWindow(860, 620, 5);
        if (ImGui::Begin("Station Map", &show_station_map)) {
            if (station_map_window_) station_map_window_->render();
        }
        ImGui::End();
        if (!show_station_map) current_tab_ = AppTab::Chat;
    }

    // Messages window
    static bool show_messages = false;
    if (current_tab_ == AppTab::Messages) show_messages = true;
    if (show_messages) {
        prepareToolWindow(600, 450, 6);
        if (ImGui::Begin("Messages (Store & Forward)", &show_messages)) {
            if (message_window_) message_window_->render();
        }
        ImGui::End();
        if (!show_messages) current_tab_ = AppTab::Chat;
    }

    // BBS window
    static bool show_bbs = false;
    if (current_tab_ == AppTab::BBS) show_bbs = true;
    if (show_bbs) {
        prepareToolWindow(650, 500, 7);
        if (ImGui::Begin("BBS File Sharing", &show_bbs)) {
            if (bbs_window_) bbs_window_->render();
        }
        ImGui::End();
        if (!show_bbs) current_tab_ = AppTab::Chat;
    }

    // Frequency window
    static bool show_frequency = false;
    if (current_tab_ == AppTab::Frequency) show_frequency = true;
    if (show_frequency) {
        prepareToolWindow(500, 400, 8);
        if (ImGui::Begin("Frequency Management", &show_frequency)) {
            if (frequency_window_) frequency_window_->render();
        }
        ImGui::End();
        if (!show_frequency) current_tab_ = AppTab::Chat;
    }

    // Rendering
    ImGui::Render();
    glViewport(0, 0, static_cast<int>(ImGui::GetIO().DisplaySize.x),
               static_cast<int>(ImGui::GetIO().DisplaySize.y));
    const ImVec4 clear_color = ui::palette().app_bg;
    glClearColor(clear_color.x, clear_color.y, clear_color.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());

    // Update and render additional platform windows (for viewports)
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        SDL_Window* backup_current_window = SDL_GL_GetCurrentWindow();
        SDL_GLContext backup_current_context = SDL_GL_GetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        SDL_GL_MakeCurrent(backup_current_window, backup_current_context);
    }

    if (!test_shot_path_.empty()) {
        // Test hook: read back the frame just rendered (works with the
        // display asleep, unlike a screen capture) and write a binary PPM.
        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(window_, &w, &h);
        std::vector<unsigned char> px(size_t(w) * size_t(h) * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
        std::ofstream f(test_shot_path_, std::ios::binary);
        f << "P6\n" << w << " " << h << "\n255\n";
        for (int y = h - 1; y >= 0; --y) {
            f.write(reinterpret_cast<const char*>(px.data() + size_t(y) * size_t(w) * 3),
                    std::streamsize(w) * 3);
        }
        testLog("screenshot written: " + test_shot_path_);
        test_shot_path_.clear();
    }
    SDL_GL_SwapWindow(window_);
}

void App::renderMenuBar() {
    static bool open_about_dialog = false;

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Settings", "Ctrl+,")) {
                show_settings_ = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Ctrl+Q")) {
                running_ = false;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Connection")) {
            bool connected = modem_ && (modem_->isConnected() || modem_->isConnecting());
            if (ImGui::MenuItem("Connect to Modem", nullptr, false, !connected)) {
                connectToModem();
            }
            if (ImGui::MenuItem("Disconnect from Modem", nullptr, false, connected)) {
                disconnectFromModem();
            }
            ImGui::Separator();

            bool station_connected = modem_ && modem_->getStatus().link_state == LinkState::Connected;
            if (ImGui::MenuItem("Disconnect Station", nullptr, false, station_connected)) {
                if (modem_) modem_->disconnectStation();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Frequency")) {
                current_tab_ = AppTab::Frequency;
            }
            if (ImGui::MenuItem("Messages")) {
                current_tab_ = AppTab::Messages;
            }
            if (ImGui::MenuItem("Mail")) {
                current_tab_ = AppTab::Mail;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About Skywave Link")) {
                open_about_dialog = true;
            }
            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
    }

    // About dialog (must be outside menu bar)
    if (open_about_dialog) {
        ImGui::OpenPopup("AboutDialog");
        open_about_dialog = false;
    }
    if (ImGui::BeginPopupModal("AboutDialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Skywave Link");
            ImGui::Separator();
            ImGui::Text("HF Digital Communications Application");
            ImGui::Text("Version 1.0.0");
            ImGui::Spacing();
            ImGui::Text("A peer-to-peer digital messaging system for");
            ImGui::Text("amateur radio operators using the Skywave modem.");
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Text("Features:");
            ImGui::BulletText("Real-time text chat");
            ImGui::BulletText("File & image transfer");
            ImGui::BulletText("Mail (offline messages)");
            ImGui::BulletText("Broadcast messaging");
            ImGui::BulletText("BBS file sharing");
            ImGui::BulletText("APRS position reporting");
            ImGui::BulletText("Email gateway");
            ImGui::Spacing();

        if (ImGui::Button("Close", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void App::renderStatusBar() {
    // Status bar as child window at bottom (MainContentRenderer reserves
    // ui::statusBarHeight() for it)
    const ui::Palette& pal = ui::palette();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 6.0f));
    ImGui::BeginChild("StatusBar", ImVec2(-1, ui::statusBarHeight()),
                      ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    // Fields flow left to right, split by a faint divider.
    auto divider = []() {
        ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.8f);
        ImGui::TextDisabled("|");
        ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.8f);
    };

    const auto& status = modem_ ? modem_->getStatus() : ModemStatus{};
    const VoiceStatus voice = modem_ ? modem_->getVoiceStatus() : VoiceStatus{};

    // UTC time display (like VarAC)
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    char utc_str[32];
    std::strftime(utc_str, sizeof(utc_str), "UTC: %Y-%m-%d %H:%M:%S", std::gmtime(&time_t_now));
    ImGui::Text("%s", utc_str);

    divider();

    // Modem status
    if (status.state == ModemState::Connected) {
        ImGui::TextColored(pal.success, "MODEM OK");
    } else if (modem_ && modem_->isConnecting()) {
        ImGui::TextColored(pal.warning, "MODEM CONNECTING...");
    } else {
        ImGui::TextColored(pal.danger, "MODEM OFF");
    }

    divider();

    // Link status
    if (status.link_state == LinkState::Connected) {
        ImGui::TextColored(pal.success, "LINK: %s", status.remote_callsign.c_str());
    } else {
        ImGui::Text("LINK: IDLE");
    }

    divider();

    // Band
    ImGui::Text("Band: %s", current_band_.c_str());

    divider();

    // Grid square
    ImGui::Text("Grid: %s", config_.station.grid_square.c_str());

    const bool show_voice =
        voice.mode != VoiceMode::Off ||
        voice.state != VoiceState::Idle ||
        voice.active ||
        voice_start_pending_;
    if (show_voice) {
        const std::string transport =
            voice.transport_mode >= 0
                ? ("M" + std::to_string(voice.transport_mode))
                : std::string("OFF");
        const bool voice_hot =
            voice.state == VoiceState::TxRequested ||
            voice.state == VoiceState::TxActive ||
            voice.state == VoiceState::RxActive ||
            voice_start_pending_;
        divider();
        if (voice_hot) {
            ImGui::TextColored(pal.warning,
                               "VOICE: %s %s %s %s%s",
                               voiceModeLabel(voice.mode),
                               voiceStateLabel(voice.state),
                               liveVoiceCodecDisplayLabel(voice.codec),
                               transport.c_str(),
                               voice.ptt ? " PTT" : (voice_start_pending_ ? " PENDING" : ""));
        } else {
            ImGui::Text("VOICE: %s %s %s %s%s",
                        voiceModeLabel(voice.mode),
                        voiceStateLabel(voice.state),
                        liveVoiceCodecDisplayLabel(voice.codec),
                        transport.c_str(),
                        voice_start_pending_ ? " PENDING" : "");
        }
    }

    ImGui::EndChild();  // End StatusBar
}

std::vector<std::pair<std::string, float>> App::computeBandRecommendations() const {
    std::vector<std::pair<std::string, float>> ranked;
    if (!store_ || !store_->isOpen()) {
        return ranked;
    }

    struct Accum {
        double weighted_snr = 0.0;
        double total_weight = 0.0;
        double weighted_prop = 0.0;
        double prop_weight = 0.0;
    };
    std::map<std::string, Accum> by_band;

    const int64_t now = unixNowSeconds();
    std::time_t now_t = static_cast<std::time_t>(now);
    std::tm now_tm = *std::localtime(&now_t);
    const int now_hour = now_tm.tm_hour;

    auto beacons = store_->loadRecentBeacons(400);
    for (const auto& b : beacons) {
        if (b.band.empty()) {
            continue;
        }
        const int64_t age_sec = std::max<int64_t>(0, now - b.timestamp);
        const double age_hours = static_cast<double>(age_sec) / 3600.0;
        std::time_t ts = static_cast<std::time_t>(b.timestamp);
        std::tm tm = *std::localtime(&ts);
        int hour_diff = std::abs(now_hour - tm.tm_hour);
        hour_diff = std::min(hour_diff, 24 - hour_diff);

        const double time_weight = 1.0 / (1.0 + static_cast<double>(hour_diff));
        const double decay_weight = std::exp(-age_hours / 24.0);
        const double weight = time_weight * decay_weight;

        auto& acc = by_band[b.band];
        acc.weighted_snr += static_cast<double>(b.snr_db) * weight;
        acc.total_weight += weight;
        if (freq_mgr_ &&
            !config_.station.grid_square.empty() &&
            !b.grid_square.empty()) {
            auto est = FrequencyManager::estimatePropagationWindow(
                config_.station.grid_square,
                b.grid_square,
                now_hour);
            if (est.valid) {
                acc.weighted_prop += freq_mgr_->scoreBandForPropagation(b.band, est) * weight;
                acc.prop_weight += weight;
            }
        }
    }

    auto pings = store_->loadRecentPings(400);
    for (const auto& p : pings) {
        if (p.band.empty()) {
            continue;
        }
        const int64_t age_sec = std::max<int64_t>(0, now - p.timestamp);
        const double age_hours = static_cast<double>(age_sec) / 3600.0;
        std::time_t ts = static_cast<std::time_t>(p.timestamp);
        std::tm tm = *std::localtime(&ts);
        int hour_diff = std::abs(now_hour - tm.tm_hour);
        hour_diff = std::min(hour_diff, 24 - hour_diff);

        const double time_weight = 1.0 / (1.0 + static_cast<double>(hour_diff));
        const double decay_weight = std::exp(-age_hours / 12.0);
        const double weight = 0.5 * time_weight * decay_weight;
        const double snr = p.online ? p.snr_db : -20.0;

        auto& acc = by_band[p.band];
        acc.weighted_snr += snr * weight;
        acc.total_weight += weight;
    }

    for (const auto& entry : by_band) {
        const auto& band = entry.first;
        const auto& acc = entry.second;
        if (acc.total_weight <= 0.0) {
            continue;
        }
        const double avg_snr = acc.weighted_snr / acc.total_weight;
        const double avg_prop = (acc.prop_weight > 0.0) ? (acc.weighted_prop / acc.prop_weight) : 0.0;
        ranked.emplace_back(band, static_cast<float>(avg_snr + (avg_prop * 0.85)));
    }

    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.second > b.second;
    });
    return ranked;
}

void App::refreshBBSFiles() {
    std::vector<BBSFile> local_files;
    std::filesystem::path bbs_dir(config_.bbs.bbs_path);
    std::error_code ec;
    if (!std::filesystem::exists(bbs_dir, ec)) {
        std::filesystem::create_directories(bbs_dir, ec);
        if (ec) {
            appendSystemChat("[BBS] Cannot create BBS folder " + bbs_dir.string() + ": " + ec.message());
        }
    }

    ec.clear();
    if (std::filesystem::is_directory(bbs_dir, ec)) {
        std::filesystem::directory_iterator it(bbs_dir, ec);
        if (ec) {
            appendSystemChat("[BBS] Cannot read BBS folder " + bbs_dir.string() + ": " + ec.message());
        }
        for (; !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            const auto& entry = *it;
            std::error_code entry_ec;
            if (!entry.is_regular_file(entry_ec) || entry_ec) {
                continue;
            }
            if (entry.path().extension() == ".desc") {
                continue;
            }

            BBSFile file;
            file.filename = entry.path().filename().string();
            const auto size = entry.file_size(entry_ec);
            if (entry_ec) {
                continue;
            }
            file.size_bytes = static_cast<size_t>(size);
            const auto mtime = entry.last_write_time(entry_ec);
            file.upload_time = entry_ec ? 0 : std::chrono::duration_cast<std::chrono::seconds>(
                mtime.time_since_epoch()).count();
            file.remote = false;
            file.uploader = normalizeCallsign(config_.station.callsign);
            if (file.uploader.empty()) {
                file.uploader = "LOCAL";
            }

            std::ifstream desc_file(entry.path().string() + ".desc");
            if (desc_file.is_open()) {
                std::ostringstream ss;
                ss << desc_file.rdbuf();
                file.description = trimSpaces(ss.str());
            }

            local_files.push_back(std::move(file));
        }
    }

    bbs_files_.clear();
    bbs_files_.reserve(local_files.size() + remote_bbs_files_.size());
    bbs_files_.insert(bbs_files_.end(), local_files.begin(), local_files.end());
    for (const auto& entry : remote_bbs_files_) {
        bbs_files_.push_back(entry.second);
    }

    std::sort(bbs_files_.begin(), bbs_files_.end(), [](const BBSFile& a, const BBSFile& b) {
        if (a.remote != b.remote) {
            return !a.remote;
        }
        if (a.uploader != b.uploader) {
            return a.uploader < b.uploader;
        }
        return a.filename < b.filename;
    });

}

void App::uploadToBBS(const std::string& filepath, const std::string& description) {
    std::filesystem::path src(filepath);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(src, ec)) {
        appendSystemChat("[BBS] Upload failed, file not found: " + filepath);
        return;
    }

    const auto file_size = std::filesystem::file_size(src, ec);
    if (ec) {
        appendSystemChat("[BBS] Upload failed for " + filepath + ": " + ec.message());
        return;
    }
    if (file_size > config_.bbs.max_upload_size) {
        appendSystemChat("[BBS] Upload failed, file too large: " + std::to_string(file_size) + " bytes");
        return;
    }

    std::filesystem::path bbs_dir(config_.bbs.bbs_path);
    std::filesystem::create_directories(bbs_dir, ec);
    if (ec) {
        appendSystemChat("[BBS] Upload failed, cannot create " + bbs_dir.string() + ": " + ec.message());
        return;
    }

    std::filesystem::path dest = bbs_dir / src.filename();
    // Uploading a file that already lives in the BBS folder is a re-announce,
    // not a copy (copy_file onto itself fails).
    const bool same_file = std::filesystem::exists(dest, ec) &&
                           std::filesystem::equivalent(src, dest, ec) && !ec;
    ec.clear();
    if (!same_file) {
        std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            appendSystemChat("[BBS] Upload failed for " + src.filename().string() + ": " + ec.message());
            return;
        }
    }

    // Save description to sidecar file
    if (!description.empty()) {
        std::ofstream desc_file(dest.string() + ".desc");
        desc_file << description;
    }

    refreshBBSFiles();

    BBSFile announced;
    announced.filename = src.filename().string();
    announced.description = trimSpaces(description);
    if (announced.description.size() > kBbsMaxDescriptionLen) {
        announced.description.resize(kBbsMaxDescriptionLen);
    }
    announced.size_bytes = static_cast<size_t>(file_size);
    announced.upload_time = unixNowSeconds();
    announced.uploader = normalizeCallsign(config_.station.callsign);
    announced.remote = false;
    announceBBSFile(announced);
}

void App::downloadFromBBS(const BBSFile& file) {
    if (file.remote) {
        requestRemoteBBSDownload(file.uploader, file.filename);
        return;
    }
    downloadFromBBS(file.filename);
}

void App::downloadFromBBS(const std::string& filename) {
    // Sanitize filename to prevent path traversal
    std::string safe_name = std::filesystem::path(filename).filename().string();
    if (safe_name.empty() || safe_name == "." || safe_name == "..") {
        appendSystemChat("[BBS] Invalid filename: " + filename);
        return;
    }

    std::filesystem::path src(config_.bbs.bbs_path);
    src /= safe_name;

    std::error_code ec;
    if (!std::filesystem::exists(src, ec)) {
        for (const auto& entry : remote_bbs_files_) {
            const BBSFile& remote = entry.second;
            if (!remote.remote || remote.filename != safe_name) {
                continue;
            }
            requestRemoteBBSDownload(remote.uploader, remote.filename);
            return;
        }
        appendSystemChat("[BBS] File not found: " + safe_name);
        return;
    }

    std::string download_dir = trimSpaces(config_.files.download_path);
    if (download_dir.empty()) {
        download_dir = "downloads";
    }
    std::filesystem::path dest(download_dir);
    std::filesystem::create_directories(dest, ec);
    if (ec) {
        appendSystemChat("[BBS] Cannot create download folder " + dest.string() + ": " + ec.message());
        return;
    }
    dest /= safe_name;

    if (std::filesystem::exists(dest, ec) && std::filesystem::equivalent(src, dest, ec) && !ec) {
        return;  // Download folder is the BBS folder; nothing to copy.
    }
    ec.clear();
    std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        appendSystemChat("[BBS] Download of " + safe_name + " failed: " + ec.message());
        return;
    }
    appendSystemChat("[BBS] Saved " + safe_name + " to " + dest.string());
}

void App::deleteBBSFile(const std::string& filename) {
    // Sanitize filename to prevent path traversal
    std::string safe_name = std::filesystem::path(filename).filename().string();
    if (safe_name.empty() || safe_name == "." || safe_name == "..") {
        appendSystemChat("[BBS] Invalid filename: " + filename);
        return;
    }

    std::filesystem::path file(config_.bbs.bbs_path);
    file /= safe_name;

    std::error_code ec;
    if (std::filesystem::exists(file, ec)) {
        std::filesystem::remove(file, ec);
        if (ec) {
            appendSystemChat("[BBS] Cannot delete " + safe_name + ": " + ec.message());
            return;
        }

        // Also remove description sidecar
        std::filesystem::path desc(file.string() + ".desc");
        std::filesystem::remove(desc, ec);

        refreshBBSFiles();
        announceBBSFileRemoval(safe_name);
    }
}

void App::announceBBSFile(const BBSFile& file) {
    if (!modem_) {
        return;
    }
    std::string safe_name = std::filesystem::path(file.filename).filename().string();
    if (safe_name.empty()) {
        return;
    }
    if (safe_name.size() > kBbsMaxFilenameLen) {
        safe_name.resize(kBbsMaxFilenameLen);
    }
    std::string description = trimSpaces(file.description);
    if (description.size() > kBbsMaxDescriptionLen) {
        description.resize(kBbsMaxDescriptionLen);
    }
    std::string owner = normalizeCallsign(config_.station.callsign);
    if (owner.size() > 15) {
        owner.resize(15);
    }

    std::vector<uint8_t> packet;
    packet.reserve(1 + 1 + 1 + 4 + safe_name.size() + description.size() + 1 + owner.size());
    packet.push_back(kBbsAnnounceMarker);
    packet.push_back(static_cast<uint8_t>(safe_name.size()));
    packet.push_back(static_cast<uint8_t>(description.size()));
    writeUint32LE(packet, static_cast<uint32_t>(
        std::min<size_t>(file.size_bytes, static_cast<size_t>(std::numeric_limits<uint32_t>::max()))));
    packet.insert(packet.end(), safe_name.begin(), safe_name.end());
    packet.insert(packet.end(), description.begin(), description.end());
    packet.push_back(static_cast<uint8_t>(owner.size()));
    packet.insert(packet.end(), owner.begin(), owner.end());

    (void)sendBroadcastPayload(packet);
}

void App::announceBBSFileRemoval(const std::string& filename) {
    if (!modem_) {
        return;
    }
    std::string safe_name = std::filesystem::path(filename).filename().string();
    if (safe_name.empty()) {
        return;
    }
    if (safe_name.size() > kBbsMaxFilenameLen) {
        safe_name.resize(kBbsMaxFilenameLen);
    }
    std::string owner = normalizeCallsign(config_.station.callsign);
    if (owner.size() > 15) {
        owner.resize(15);
    }

    std::vector<uint8_t> packet;
    packet.reserve(1 + 1 + safe_name.size() + 1 + owner.size());
    packet.push_back(kBbsRemoveMarker);
    packet.push_back(static_cast<uint8_t>(safe_name.size()));
    packet.insert(packet.end(), safe_name.begin(), safe_name.end());
    packet.push_back(static_cast<uint8_t>(owner.size()));
    packet.insert(packet.end(), owner.begin(), owner.end());
    (void)sendBroadcastPayload(packet);
}

void App::processBBSAnnouncement(const std::string& from, const std::vector<uint8_t>& data) {
    if (data.size() < 8) {
        return;
    }
    const uint8_t name_len = data[1];
    const uint8_t desc_len = data[2];
    size_t offset = 3;
    if (data.size() < offset + 4) {
        return;
    }
    const uint32_t size_bytes = readUint32LE(data.data(), offset);
    offset += 4;
    if (data.size() < offset + name_len + desc_len) {
        return;
    }

    std::string filename(data.begin() + offset, data.begin() + offset + name_len);
    offset += name_len;
    std::string description(data.begin() + offset, data.begin() + offset + desc_len);
    offset += desc_len;

    filename = std::filesystem::path(filename).filename().string();
    if (filename.empty() || filename == "." || filename == "..") {
        return;
    }
    // The owner is the station that sent the announcement; the owner field
    // inside the packet is ignored so nobody can list files under another
    // station's callsign.
    const std::string owner = normalizeCallsign(from);
    if (owner.empty() || owner == "UNKNOWN") {
        return;
    }

    BBSFile file;
    file.filename = filename;
    file.description = trimSpaces(description);
    file.size_bytes = size_bytes;
    file.upload_time = unixNowSeconds();
    file.uploader = owner;
    file.remote = true;

    upsertRemoteBBSFile(owner, file);

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "[BBS] " + file.uploader + " shared " + file.filename;
    msg.is_outgoing = false;
    msg.timestamp = unixNowSeconds();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::processBBSRemoval(const std::string& from, const std::vector<uint8_t>& data) {
    if (data.size() < 3) {
        return;
    }
    const uint8_t name_len = data[1];
    if (data.size() < 2 + name_len) {
        return;
    }
    std::string filename(data.begin() + 2, data.begin() + 2 + name_len);
    filename = std::filesystem::path(filename).filename().string();
    if (filename.empty() || filename == "." || filename == "..") {
        return;
    }

    // Only the owner (the sender of the original announcement) can remove
    // a listing; the key is built from the broadcast sender.
    const std::string owner = normalizeCallsign(from);
    if (owner.empty() || owner == "UNKNOWN") {
        return;
    }
    removeRemoteBBSFile(owner, filename);
}

void App::processBBSFileRequest(const std::string& from, const std::vector<uint8_t>& data) {
    if (!modem_ || !file_transfer_mgr_ || data.size() < 3) {
        return;
    }
    if (!config_.bbs.enabled) {
        appendSystemChat("[BBS] File request from " + normalizeCallsign(from) + " refused: BBS is disabled");
        return;
    }
    const uint8_t name_len = data[1];
    if (name_len == 0 || data.size() < 2 + name_len) {
        return;
    }
    std::string filename(data.begin() + 2, data.begin() + 2 + name_len);
    filename = std::filesystem::path(filename).filename().string();
    if (filename.empty() || filename == "." || filename == "..") {
        return;
    }
    size_t owner_offset = 2 + name_len;
    std::string target_owner;
    if (data.size() > owner_offset) {
        const uint8_t owner_len = data[owner_offset++];
        if (owner_len > 0 && data.size() >= owner_offset + owner_len) {
            target_owner.assign(data.begin() + owner_offset, data.begin() + owner_offset + owner_len);
            target_owner = normalizeCallsign(target_owner);
        }
    }

    const std::string local_call = normalizeCallsign(config_.station.callsign);
    if (!target_owner.empty() && target_owner != "UNKNOWN" &&
        !local_call.empty() && target_owner != local_call) {
        return;
    }

    std::string requester = normalizeCallsign(from);
    if ((requester.empty() || requester == "UNKNOWN") && modem_) {
        requester = normalizeCallsign(modem_->getStatus().remote_callsign);
    }
    if (requester.empty() || requester == "UNKNOWN") {
        return;
    }

    std::filesystem::path src(config_.bbs.bbs_path);
    src /= filename;
    std::error_code src_ec;
    if (filename.size() > 5 && filename.compare(filename.size() - 5, 5, ".desc") == 0) {
        return;  // Sidecars are not BBS files.
    }
    if (!std::filesystem::is_regular_file(src, src_ec)) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "[BBS] Request from " + requester + " ignored: file not found (" + filename + ")";
        msg.is_outgoing = false;
        msg.timestamp = unixNowSeconds();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    const std::string transfer_id =
        file_transfer_mgr_->sendFile(*modem_, requester, src.string());
    if (transfer_id.empty()) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "[BBS] Failed to send requested file " + filename + " to " + normalizeCallsign(from);
        msg.is_outgoing = false;
        msg.timestamp = unixNowSeconds();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "[BBS] Sending " + filename + " to " + normalizeCallsign(from);
    msg.is_outgoing = false;
    msg.timestamp = unixNowSeconds();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::requestRemoteBBSDownload(const std::string& from, const std::string& filename) {
    if (!modem_) {
        return;
    }
    std::string safe_name = std::filesystem::path(filename).filename().string();
    if (safe_name.empty() || safe_name == "." || safe_name == "..") {
        return;
    }
    if (safe_name.size() > kBbsMaxFilenameLen) {
        safe_name.resize(kBbsMaxFilenameLen);
    }

    std::string owner = normalizeCallsign(from);
    if (owner.empty() || owner == "UNKNOWN") {
        for (const auto& [key, remote] : remote_bbs_files_) {
            if (remote.filename == safe_name &&
                !remote.uploader.empty() &&
                remote.uploader != "UNKNOWN") {
                owner = normalizeCallsign(remote.uploader);
                break;
            }
        }
    }
    if (owner.empty() || owner == "UNKNOWN") {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "[BBS] Unable to determine source station for " + safe_name;
        msg.is_outgoing = false;
        msg.timestamp = unixNowSeconds();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }
    if (!modem_->isConnected()) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "[BBS] Cannot fetch " + safe_name + ": modem is disconnected";
        msg.is_outgoing = false;
        msg.timestamp = unixNowSeconds();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    std::string connected_peer = normalizeCallsign(modem_->getStatus().remote_callsign);
    if (connected_peer.empty()) {
        connected_peer = normalizeCallsign(current_chat_partner_);
    }

    std::string download_dir = trimSpaces(config_.files.download_path);
    if (download_dir.empty()) {
        download_dir = "downloads";
    }
    std::error_code download_dir_ec;
    std::filesystem::create_directories(download_dir, download_dir_ec);
    if (file_transfer_mgr_) {
        file_transfer_mgr_->setDownloadPath(download_dir);
    }

    prunePendingRemoteBbsDownloads();
    const std::string pending_key = bbsRemoteKey(owner, safe_name);
    PendingRemoteBbsDownload pending;
    pending.filename = safe_name;
    pending.owner_callsign = owner;
    pending.requested_at = unixNowSeconds();
    pending.request_sent = false;
    pending_remote_bbs_downloads_[pending_key] = pending;
    syncPendingBbsDownloadsForUi();

    const bool linked_to_owner =
        modem_->getStatus().link_state == LinkState::Connected &&
        connected_peer == owner;
    if (linked_to_owner) {
        if (sendRemoteBbsFileRequest(owner, safe_name, true)) {
            auto it = pending_remote_bbs_downloads_.find(pending_key);
            if (it != pending_remote_bbs_downloads_.end()) {
                it->second.request_sent = true;
                it->second.requested_at = unixNowSeconds();
                syncPendingBbsDownloadsForUi();
            }
        }
        return;
    }

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "[BBS] Connecting to " + owner + " to fetch " + safe_name;
    msg.is_outgoing = false;
    msg.timestamp = unixNowSeconds();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;

    connectToStation(owner);
}

bool App::sendRemoteBbsFileRequest(const std::string& owner,
                                   const std::string& filename,
                                   bool emit_chat) {
    if (!modem_) {
        return false;
    }
    const std::string safe_name = std::filesystem::path(filename).filename().string();
    if (safe_name.empty() || safe_name == "." || safe_name == "..") {
        return false;
    }
    std::string target_owner = normalizeCallsign(owner);
    if (target_owner == "UNKNOWN") {
        target_owner.clear();
    }
    if (target_owner.size() > 15) {
        target_owner.resize(15);
    }
    std::vector<uint8_t> packet;
    packet.reserve(1 + 1 + safe_name.size() + 1 + target_owner.size());
    packet.push_back(kBbsFileRequestMarker);
    packet.push_back(static_cast<uint8_t>(safe_name.size()));
    packet.insert(packet.end(), safe_name.begin(), safe_name.end());
    packet.push_back(static_cast<uint8_t>(target_owner.size()));
    packet.insert(packet.end(), target_owner.begin(), target_owner.end());

    if (!modem_->sendData(packet)) {
        if (emit_chat) {
            ChatMessage msg;
            msg.from = "SYSTEM";
            msg.text = "[BBS] Failed to request " + safe_name + " from " + owner;
            msg.is_outgoing = false;
            msg.timestamp = unixNowSeconds();
            chat_history_.push_back(msg);
            chat_scroll_to_bottom_ = true;
        }
        return false;
    }

    if (emit_chat) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "[BBS] Requested " + safe_name + " from " + owner;
        msg.is_outgoing = false;
        msg.timestamp = unixNowSeconds();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
    }
    return true;
}

bool App::flushPendingRemoteBbsDownloadsForPeer(const std::string& peer, bool emit_chat) {
    prunePendingRemoteBbsDownloads();
    const std::string normalized_peer = normalizeCallsign(peer);
    if (normalized_peer.empty() || !modem_ ||
        modem_->getStatus().link_state != LinkState::Connected) {
        return false;
    }
    bool had_pending_for_peer = false;
    bool changed = false;
    for (auto& [key, pending] : pending_remote_bbs_downloads_) {
        (void)key;
        if (normalizeCallsign(pending.owner_callsign) != normalized_peer) {
            continue;
        }
        had_pending_for_peer = true;
        if (pending.request_sent) {
            continue;
        }
        if (sendRemoteBbsFileRequest(normalized_peer, pending.filename, emit_chat)) {
            pending.request_sent = true;
            pending.requested_at = unixNowSeconds();
            changed = true;
        }
    }
    if (changed) {
        syncPendingBbsDownloadsForUi();
    }
    return had_pending_for_peer;
}

void App::syncPendingBbsDownloadsForUi() {
    pending_bbs_downloads_ui_.clear();
    pending_bbs_downloads_ui_.reserve(pending_remote_bbs_downloads_.size());
    for (const auto& [key, pending] : pending_remote_bbs_downloads_) {
        (void)key;
        PendingBbsDownload ui;
        ui.filename = pending.filename;
        ui.owner_callsign = pending.owner_callsign;
        ui.requested_at = pending.requested_at;
        ui.request_sent = pending.request_sent;
        pending_bbs_downloads_ui_.push_back(std::move(ui));
    }
}

void App::prunePendingRemoteBbsDownloads() {
    const int64_t now = unixNowSeconds();
    std::vector<PendingRemoteBbsDownload> expired;
    for (auto it = pending_remote_bbs_downloads_.begin(); it != pending_remote_bbs_downloads_.end();) {
        const int64_t age = now - it->second.requested_at;
        if (age > REMOTE_BBS_DOWNLOAD_REQUEST_TTL_SEC) {
            expired.push_back(it->second);
            it = pending_remote_bbs_downloads_.erase(it);
            continue;
        }
        ++it;
    }
    syncPendingBbsDownloadsForUi();
    for (const auto& stale : expired) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "[BBS] Download request timed out for " + stale.filename +
                   " from " + stale.owner_callsign;
        msg.is_outgoing = false;
        msg.timestamp = now;
        chat_history_.push_back(msg);
    }
    if (!expired.empty()) {
        chat_scroll_to_bottom_ = true;
    }
}

bool App::shouldAcceptRemoteBbsOffer(const std::string& from, const std::string& filename) {
    prunePendingRemoteBbsDownloads();
    const std::string safe_name = std::filesystem::path(filename).filename().string();
    if (safe_name.empty()) {
        return false;
    }
    const std::string offer_from = normalizeCallsign(from);
    if (!offer_from.empty() && offer_from != "UNKNOWN") {
        auto exact = pending_remote_bbs_downloads_.find(bbsRemoteKey(offer_from, safe_name));
        if (exact != pending_remote_bbs_downloads_.end()) {
            pending_remote_bbs_downloads_.erase(exact);
            syncPendingBbsDownloadsForUi();
            return true;
        }
    }

    std::vector<std::string> candidates;
    for (const auto& [key, pending] : pending_remote_bbs_downloads_) {
        if (pending.filename != safe_name) {
            continue;
        }
        if (!offer_from.empty() && offer_from != "UNKNOWN" &&
            normalizeCallsign(pending.owner_callsign) != offer_from) {
            continue;
        }
        candidates.push_back(key);
    }
    if (candidates.size() == 1) {
        pending_remote_bbs_downloads_.erase(candidates.front());
        syncPendingBbsDownloadsForUi();
        return true;
    }
    return false;
}

void App::upsertRemoteBBSFile(const std::string& from, const BBSFile& file) {
    std::string key = bbsRemoteKey(from, file.filename);
    BBSFile remote = file;
    remote.uploader = normalizeCallsign(from);
    remote.remote = true;
    if (remote_bbs_files_.find(key) == remote_bbs_files_.end()) {
        // Bound the remote listing; evict the oldest announcement.
        while (remote_bbs_files_.size() >= MAX_REMOTE_BBS_FILES) {
            auto oldest = remote_bbs_files_.begin();
            for (auto it = remote_bbs_files_.begin(); it != remote_bbs_files_.end(); ++it) {
                if (it->second.upload_time < oldest->second.upload_time) {
                    oldest = it;
                }
            }
            remote_bbs_files_.erase(oldest);
        }
    }
    remote_bbs_files_[key] = std::move(remote);
    refreshBBSFiles();
}

void App::removeRemoteBBSFile(const std::string& from, const std::string& filename) {
    const std::string key = bbsRemoteKey(from, filename);
    const auto erased = remote_bbs_files_.erase(key);
    if (erased > 0) {
        refreshBBSFiles();
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "[BBS] " + normalizeCallsign(from) + " removed " +
                   std::filesystem::path(filename).filename().string();
        msg.is_outgoing = false;
        msg.timestamp = unixNowSeconds();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
    }
}

bool App::decidePendingFileOffer(const std::string& transfer_id, bool accept) {
    if (!file_transfer_mgr_ || !modem_) {
        return false;
    }
    PendingFileOffer offer;
    bool found = false;
    for (const auto& o : file_transfer_mgr_->getPendingOffers()) {
        if (o.transfer_id == transfer_id) {
            offer = o;
            found = true;
            break;
        }
    }
    if (!found) {
        return false;
    }
    const bool ok = accept ? file_transfer_mgr_->acceptPendingOffer(*modem_, transfer_id)
                           : file_transfer_mgr_->rejectPendingOffer(*modem_, transfer_id);
    if (ok) {
        const std::string from = normalizeCallsign(offer.from);
        appendSystemChat(std::string(accept ? "Accepted" : "Declined") + " file offer " + offer.filename +
                         " from " + (from.empty() ? std::string("UNKNOWN") : from));
        syncTransfersFromManager();
    }
    return ok;
}

bool App::decideNewestPendingFileOffer(bool accept) {
    if (!file_transfer_mgr_) {
        return false;
    }
    const auto offers = file_transfer_mgr_->getPendingOffers();
    return !offers.empty() && decidePendingFileOffer(offers.back().transfer_id, accept);
}

void App::dropPendingFileOffers() {
    if (!file_transfer_mgr_ || file_transfer_mgr_->getPendingOffers().empty()) {
        return;
    }
    file_transfer_mgr_->dropPendingOffers();
    appendSystemChat("Pending file offers dropped: the link ended before you answered");
    syncTransfersFromManager();
}

void App::renderPendingFileOffers() {
    if (!file_transfer_mgr_) {
        return;
    }
    const auto offers = file_transfer_mgr_->getPendingOffers();
    if (offers.empty()) {
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Incoming File", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const int64_t now = unixNowSeconds();
        for (const auto& offer : offers) {
            const std::string from = normalizeCallsign(offer.from);
            ImGui::PushID(offer.transfer_id.c_str());
            ImGui::Text("%s offers %s (%zu bytes compressed)",
                        from.empty() ? "UNKNOWN" : from.c_str(),
                        offer.filename.c_str(), offer.size);
            const int64_t left = std::max<int64_t>(
                0, DEFAULT_PENDING_OFFER_TIMEOUT_SEC - (now - offer.offered_at));
            ImGui::TextDisabled("Declined automatically in %lld s", static_cast<long long>(left));
            if (ImGui::Button("Accept")) {
                decidePendingFileOffer(offer.transfer_id, true);
            }
            ImGui::SameLine();
            if (ImGui::Button("Decline")) {
                decidePendingFileOffer(offer.transfer_id, false);
            }
            ImGui::Separator();
            ImGui::PopID();
        }
    }
    ImGui::End();
}

void App::appendSystemChat(const std::string& text) {
    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = text;
    msg.is_outgoing = false;
    msg.timestamp = unixNowSeconds();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::applyMeshRelaySettings() {
    if (!mesh_relay_) {
        return;
    }
    mesh_relay_->setMyCallsign(normalizeCallsign(config_.station.callsign));
    mesh_relay_->setEnabled(config_.email.hf_store_forward_enabled);
    mesh_relay_->setDefaultTTL(static_cast<uint8_t>(std::clamp(config_.email.hf_store_forward_ttl, 1, 15)));
    mesh_relay_->setMaxHopCount(static_cast<uint8_t>(std::clamp(config_.email.hf_store_forward_hop_limit, 1, 20)));
    mesh_relay_->setDedupWindowSec(std::clamp(config_.email.hf_store_forward_dedup_window_sec, 30, 3600));
}

void App::flushMeshRelayQueue() {
    if (!mesh_relay_ || !modem_ || !mesh_relay_->isEnabled() || !modem_->isConnected()) {
        return;
    }
    const ModemStatus status = modem_->getStatus();
    const bool busy = status.busy_detected || status.ptt_active || status.tx_buffer_bytes > 0;
    if (busy) {
        return;
    }

    RelayQueueEntry entry;
    if (!mesh_relay_->getNextRelay(entry)) {
        return;
    }

    if (!sendBroadcastPayload(entry.packet)) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Mesh relay forward failed for " + entry.header.destination;
        msg.is_outgoing = false;
        msg.timestamp = unixNowSeconds();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
    }
}

bool App::forwardPacketViaMesh(const std::vector<uint8_t>& payload,
                               const std::string& destination_callsign,
                               const std::string& message_id) {
    if (!mesh_relay_ || !modem_ || !modem_->isConnected() || !mesh_relay_->isEnabled()) {
        return false;
    }

    const std::string destination = normalizeCallsign(destination_callsign);
    if (destination.empty()) {
        return false;
    }

    auto relay_packet = mesh_relay_->createRelayPacket(destination, payload, message_id);
    const bool ok = modem_->sendData(relay_packet);
    if (ok && !message_id.empty()) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Relayed MID " + message_id + " toward " + destination +
                   " (TTL " + std::to_string(mesh_relay_->getDefaultTTL()) + ")";
        msg.is_outgoing = false;
        msg.timestamp = unixNowSeconds();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
    }
    return ok;
}

// Event handlers

void App::onModemStateChange(const std::string& state) {
    (void)state;
}

void App::onDataReceived(const std::vector<uint8_t>& data) {
    // The data socket carries link traffic, which is from the connected
    // peer. Senders are never taken from inside the payload here.
    if (!data.empty() &&
        modem_ &&
        modem_->getStatus().link_state != LinkState::Connected &&
        isDisconnectedBroadcastPayload(data[0])) {
        // When idle/listening, disconnected broadcast traffic should arrive through the
        // sender-tagged monitor path. Ignore raw data-socket duplicates that would
        // otherwise show up as UNKNOWN/from-local artifacts in the UI.
        return;
    }

    processIncomingMessage(data, "", RxOrigin::Link);
}

// Skywave separates telemetry from application traffic: BEACON and CQ carry
// only station metadata, while operator payloads ride BCAST_DATA fragment
// chains and reach this app through onBroadcastMessage(). `frame.payload` is
// the link-frame body with the envelope stripped (see skywave_protocol.hpp).
void App::onMonitorFrame(const MonitorFrame& frame) {
    const std::string frame_type = toUpperAscii(frame.frame_type);
    const std::string sender = normalizeCallsign(frame.src_callsign);

    if (frame_type == "CQ") {
        const std::string cq_from = sender.empty() ? std::string("UNKNOWN") : sender;
        addHeardStation(cq_from);
        processCQResponse(cq_from);
        return;
    }

    if (frame_type != "BEACON") {
        return;
    }

    // BEACON body: callsign, noise floor (biased +20 dB), family, sw version.
    float snr = 0.0f;
    if (auto beacon = parseSkywaveBeacon(frame.payload)) {
        snr = beacon->snr_floor_db;
    }
    processReceivedBeacon(sender.empty() ? "UNKNOWN" : sender, "", "", snr, false);
}

void App::onBroadcastMessage(const std::string& raw_sender,
                             const std::vector<uint8_t>& payload) {
    const std::string sender = normalizeCallsign(raw_sender);
    if (payload.empty()) {
        return;
    }

    // Application envelope used by this app's own broadcasts.
    if (payload.size() >= 4 &&
        payload[0] == 0xBD &&
        payload[1] == 0x31 &&
        payload[2] == 0x01) {
        const size_t app_len = static_cast<size_t>(payload[3]);
        if ((4u + app_len) <= payload.size()) {
            // Same whitelist as an unwrapped broadcast: the envelope must
            // not smuggle session-only types past it.
            if (app_len > 0 && isDisconnectedBroadcastPayload(payload[4])) {
                std::vector<uint8_t> inner(payload.begin() + 4,
                                           payload.begin() + 4 + app_len);
                processIncomingMessage(inner, sender, RxOrigin::Broadcast);
            }
            return;
        }
    }

    if (isDisconnectedBroadcastPayload(payload[0])) {
        processIncomingMessage(payload, sender, RxOrigin::Broadcast);
    }
}

void App::onConnected(const std::string& callsign) {
    std::string connected_peer = normalizeCallsign(callsign);
    if (connected_peer.empty() && modem_) {
        connected_peer = normalizeCallsign(modem_->getStatus().remote_callsign);
    }
    if (connected_peer.empty() && !pending_connect_target_.empty()) {
        connected_peer = normalizeCallsign(pending_connect_target_);
    }
    if (connected_peer.empty()) {
        connected_peer = "UNKNOWN";
    }
    current_chat_partner_ = connected_peer;

    const std::string normalized_pending = normalizeCallsign(pending_connect_target_);
    bool matches_pending = !normalized_pending.empty() &&
                           normalizeCallsign(current_chat_partner_) == normalized_pending;
    if (qsy_workflow_state_ == QsyWorkflowState::ReconnectConnecting &&
        !qsy_workflow_peer_.empty()) {
        matches_pending = normalizeCallsign(current_chat_partner_) ==
                          normalizeCallsign(qsy_workflow_peer_);
    }
    if (matches_pending) {
        qsy_initiator_ = true;
        if (qsy_workflow_peer_.empty()) {
            qsy_workflow_peer_ = normalizeCallsign(current_chat_partner_);
        }
    } else if (qsy_workflow_state_ == QsyWorkflowState::ReconnectPending ||
               qsy_workflow_state_ == QsyWorkflowState::ReconnectConnecting) {
        // A different peer connected while recovery was pending; fall back to responder role.
        qsy_initiator_ = false;
        qsy_workflow_peer_ = normalizeCallsign(current_chat_partner_);
        qsy_reconnect_attempts_ = 0;
        qsy_workflow_state_ = QsyWorkflowState::Idle;
    } else {
        qsy_initiator_ = false;
        qsy_workflow_peer_ = normalizeCallsign(current_chat_partner_);
        qsy_reconnect_attempts_ = 0;
        qsy_workflow_state_ = QsyWorkflowState::Idle;
    }
    pending_connect_target_.clear();

    if (!auto_gateway_target_.empty() &&
        normalizeCallsign(current_chat_partner_) == normalizeCallsign(auto_gateway_target_)) {
        ChatMessage gw_msg;
        gw_msg.from = "SYSTEM";
        gw_msg.text = "Gateway link established with " + current_chat_partner_;
        gw_msg.is_outgoing = false;
        gw_msg.timestamp = unixNowSeconds();
        chat_history_.push_back(gw_msg);
        chat_scroll_to_bottom_ = true;
    }

    // Start QSO tracking
    startQSO(current_chat_partner_);

    // Stop CQ if active
    if (cq_active_) {
        stopCQ();
    }

    // Queue PSK Reporter spot for connection
    if (config_.psk_reporter.enabled) {
        // Get SNR from modem status if available
        float snr = 0.0f;
        if (modem_) {
            snr = modem_->getStatus().snr_db;
        }
        if (snr >= config_.psk_reporter.min_snr) {
            queuePSKReporterSpot(current_chat_partner_, "", snr);
        }
    }

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "Connected to " + current_chat_partner_;
    msg.is_outgoing = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;

    // SNR keep-alive is scheduled in startQSO() with staggered timing.

    if (config_.voice.auto_start &&
        configVoiceMode(config_.voice.mode) == VoiceMode::Connected &&
        modem_ && modem_->isConnected()) {
        applyVoiceSettingsToModem(false);
        voice_start_pending_ = true;
        voice_start_inflight_ = false;
    }

    // If away mode, send auto-reply
    if (away_mode_active_ && config_.away.auto_reply) {
        sendChatMessage("[AUTO] " + config_.away.away_message);
    }

    const bool gateway_session =
        normalizeCallsign(current_chat_partner_) == normalizeCallsign(auto_gateway_target_) ||
        isKnownEmailGateway(current_chat_partner_);
    const bool pending_bbs_download = flushPendingRemoteBbsDownloadsForPeer(current_chat_partner_);
    if (pending_connect_auto_qsy_ && qsy_initiator_ &&
        config_.radio.auto_qsy && !gateway_session && !pending_bbs_download) {
        std::string target_band;
        int target_slot = 0;
        uint32_t target_freq_hz = 0;
        if (buildAutoQsyTarget(&target_band, &target_slot, &target_freq_hz)) {
            sendQsySuggestion(target_band, target_slot, target_freq_hz);
        }
    }
    pending_connect_auto_qsy_ = false;
}

void App::onDisconnected() {
    dropPendingFileOffers();
    voice_local_ptt_ = false;
    voice_auto_started_ = false;
    voice_start_pending_ = false;
    voice_start_inflight_ = false;
    pending_connect_auto_qsy_ = false;

    if (modem_ && modem_->isConnected()) {
        (void)modem_->setListenMode(true);
    }

    const bool workflow_active = (qsy_workflow_state_ != QsyWorkflowState::Idle) ||
                                 !qsy_target_band_.empty() || qsy_suggestion_pending_;
    const auto now = std::chrono::steady_clock::now();
    const int window_ms = std::clamp(config_.radio.qsy_reconnect_window_ms, 1000, 120000);
    const bool sideband_failed_soon =
        (qsy_workflow_state_ == QsyWorkflowState::SidebandActive) &&
        (std::chrono::duration_cast<std::chrono::milliseconds>(
             now - qsy_sideband_started_).count() <= window_ms);
    const bool pre_sideband_failure =
        (qsy_workflow_state_ == QsyWorkflowState::AwaitingAck) ||
        (qsy_workflow_state_ == QsyWorkflowState::ReconnectPending) ||
        (qsy_workflow_state_ == QsyWorkflowState::ReconnectConnecting);
    const bool should_recover = sideband_failed_soon || pre_sideband_failure;

    if (!current_chat_partner_.empty()) {
        // End QSO and show duration
        std::string duration = formatQSODuration();
        endQSO();

        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Disconnected from " + current_chat_partner_ + " (QSO: " + duration + ")";
        msg.is_outgoing = false;
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
    }
    current_chat_partner_.clear();

    // Requests sent over the dropped link must be re-sent after reconnect.
    bool bbs_pending_changed = false;
    for (auto& [key, pending] : pending_remote_bbs_downloads_) {
        (void)key;
        if (pending.request_sent) {
            pending.request_sent = false;
            bbs_pending_changed = true;
        }
    }
    if (bbs_pending_changed) {
        syncPendingBbsDownloadsForUi();
    }

    if (workflow_active) {
        returnToCallingFrequency("QSY failover: Returned to calling frequency");
    } else if (away_mode_active_) {
        // In away mode, return to calling frequency to listen for more connections.
        returnToCallingFrequency("Away mode: Returned to calling frequency");
    }

    if (should_recover && qsy_initiator_ && modem_ && modem_->isConnected() &&
        !qsy_workflow_peer_.empty()) {
        if (qsy_workflow_state_ == QsyWorkflowState::ReconnectPending &&
            qsy_reconnect_due_ > now) {
            return;
        }
        const int max_attempts = std::clamp(config_.radio.qsy_max_reconnect_attempts, 1, 10);
        if (qsy_reconnect_attempts_ < max_attempts) {
            qsy_reconnect_attempts_++;
            const int delay_ms = std::clamp(config_.radio.qsy_reconnect_delay_ms, 500, 15000);
            qsy_reconnect_due_ = now + std::chrono::milliseconds(delay_ms);
            qsy_workflow_state_ = QsyWorkflowState::ReconnectPending;
            pending_connect_target_ = qsy_workflow_peer_;

            ChatMessage retry_msg;
            retry_msg.from = "SYSTEM";
            retry_msg.text = "QSY link failed. Reconnect attempt " +
                             std::to_string(qsy_reconnect_attempts_) + "/" +
                             std::to_string(max_attempts) + " to " + qsy_workflow_peer_ +
                             " will start on calling frequency.";
            retry_msg.is_outgoing = false;
            retry_msg.timestamp = unixNowSeconds();
            chat_history_.push_back(retry_msg);
            chat_scroll_to_bottom_ = true;
            return;
        }

        ChatMessage fail_msg;
        fail_msg.from = "SYSTEM";
        fail_msg.text = "QSY recovery exhausted after " + std::to_string(max_attempts) +
                        " attempts. Station link will remain disconnected.";
        fail_msg.is_outgoing = false;
        fail_msg.timestamp = unixNowSeconds();
        chat_history_.push_back(fail_msg);
        chat_scroll_to_bottom_ = true;
    }

    resetQsyWorkflow(true);
}

void App::connectToModem() {
    if (!modem_) return;
    if (modem_->isConnecting()) return;

    // The TCP connects run on a helper thread (see pollModemConnect()), so an
    // unreachable host never freezes the window.
    if (modem_->beginConnect(config_.modem.host,
                             config_.modem.command_port,
                             config_.modem.data_port,
                             config_.modem.voice_port)) {
        shared_state_.addSystemMessage("Connecting to modem at " + config_.modem.host + ":" +
                                       std::to_string(config_.modem.command_port) + "...");
    }
}

struct App::PathPickerJob {
    std::mutex mu;
    bool done = false;
    std::string path;
};

void App::startPathPicker(bool folder, PathPickedCallback done) {
#ifdef __APPLE__
    if (path_picker_job_) {
        return;  // a picker is already open
    }
    auto job = std::make_shared<PathPickerJob>();
    path_picker_job_ = job;
    path_picker_done_ = std::move(done);
    try {
        // osascript shows the dialog in its own process; waiting for it on a
        // helper thread keeps the window rendering and the modem polled.
        std::thread([job, folder]() {
            std::string path = folder ? openNativeFolderDialog() : openNativeFileDialog();
            std::lock_guard<std::mutex> lock(job->mu);
            job->path = std::move(path);
            job->done = true;
        }).detach();
    } catch (const std::exception&) {
        path_picker_job_.reset();
        path_picker_done_ = nullptr;
    }
#else
    (void)folder;
    (void)done;
#endif
}

void App::pollPathPicker() {
    if (!path_picker_job_) return;
    std::string path;
    {
        std::lock_guard<std::mutex> lock(path_picker_job_->mu);
        if (!path_picker_job_->done) return;
        path = path_picker_job_->path;
    }
    path_picker_job_.reset();
    PathPickedCallback done = std::move(path_picker_done_);
    path_picker_done_ = nullptr;
    if (done) done(path);
}

void App::pollAPRSConnect() {
    if (!aprs_ || !aprs_->isConnecting()) return;
    switch (aprs_->pollConnect()) {
    case APRSConnectProgress::Connected:
        if (aprs_->takeQueuedPositionSent()) {
            shared_state_.addSystemMessage("APRS-IS position beacon sent");
        }
        break;
    case APRSConnectProgress::Failed:
        shared_state_.addSystemMessage("APRS-IS connect failed: " +
                                       (aprs_->lastConnectError().empty()
                                            ? std::string("no connection")
                                            : aprs_->lastConnectError()));
        break;
    default:
        break;
    }
}

void App::pollModemConnect() {
    if (!modem_ || !modem_->isConnecting()) return;
    switch (modem_->pollConnect()) {
    case ConnectProgress::Connected:
        onModemConnected();
        break;
    case ConnectProgress::Failed:
        shared_state_.addSystemMessage("Could not reach the modem at " + config_.modem.host + ":" +
                                       std::to_string(config_.modem.command_port));
        break;
    default:
        break;
    }
}

void App::onModemConnected() {
    // Set callsign
    if (!config_.station.callsign.empty()) {
        modem_->setCallsign(config_.station.callsign);
    }
    applyModemSettingsToModem();
    applyVoiceSettingsToModem(false);
    // Passive monitoring is opt-in on Skywave and is how this app hears
    // CQ, beacons and broadcast traffic from stations it is not linked
    // to (INTERFACES.md "MONITOR ON|OFF").
    (void)modem_->setMonitorMode(true);
    // STATUS replies are folded into the status snapshot by poll().
    modem_->sendCommand("STATUS");
}

void App::disconnectFromModem() {
    if (modem_) {
        if (voice_local_ptt_) {
            (void)modem_->setVoicePtt(false);
            voice_local_ptt_ = false;
        }
        voice_start_pending_ = false;
        voice_start_inflight_ = false;
        modem_->disconnect();
    }
}

void App::connectToStation(const std::string& callsign) {
    const std::string normalized = normalizeCallsign(callsign);
    if (modem_ && modem_->isConnected() && !normalized.empty()) {
        resetQsyWorkflow(true);

        std::string calling_band = trimSpaces(current_band_);
        if (calling_band.empty()) {
            calling_band = trimSpaces(config_.radio.default_band);
        }
        if (!calling_band.empty()) {
            current_band_ = calling_band;
        }
        returnToCallingFrequency("");

        qsy_initiator_ = true;
        qsy_workflow_peer_ = normalized;
        qsy_reconnect_attempts_ = 0;
        pending_connect_target_ = normalized;
        /* Generic direct connects should stay on the calling frequency.
         * Auto-QSY is only safe when an explicit CQ-response flow arms it. */
        pending_connect_auto_qsy_ = false;

        modem_->connectToStation(normalized);
    }
}

void App::sendChatMessage(const std::string& text) {
    if (!modem_ || modem_->getStatus().link_state != LinkState::Connected) {
        return;
    }

    // Use fragmentation for long messages
    sendFragmentedMessage(text);
}

void App::sendFile(const std::string& path) {
    if (!modem_ || modem_->getStatus().link_state != LinkState::Connected) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Cannot send file: Not connected";
        msg.is_outgoing = false;
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    // Check file exists and size
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Cannot open file: " + path;
        msg.is_outgoing = false;
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    std::streampos end_pos = file.tellg();
    if (end_pos < 0) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Cannot read file size: " + path;
        msg.is_outgoing = false;
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }
    size_t file_size = static_cast<size_t>(end_pos);
    file.close();

    // Reject empty files to avoid zero-length transfer edge cases.
    if (file_size == 0) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Cannot send empty file: " + path;
        msg.is_outgoing = false;
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    // Check size limit (50 KB)
    if (file_size > 50 * 1024) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "File too large (max 50 KB): " + std::to_string(file_size) + " bytes";
        msg.is_outgoing = false;
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    std::string remote_callsign = current_chat_partner_;
    if (remote_callsign.empty()) {
        remote_callsign = modem_->getStatus().remote_callsign;
    }
    if (remote_callsign.empty()) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Cannot send file: No connected remote station callsign";
        msg.is_outgoing = false;
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    // Extract filename from path
    std::string filename = path;
    size_t slash_pos = path.find_last_of("/\\");
    if (slash_pos != std::string::npos) {
        filename = path.substr(slash_pos + 1);
    }

    if (!file_transfer_mgr_) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "File transfer subsystem not initialized";
        msg.is_outgoing = false;
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    std::string transfer_id = file_transfer_mgr_->sendFile(*modem_, remote_callsign, path);
    if (transfer_id.empty()) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Failed to start file transfer: " + filename;
        msg.is_outgoing = false;
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }
    syncTransfersFromManager();

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "Starting file transfer: " + filename + " (" + std::to_string(file_size) +
               " bytes), id=" + transfer_id;
    msg.is_outgoing = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::updatePosition(double lat, double lon) {
    config_.station.latitude = lat;
    config_.station.longitude = lon;

    if (aprs_) {
        aprs_->setMyPosition(lat, lon);
        aprs_->setMyComment(position_comment_);
    }
}

void App::refreshMessages() {
    if (!store_) return;

    inbox_messages_ = store_->getInboxMessages(config_.station.callsign);
    outbox_messages_ = store_->getOutboxMessages(config_.station.callsign);
}

void App::sendPendingMessages() {
    if (!modem_ || !store_) return;
    if (modem_->getStatus().link_state != LinkState::Connected) return;
    // An empty partner would match every pending message in the store.
    if (normalizeCallsign(current_chat_partner_).empty() ||
        normalizeCallsign(current_chat_partner_) == "UNKNOWN") return;

    // Get pending messages for the connected station
    auto pending = store_->getPendingMessages(current_chat_partner_);

    for (auto& msg : pending) {
        if (msg.type != MessageType::Private &&
            msg.type != MessageType::Bulletin &&
            msg.type != MessageType::Email) {
            continue;
        }

        // Mail composed in this session is owned by updateReliableMail()
        // (send, ACK timeout, retry); sending it here too delivers it twice.
        const bool tracked = std::any_of(
            mail_outbox_.begin(), mail_outbox_.end(), [&](const MailMessage& m) {
                return (msg.id > 0 && m.store_id == msg.id) ||
                       (!msg.message_id.empty() && m.message_id == msg.message_id);
            });
        if (tracked) {
            continue;
        }

        if (msg.message_id.empty()) {
            msg.message_id = MessageStore::generateMessageId(
                normalizeCallsign(config_.station.callsign));
            if (msg.message_id.size() > kMailMaxMessageIdLen) {
                msg.message_id.resize(kMailMaxMessageIdLen);
            }
            if (msg.id > 0) {
                (void)store_->updateMessage(msg);
            }
        }

        MailMessage mail = makeUiMailMessage(msg, true);
        if (mail.from.empty()) {
            mail.from = normalizeCallsign(config_.station.callsign);
        }
        if (mail.to.empty()) {
            mail.to = normalizeCallsign(current_chat_partner_);
        }
        if (mail.message_id.empty()) {
            continue;
        }

        if (!sendMailWirePacket(mail)) {
            continue;
        }

        if (msg.id > 0) {
            store_->markAsSent(msg.id);
        }

        // Hand the message to updateReliableMail() so it is ACK-tracked
        // and retried like mail composed in this session.
        if (!mail.is_broadcast) {
            mail.status = MessageStatus::Sent;
            mail.timestamp = unixNowSeconds();
            mail_outbox_.push_back(mail);
        }

        ChatMessage chat_msg;
        chat_msg.from = "SYSTEM";
        chat_msg.text = "Sent pending message: " + mail.subject;
        chat_msg.is_outgoing = false;
        chat_msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(chat_msg);
        chat_scroll_to_bottom_ = true;
        break;
    }
}

bool App::isLinkConnected() const {
    return modem_ && modem_->getStatus().link_state == LinkState::Connected;
}

std::string App::linkedPeerCallsign() const {
    if (!isLinkConnected()) {
        return "";
    }
    // Same attribution processIncomingMessage() uses for link traffic.
    std::string peer = normalizeCallsign(current_chat_partner_);
    if (peer.empty() || peer == "UNKNOWN") {
        peer = normalizeCallsign(modem_->getStatus().remote_callsign);
    }
    return peer == "UNKNOWN" ? std::string() : peer;
}

void App::processIncomingMessage(const std::vector<uint8_t>& data, const std::string& from_override,
                                 RxOrigin origin, int relay_depth, const std::string& relayed_by) {
    if (data.empty()) return;

    std::string sender = normalizeCallsign(from_override.empty() ? current_chat_partner_ : from_override);
    if (sender.empty() || sender == "?") {  // the modem names unknown senders "?"
        sender = "UNKNOWN";
    }

    // Check for special message types
    uint8_t msg_type = data[0];

    // Session-only types act on the current link; only the connected peer,
    // over the link itself, may send them.
    const bool from_linked_peer = origin == RxOrigin::Link && isLinkConnected();
    if (app_detail::isSessionOnlyPayload(msg_type) && !from_linked_peer) {
        std::cerr << "[RX] Ignoring session-only packet 0x" << std::hex << static_cast<int>(msg_type)
                  << std::dec << " from " << sender << " (not the linked peer)\n";
        return;
    }
    // A relay delivers chat/mail only, and never another relay packet.
    if (origin == RxOrigin::Mesh && !app_detail::isMeshDeliverablePayload(msg_type)) {
        return;
    }

    // Mesh relay packet (0xE5): enforce TTL/hop/dedup policy before payload handling.
    if (msg_type == RELAY_PACKET_MARKER) {
        if (!mesh_relay_ || relay_depth >= 1) {
            return;
        }
        RelayHeader relay_header;
        size_t relay_header_size = 0;
        std::string relay_source = sender;
        if (MeshRelay::deserializeHeader(data.data(), data.size(), relay_header, relay_header_size)) {
            const std::string parsed_source = normalizeCallsign(relay_header.source);
            if (!parsed_source.empty()) {
                relay_source = parsed_source;
            }
        }

        std::vector<uint8_t> relay_payload;
        const auto relay_result = mesh_relay_->processRelayPacket(data, relay_payload);
        if (relay_result == RelayResult::ForUs && !relay_payload.empty() &&
            app_detail::isMeshDeliverablePayload(relay_payload[0])) {
            // Attributed to the header's source, which cannot be verified;
            // the station that relayed it (sender) is recorded alongside.
            processIncomingMessage(relay_payload, relay_source, RxOrigin::Mesh, relay_depth + 1, sender);
        }
        flushMeshRelayQueue();
        return;
    }

    // File transfer protocol messages (0x10-0x18).
    if (file_transfer_mgr_ && modem_ &&
        msg_type >= static_cast<uint8_t>(FileMessageType::FileOffer) &&
        msg_type <= static_cast<uint8_t>(FileMessageType::FileResume)) {
        file_transfer_mgr_->processIncomingData(*modem_, data);
        syncTransfersFromManager();
        return;
    }

    // Typing indicator (0xFE prefix)
    if (msg_type == 0xFE && data.size() == 1) {
        processTypingIndicator(sender);
        return;
    }

    // Fragmented message (0xFD prefix: 0xFD | frag_idx | total_frags | data...)
    if (msg_type == 0xFD && data.size() >= 3) {
        processMessageFragment(sender, data);
        return;
    }

    // CQ call (0xFC prefix)
    if (msg_type == 0xFC) {
        std::string cq_from = sender;
        bool sender_is_hash = isLikelyHashId(cq_from);

        // New CQ payload format is "CQ <CALLSIGN>".
        if ((cq_from == "UNKNOWN" || sender_is_hash) &&
            data.size() > 4 && data[1] == 'C' && data[2] == 'Q' && data[3] == ' ') {
            std::string candidate(data.begin() + 4, data.end());
            size_t end = candidate.find(' ');
            if (end != std::string::npos) {
                candidate.resize(end);
            }
            if (!candidate.empty()) {
                cq_from = normalizeCallsign(candidate);
            }
        }

        cq_from = normalizeCallsign(cq_from);
        addHeardStation(cq_from);
        processCQResponse(cq_from);
        return;
    }

    // Broadcast message (0xFB prefix: 0xFB | message...)
    if (msg_type == 0xFB && data.size() > 1) {
        std::string broadcast_text(data.begin() + 1, data.end());
        if (relayed_by.empty()) {
            addHeardStation(sender);  // a relayed source was not heard
        }

        ChatMessage msg;
        msg.from = sender;
        msg.to = kMailBroadcastTarget;
        msg.via = relayed_by;
        msg.text = app_detail::relayAttributionMarker(relayed_by) + "[BROADCAST] " + broadcast_text;
        msg.is_outgoing = false;
        msg.is_broadcast = true;
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    // Advanced Beacon (0xFA prefix: 0xFA | snr_byte | grid_len | grid_square | flags | message...)
    if (msg_type == 0xFA && data.size() >= 3) {
        float snr = static_cast<float>(static_cast<int8_t>(data[1]));  // SNR as signed byte
        uint8_t grid_len = data[2];
        std::string grid_square;
        bool is_email_gateway = false;

        size_t msg_start = 3;
        if (grid_len > 0 && data.size() >= 3 + grid_len) {
            grid_square = std::string(data.begin() + 3, data.begin() + 3 + grid_len);
            msg_start = 3 + grid_len;
        }

        // Parse flags byte (added after grid square)
        if (data.size() > msg_start) {
            uint8_t flags = data[msg_start];
            is_email_gateway = (flags & 0x01) != 0;
            msg_start++;
        }

        std::string beacon_msg;
        if (data.size() > msg_start) {
            beacon_msg = std::string(data.begin() + msg_start, data.end());
        }
        std::string beacon_from = normalizeCallsign(sender);
        std::string beacon_body = beacon_msg;
        if (beacon_from.empty() || beacon_from == "UNKNOWN" || isLikelyHashId(beacon_from)) {
            auto parsed = extractCallsignPrefix(beacon_msg);
            if (!parsed.first.empty()) {
                beacon_from = parsed.first;
                beacon_body = parsed.second;
            }
        }
        auto prefixed = extractCallsignPrefix(beacon_body);
        if (!prefixed.first.empty() && prefixed.first == beacon_from) {
            beacon_body = prefixed.second;
        }
        if (stripBeaconCapability(beacon_body, kBeaconEmailGatewayMarker)) {
            is_email_gateway = true;
        }
        if (beacon_from.empty()) {
            beacon_from = "UNKNOWN";
        }
        processReceivedBeacon(beacon_from, grid_square, beacon_body, snr, is_email_gateway);
        return;
    }

    // Position relay ACK (0xF3: 0xF3 | status(1) | message...)
    if (msg_type == kPositionRelayAckMarker && data.size() >= 2) {
        const uint8_t relay_status = data[1];
        std::string relay_msg;
        if (data.size() > 2) {
            relay_msg.assign(data.begin() + 2, data.end());
        }

        ChatMessage msg;
        msg.from = "SYSTEM";
        if (relay_status == kPositionRelayAckStatusRelayed) {
            msg.text = "[APRS relay ACK from " + sender + "] " +
                       (relay_msg.empty() ? "Position relay succeeded" : relay_msg);
        } else {
            msg.text = "[APRS relay ACK from " + sender + "] " +
                       (relay_msg.empty() ? "Position relay unavailable" : relay_msg);
        }
        msg.timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        msg.is_outgoing = false;
        msg.delivered = true;
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    // Position relay request (0xF5 prefix: 0xF5 | lat(8) | lon(8) | grid_len | grid | comment)
    // This is sent by a connected station asking us to relay their position to APRS-IS
    if (msg_type == 0xF5 && data.size() >= 18) {
        double lat = readDoubleLE(data.data(), 1);
        double lon = readDoubleLE(data.data(), 9);

        std::string grid_square;
        std::string comment;
        size_t offset = 17;
        if (data.size() > offset) {
            uint8_t grid_len = data[offset];
            offset++;
            if (data.size() >= offset + grid_len) {
                grid_square = std::string(data.begin() + offset, data.begin() + offset + grid_len);
                offset += grid_len;
            }
            if (data.size() > offset) {
                comment = std::string(data.begin() + offset, data.end());
            }
        }
        // Remote text goes onto our APRS-IS session: no CR/LF/control
        // characters, and coordinates must be real.
        grid_square = app_detail::sanitizeRemoteText(grid_square, 10);
        comment = app_detail::sanitizeRemoteText(comment, 200);
        if (!app_detail::isValidLatLon(lat, lon)) {
            appendSystemChat("Position relay request from " + sender + " ignored: invalid coordinates");
            return;
        }

        recordStationLocation(sender,
                              true,
                              lat,
                              lon,
                              grid_square,
                              comment,
                              StationLocationSource::DirectPosition,
                              0.0f,
                              false);

        bool relay_ok = false;
        std::string relay_status_text;

        // If we have APRS-IS connection, relay the position
        if (sender != "UNKNOWN" && aprs_ && aprs_->getState() == APRSState::Connected) {
            relay_ok = aprs_->sendPositionFor(sender, lat, lon, grid_square, comment);
            if (relay_ok) {
                relay_status_text = "Position relayed to APRS-IS";
            } else {
                relay_status_text = "APRS-IS relay transmit failed";
            }

            ChatMessage msg;
            msg.from = "SYSTEM";
            msg.text = relay_ok
                ? ("Relayed position for " + sender + " to APRS-IS")
                : ("Failed to relay position for " + sender + " to APRS-IS");
            msg.timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            msg.is_outgoing = false;
            msg.delivered = true;
            chat_history_.push_back(msg);
        } else {
            relay_status_text = "Position received (relay station has no APRS-IS connection)";
            ChatMessage msg;
            msg.from = "SYSTEM";
            msg.text = "Position relay request from " + sender + " (no APRS-IS connection)";
            msg.timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            msg.is_outgoing = false;
            msg.delivered = true;
            chat_history_.push_back(msg);
        }

        // Send explicit ACK back to the connected station that requested relay.
        if (modem_) {
            std::vector<uint8_t> ack_packet;
            ack_packet.push_back(kPositionRelayAckMarker);
            ack_packet.push_back(relay_ok ? kPositionRelayAckStatusRelayed
                                          : kPositionRelayAckStatusReceivedOnly);

            if (relay_status_text.size() > kPositionRelayAckTextMax) {
                relay_status_text.resize(kPositionRelayAckTextMax);
            }
            ack_packet.insert(ack_packet.end(), relay_status_text.begin(), relay_status_text.end());
            modem_->sendData(ack_packet);
        }

        chat_scroll_to_bottom_ = true;
        return;
    }

    // Position beacon (0xF4 prefix: 0xF4 | lat(8) | lon(8) | grid_len | grid | callsign+comment)
    // Received as a broadcast - we can optionally relay to APRS-IS
    if (msg_type == 0xF4 && data.size() >= 18) {
        double lat = readDoubleLE(data.data(), 1);
        double lon = readDoubleLE(data.data(), 9);

        std::string grid_square;
        std::string beacon_text;
        std::string beacon_from = normalizeCallsign(sender);
        size_t offset = 17;
        if (data.size() > offset) {
            uint8_t grid_len = data[offset];
            offset++;
            if (data.size() >= offset + grid_len) {
                grid_square = std::string(data.begin() + offset, data.begin() + offset + grid_len);
                offset += grid_len;
            }
            if (data.size() > offset) {
                beacon_text = std::string(data.begin() + offset, data.end());
            }
        }

        grid_square = app_detail::sanitizeRemoteText(grid_square, 10);
        beacon_text = app_detail::sanitizeRemoteText(beacon_text, 200);
        if (!app_detail::isValidLatLon(lat, lon)) {
            return;
        }

        // The callsign inside the payload is advisory: it may label the
        // beacon locally but is never used to speak for that station.
        bool sender_from_payload = false;
        if (beacon_from.empty() || beacon_from == "UNKNOWN" || isLikelyHashId(beacon_from)) {
            auto parsed = extractCallsignPrefix(beacon_text);
            if (!parsed.first.empty()) {
                beacon_from = parsed.first;
                beacon_text = parsed.second;
                sender_from_payload = true;
            }
        }
        auto prefixed = extractCallsignPrefix(beacon_text);
        if (!prefixed.first.empty() && prefixed.first == beacon_from) {
            beacon_text = prefixed.second;
        }
        const bool legacy_gateway_marker = stripBeaconCapability(beacon_text, kBeaconEmailGatewayMarker);
        if (beacon_from.empty()) {
            beacon_from = "UNKNOWN";
        }

        // Add to heard beacons with position info
        processReceivedBeacon(beacon_from, grid_square, beacon_text, 0.0f, legacy_gateway_marker);
        recordStationLocation(beacon_from,
                              true,
                              lat,
                              lon,
                              grid_square,
                              beacon_text,
                              StationLocationSource::PositionBeacon,
                              0.0f,
                              legacy_gateway_marker);

        // Auto-relay to APRS-IS if connected (only under a modem-reported sender).
        const bool relay_to_aprs = !sender_from_payload && beacon_from != "UNKNOWN" &&
                                   aprs_ && aprs_->getState() == APRSState::Connected;
        if (relay_to_aprs) {
            aprs_->sendPositionFor(beacon_from, lat, lon, grid_square, beacon_text);
        }

        ChatMessage msg;
        msg.from = beacon_from;
        msg.text = "[POSITION] " + grid_square + " (" + std::to_string(lat).substr(0, 8) + ", "
                   + std::to_string(lon).substr(0, 9) + ") " + beacon_text;
        if (relay_to_aprs) {
            msg.text += " [Relayed to APRS]";
        }
        msg.timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        msg.is_outgoing = false;
        msg.delivered = true;
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    // Pending message notification (0xF9 prefix)
    if (msg_type == 0xF9) {
        // Remote station has pending messages for us
        ChatMessage msg;
        msg.from = sender;
        msg.text = "[SYSTEM] Station has pending messages for you";
        msg.is_outgoing = false;
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    // QSY suggestion (0xF2)
    if (msg_type == kQsySuggestMarker) {
        processQsySuggestion(sender, data);
        return;
    }

    // QSY confirmation (0xF1)
    if (msg_type == kQsyConfirmMarker) {
        processQsyConfirm(sender, data);
        return;
    }

    // SNR report (0xF8 prefix: 0xF8 | snr_lo | snr_hi) - SNR * 10 as int16
    if (msg_type == 0xF8 && data.size() >= 3) {
        int16_t snr_val = static_cast<int16_t>(data[1] | (data[2] << 8));
        float snr = static_cast<float>(snr_val) / 10.0f;
        processSNRReport(snr);
        return;
    }

    // Away mode response (0xF7 prefix: 0xF7 | response...)
    if (msg_type == 0xF7 && data.size() > 1) {
        std::string away_response(data.begin() + 1, data.end());

        ChatMessage msg;
        msg.from = sender;
        msg.text = "[AWAY] " + away_response;
        msg.is_outgoing = false;
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    // Away mode request (0xF6 prefix: 0xF6 | command | data...)
    if (msg_type == 0xF6 && data.size() >= 2) {
        processAwayModeRequest(sender, data);
        return;
    }

    // Ping request (0xE0 prefix: 0xE0 | grid_len | grid_square)
    if (msg_type == 0xE0) {
        processPingRequest(sender, data);
        return;
    }

    // Ping response (0xE1 prefix: 0xE1 | snr_byte | grid_len | grid_square)
    if (msg_type == 0xE1 && data.size() >= 2) {
        processPingResponse(sender, data);
        return;
    }

    // Reliable mail ACK/NACK (0xE8)
    if (msg_type == kMailAckMarker && data.size() >= 4) {
        processMailAck(sender, data);
        return;
    }

    // Reliable private mail (0xE6)
    if (msg_type == kMailPrivateReliableMarker && data.size() >= 6) {
        processReliableMailReceived(sender, data, false, from_linked_peer, relayed_by);
        return;
    }

    // Reliable broadcast mail (0xE7)
    if (msg_type == kMailBroadcastReliableMarker && data.size() >= 5) {
        processReliableMailReceived(sender, data, true, from_linked_peer, relayed_by);
        return;
    }

    // Private mail (0xE2 prefix)
    if (msg_type == 0xE2 && data.size() >= 4) {
        processMailReceived(sender, data, from_linked_peer, relayed_by);
        return;
    }

    // Broadcast mail (0xE3 prefix)
    if (msg_type == 0xE3 && data.size() >= 2) {
        processMailReceived(sender, data, from_linked_peer, relayed_by);
        return;
    }

    // Weather message (0xE4 prefix) - request or response
    if (msg_type == 0xE4 && data.size() >= 2) {
        processWeatherMessage(sender, data);
        return;
    }

    if (msg_type == kBbsAnnounceMarker && data.size() >= 8) {
        processBBSAnnouncement(sender, data);
        return;
    }

    if (msg_type == kBbsRemoveMarker && data.size() >= 3) {
        processBBSRemoval(sender, data);
        return;
    }

    if (msg_type == kBbsFileRequestMarker && data.size() >= 3) {
        processBBSFileRequest(sender, data);
        return;
    }

    // Regular text message
    std::string text(data.begin(), data.end());

    ChatMessage msg;
    msg.from = sender;
    msg.to = config_.station.callsign;
    msg.via = relayed_by;
    msg.text = app_detail::relayAttributionMarker(relayed_by) + text;
    msg.is_outgoing = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

// ============================================================================
// Chat Typing Indicator and Fragmentation
// ============================================================================

void App::sendTypingIndicator() {
    // Typing indicators are disabled by design.
    return;
}

void App::processTypingIndicator(const std::string& from) {
    // Typing indicators are disabled by design.
    (void)from;
}

void App::sendFragmentedMessage(const std::string& text) {
    if (!modem_ || modem_->getStatus().link_state != LinkState::Connected) {
        return;
    }

    if (text.size() <= MAX_CHAT_FRAGMENT_SIZE) {
        // No fragmentation needed
        modem_->sendText(text);
    } else {
        // Fragment the message
        size_t total_frags = (text.size() + MAX_CHAT_FRAGMENT_SIZE - 1) / MAX_CHAT_FRAGMENT_SIZE;
        if (total_frags > 255) total_frags = 255;  // Max fragments

        for (size_t i = 0; i < total_frags; ++i) {
            size_t start = i * MAX_CHAT_FRAGMENT_SIZE;
            size_t len = std::min(MAX_CHAT_FRAGMENT_SIZE, text.size() - start);
            std::string frag = text.substr(start, len);

            // Build fragment packet: 0xFD | frag_idx | total_frags | data
            std::vector<uint8_t> packet;
            packet.push_back(0xFD);
            packet.push_back(static_cast<uint8_t>(i));
            packet.push_back(static_cast<uint8_t>(total_frags));
            packet.insert(packet.end(), frag.begin(), frag.end());

            modem_->sendData(packet);
        }
    }

    // Add to chat history
    ChatMessage msg;
    msg.from = config_.station.callsign;
    msg.to = current_chat_partner_;
    msg.text = text;
    msg.is_outgoing = true;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::processMessageFragment(const std::string& from, const std::vector<uint8_t>& data) {
    if (data.size() < 4) return;  // Too short

    const uint8_t frag_idx = data[1];
    const uint8_t total_frags = data[2];
    const std::string frag_data(data.begin() + 3, data.end());

    // Fragments from an unattributed sender ("UNKNOWN") are refused by the
    // assembler so unrelated stations' fragments never mix.
    std::string full_message;
    if (!fragment_assembler_.add(normalizeCallsign(from), frag_idx, total_frags, frag_data,
                                 unixNowSeconds(), &full_message)) {
        return;
    }

    ChatMessage msg;
    msg.from = from;
    msg.to = config_.station.callsign;
    msg.text = full_message;
    msg.is_outgoing = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::pruneFragmentBuffers() {
    fragment_assembler_.expire(unixNowSeconds());
}

// ============================================================================
// CQ and Broadcast
// ============================================================================

void App::startCQ() {
    cq_active_ = true;
    cq_current_repeat_ = 0;
    last_cq_sent_ = std::chrono::steady_clock::now() - std::chrono::milliseconds(CQ_INTERVAL_MS);

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "Starting CQ...";
    msg.is_outgoing = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::stopCQ() {
    cq_active_ = false;

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "CQ stopped";
    msg.is_outgoing = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::updateCQ() {
    if (!cq_active_ || !modem_) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_cq_sent_);

    if (elapsed.count() >= CQ_INTERVAL_MS && cq_current_repeat_ < cq_repeat_count_) {
        modem_->sendCq();

        last_cq_sent_ = now;
        cq_current_repeat_++;

        if (cq_current_repeat_ >= cq_repeat_count_) {
            cq_active_ = false;
            ChatMessage msg;
            msg.from = "SYSTEM";
            msg.text = "CQ complete (" + std::to_string(cq_repeat_count_) + " calls sent)";
            msg.is_outgoing = false;
            msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            chat_history_.push_back(msg);
            chat_scroll_to_bottom_ = true;
        }
    }
}

bool App::sendBroadcastPayload(const std::vector<uint8_t>& payload) {
    if (!modem_ || payload.empty()) {
        return false;
    }
    if (modem_->getStatus().link_state == LinkState::Connected) {
        return modem_->sendData(payload);
    }
    return modem_->sendBroadcastData(payload);
}

void App::sendBroadcast(const std::string& message) {
    if (!modem_) return;

    std::vector<uint8_t> packet;
    packet.push_back(0xFB);  // Broadcast marker
    packet.insert(packet.end(), message.begin(), message.end());

    if (!sendBroadcastPayload(packet)) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Broadcast send failed";
        msg.is_outgoing = false;
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    ChatMessage msg;
    msg.from = config_.station.callsign;
    msg.to = kMailBroadcastTarget;
    msg.text = "[BROADCAST] " + message;
    msg.is_outgoing = true;
    msg.is_broadcast = true;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::processCQResponse(const std::string& from) {
    recent_cq_unix_[normalizeCallsign(from)] = unixNowSeconds();
    if (config_.psk_reporter.enabled) {
        float snr = 0.0f;
        if (modem_) {
            snr = modem_->getStatus().snr_db;
        }
        if (snr >= config_.psk_reporter.min_snr) {
            queuePSKReporterSpot(from, "", snr);
        }
    }

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "Heard CQ from " + from;
    msg.is_outgoing = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::sendQsySuggestion(const std::string& band, int slot, uint32_t frequency_hz) {
    if (!modem_ || modem_->getStatus().link_state != LinkState::Connected) {
        return;
    }
    if (!qsy_initiator_) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "QSY request blocked: only the calling station initiator may propose QSY.";
        msg.is_outgoing = false;
        msg.timestamp = unixNowSeconds();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    std::string normalized_band = trimSpaces(band);
    if (normalized_band.empty()) {
        return;
    }
    if (normalized_band.size() > 15) {
        normalized_band.resize(15);
    }

    std::vector<uint8_t> packet;
    packet.push_back(kQsySuggestMarker);
    packet.push_back(static_cast<uint8_t>(normalized_band.size()));
    packet.insert(packet.end(), normalized_band.begin(), normalized_band.end());
    int safe_slot = std::clamp(slot, -120, 120);
    if (safe_slot == 0) {
        safe_slot = 1;
    }

    uint32_t safe_frequency_hz = frequency_hz;
    if (safe_frequency_hz == 0 && freq_mgr_) {
        safe_frequency_hz = static_cast<uint32_t>(
            freq_mgr_->getSlotFrequency(normalized_band, safe_slot));
    }
    if (safe_frequency_hz == 0) {
        return;
    }

    packet.push_back(static_cast<uint8_t>(static_cast<int8_t>(safe_slot)));
    writeUint32LE(packet, safe_frequency_hz);

    if (!modem_->sendData(packet)) {
        return;
    }

    const ModemStatus st = modem_->getStatus();
    const std::string target = normalizeCallsign(
        st.remote_callsign.empty() ? current_chat_partner_ : st.remote_callsign);
    if (!target.empty()) {
        qsy_workflow_peer_ = target;
    }
    qsy_target_band_ = normalized_band;
    qsy_target_slot_ = safe_slot;
    qsy_target_frequency_hz_ = safe_frequency_hz;
    qsy_workflow_state_ = QsyWorkflowState::AwaitingAck;
    qsy_ack_deadline_ = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(
                            std::clamp(config_.radio.qsy_ack_timeout_ms, 2000, 30000));

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "QSY suggestion sent to " + target + ": " +
               normalized_band + " slot " + (safe_slot >= 0 ? "+" : "") + std::to_string(safe_slot) +
               " (" + std::to_string(safe_frequency_hz / 1000.0).substr(0, 8) + " kHz)";
    msg.is_outgoing = false;
    msg.timestamp = unixNowSeconds();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::processQsySuggestion(const std::string& from, const std::vector<uint8_t>& data) {
    if (data.size() < 8) {
        return;
    }
    size_t pos = 1;
    const uint8_t band_len = data[pos++];
    if (band_len == 0 || data.size() < pos + band_len + 1 + 4) {
        return;
    }
    const std::string from_call = normalizeCallsign(from);
    if (!qsy_workflow_peer_.empty() &&
        from_call != normalizeCallsign(qsy_workflow_peer_)) {
        return;
    }

    // Only the connected peer may suggest a QSY (enforced on receive too).
    const std::string linked_peer = linkedPeerCallsign();
    if (linked_peer.empty() || from_call != linked_peer) {
        return;
    }
    const std::string suggested_band(data.begin() + pos, data.begin() + pos + band_len);
    const int suggested_slot = static_cast<int>(static_cast<int8_t>(data[pos + band_len]));
    const uint32_t suggested_freq = readUint32LE(data.data(), pos + band_len + 1);
    if (!qsyTargetInBand(suggested_band, suggested_slot == 0 ? 1 : suggested_slot, suggested_freq)) {
        appendSystemChat("Ignored QSY suggestion from " + from_call + ": target outside band " +
                         app_detail::sanitizeRemoteText(suggested_band, 16));
        return;
    }

    qsy_suggestion_band_.assign(data.begin() + pos, data.begin() + pos + band_len);
    pos += band_len;
    qsy_suggestion_slot_ = static_cast<int>(static_cast<int8_t>(data[pos++]));
    qsy_suggestion_frequency_hz_ = readUint32LE(data.data(), pos);
    if (qsy_suggestion_slot_ == 0) {
        qsy_suggestion_slot_ = 1;
    }
    qsy_suggestion_from_ = from_call;
    qsy_suggestion_pending_ = true;
    qsy_suggestion_received_unix_ = unixNowSeconds();
    qsy_workflow_peer_ = qsy_suggestion_from_;
    qsy_target_band_ = qsy_suggestion_band_;
    qsy_target_slot_ = qsy_suggestion_slot_;
    qsy_target_frequency_hz_ = qsy_suggestion_frequency_hz_;

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = qsy_suggestion_from_ + " suggests QSY to " + qsy_suggestion_band_ +
               " slot " + (qsy_suggestion_slot_ >= 0 ? "+" : "") + std::to_string(qsy_suggestion_slot_) +
               " (" + std::to_string(qsy_suggestion_frequency_hz_ / 1000.0).substr(0, 8) + " kHz)";
    msg.is_outgoing = false;
    msg.timestamp = unixNowSeconds();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;

    if (qsy_initiator_) {
        // Collision protection: only the original calling station should initiate QSY.
        respondToQsySuggestion(false);
        return;
    }

    if (config_.radio.auto_qsy) {
        respondToQsySuggestion(true);
    }
}

void App::processQsyConfirm(const std::string& from, const std::vector<uint8_t>& data) {
    if (data.size() < 9) {
        return;
    }
    size_t pos = 1;
    const uint8_t band_len = data[pos++];
    if (band_len == 0 || data.size() < pos + band_len + 1 + 4 + 1) {
        return;
    }
    std::string band(data.begin() + pos, data.begin() + pos + band_len);
    pos += band_len;
    const int slot = static_cast<int>(static_cast<int8_t>(data[pos++]));
    const uint32_t freq_hz = readUint32LE(data.data(), pos);
    pos += 4;
    const bool accepted = data[pos] == kQsyConfirmAccepted;
    const std::string from_call = normalizeCallsign(from);

    if (!qsy_workflow_peer_.empty() &&
        from_call != normalizeCallsign(qsy_workflow_peer_)) {
        return;
    }
    if (accepted && !qsyTargetInBand(band, slot == 0 ? 1 : slot, freq_hz)) {
        appendSystemChat("Ignored QSY confirmation from " + from_call + ": target outside band");
        return;
    }
    qsy_workflow_peer_ = from_call;
    qsy_target_band_ = band;
    qsy_target_slot_ = (slot == 0 ? 1 : slot);
    qsy_target_frequency_hz_ = freq_hz;

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = from_call + std::string(accepted ? " accepted " : " declined ") +
               "QSY to " + band + " slot " +
               (qsy_target_slot_ >= 0 ? "+" : "") + std::to_string(qsy_target_slot_) +
               " (" + std::to_string(freq_hz / 1000.0).substr(0, 8) + " kHz)";
    msg.is_outgoing = false;
    msg.timestamp = unixNowSeconds();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;

    if (!accepted) {
        qsy_workflow_state_ = QsyWorkflowState::Idle;
        returnToCallingFrequency("QSY declined; staying on calling frequency");
        return;
    }

    if (qsy_workflow_state_ == QsyWorkflowState::AwaitingAck) {
        executeQsyTarget(qsy_target_band_, qsy_target_slot_, qsy_target_frequency_hz_);
        qsy_workflow_state_ = QsyWorkflowState::SidebandActive;
        qsy_sideband_started_ = std::chrono::steady_clock::now();
    }
}

void App::respondToQsySuggestion(bool accept) {
    if (!qsy_suggestion_pending_ || !modem_ || modem_->getStatus().link_state != LinkState::Connected) {
        return;
    }
    qsy_workflow_peer_ = normalizeCallsign(qsy_suggestion_from_);
    qsy_target_band_ = qsy_suggestion_band_;
    qsy_target_slot_ = (qsy_suggestion_slot_ == 0 ? 1 : qsy_suggestion_slot_);
    qsy_target_frequency_hz_ = qsy_suggestion_frequency_hz_;

    std::vector<uint8_t> packet;
    packet.push_back(kQsyConfirmMarker);
    packet.push_back(static_cast<uint8_t>(qsy_suggestion_band_.size()));
    packet.insert(packet.end(), qsy_suggestion_band_.begin(), qsy_suggestion_band_.end());
    packet.push_back(static_cast<uint8_t>(static_cast<int8_t>(std::clamp(qsy_target_slot_, -120, 120))));
    writeUint32LE(packet, qsy_target_frequency_hz_);
    packet.push_back(accept ? kQsyConfirmAccepted : kQsyConfirmDeclined);
    if (!modem_->sendData(packet)) {
        return;
    }

    if (accept) {
        executeQsyTarget(qsy_target_band_, qsy_target_slot_, qsy_target_frequency_hz_);
        qsy_workflow_state_ = QsyWorkflowState::SidebandActive;
        qsy_sideband_started_ = std::chrono::steady_clock::now();
    } else {
        qsy_workflow_state_ = QsyWorkflowState::Idle;
    }

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = std::string(accept ? "Accepted" : "Declined") +
               " QSY suggestion from " + qsy_suggestion_from_;
    msg.is_outgoing = false;
    msg.timestamp = unixNowSeconds();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;

    qsy_suggestion_pending_ = false;
    qsy_suggestion_from_.clear();
    qsy_suggestion_band_.clear();
    qsy_suggestion_slot_ = 0;
    qsy_suggestion_frequency_hz_ = 0;
    qsy_suggestion_received_unix_ = 0;
}

void App::updateQsyWorkflow() {
    if (!modem_ || !modem_->isConnected()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const LinkState link_state = modem_->getStatus().link_state;

    if (qsy_suggestion_pending_ && qsy_suggestion_received_unix_ > 0) {
        const int64_t age_sec = unixNowSeconds() - qsy_suggestion_received_unix_;
        if (age_sec > 90) {
            qsy_suggestion_pending_ = false;
            qsy_suggestion_from_.clear();
            qsy_suggestion_band_.clear();
            qsy_suggestion_slot_ = 0;
            qsy_suggestion_frequency_hz_ = 0;
            qsy_suggestion_received_unix_ = 0;
        }
    }

    if (qsy_workflow_state_ == QsyWorkflowState::AwaitingAck) {
        if (link_state != LinkState::Connected) {
            return;
        }
        if (now < qsy_ack_deadline_) {
            return;
        }

        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "QSY ACK timeout. Returning to calling frequency and retrying connect.";
        msg.is_outgoing = false;
        msg.timestamp = unixNowSeconds();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;

        returnToCallingFrequency("");
        qsy_workflow_state_ = QsyWorkflowState::ReconnectPending;
        (void)modem_->disconnectStation();
        return;
    }

    if (qsy_workflow_state_ == QsyWorkflowState::ReconnectPending) {
        if (link_state != LinkState::Idle) {
            return;
        }
        if (now < qsy_reconnect_due_) {
            return;
        }
        if (qsy_workflow_peer_.empty()) {
            resetQsyWorkflow(true);
            return;
        }

        returnToCallingFrequency("");
        pending_connect_target_ = qsy_workflow_peer_;
        qsy_workflow_state_ = QsyWorkflowState::ReconnectConnecting;
        qsy_connect_attempt_started_ = now;
        modem_->connectToStation(qsy_workflow_peer_);

        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Reconnecting to " + qsy_workflow_peer_ + " on calling frequency...";
        msg.is_outgoing = false;
        msg.timestamp = unixNowSeconds();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    if (qsy_workflow_state_ == QsyWorkflowState::ReconnectConnecting) {
        if (link_state == LinkState::Connected) {
            return;
        }
        const int connect_timeout_ms = std::clamp(config_.radio.qsy_connect_timeout_ms, 2000, 45000);
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                now - qsy_connect_attempt_started_).count() < connect_timeout_ms) {
            return;
        }

        const int max_attempts = std::clamp(config_.radio.qsy_max_reconnect_attempts, 1, 10);
        if (qsy_reconnect_attempts_ >= max_attempts) {
            ChatMessage msg;
            msg.from = "SYSTEM";
            msg.text = "QSY recovery failed after " + std::to_string(max_attempts) +
                       " attempts. Disconnecting workflow.";
            msg.is_outgoing = false;
            msg.timestamp = unixNowSeconds();
            chat_history_.push_back(msg);
            chat_scroll_to_bottom_ = true;
            resetQsyWorkflow(true);
            return;
        }

        qsy_reconnect_attempts_++;
        const int delay_ms = std::clamp(config_.radio.qsy_reconnect_delay_ms, 500, 15000);
        qsy_workflow_state_ = QsyWorkflowState::ReconnectPending;
        qsy_reconnect_due_ = now + std::chrono::milliseconds(delay_ms);
        (void)modem_->disconnectStation();

        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Reconnect attempt timed out. Scheduling retry " +
                   std::to_string(qsy_reconnect_attempts_) + "/" +
                   std::to_string(max_attempts) + ".";
        msg.is_outgoing = false;
        msg.timestamp = unixNowSeconds();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
    }
}

void App::resetQsyWorkflow(bool clear_peer) {
    qsy_workflow_state_ = QsyWorkflowState::Idle;
    if (clear_peer) {
        qsy_initiator_ = false;
        qsy_workflow_peer_.clear();
        pending_connect_target_.clear();
        qsy_reconnect_attempts_ = 0;
    }
    qsy_target_band_.clear();
    qsy_target_slot_ = 0;
    qsy_target_frequency_hz_ = 0;
    qsy_ack_deadline_ = std::chrono::steady_clock::time_point{};
    qsy_sideband_started_ = std::chrono::steady_clock::time_point{};
    qsy_reconnect_due_ = std::chrono::steady_clock::time_point{};
    qsy_connect_attempt_started_ = std::chrono::steady_clock::time_point{};
    qsy_suggestion_pending_ = false;
    qsy_suggestion_from_.clear();
    qsy_suggestion_band_.clear();
    qsy_suggestion_slot_ = 0;
    qsy_suggestion_frequency_hz_ = 0;
    qsy_suggestion_received_unix_ = 0;
}

bool App::qsyTargetInBand(const std::string& band, int slot, uint32_t frequency_hz) const {
    if (!freq_mgr_) {
        return false;
    }
    const auto band_cfg = freq_mgr_->getBand(trimSpaces(band));
    if (!band_cfg) {
        return false;
    }
    auto inside = [&](uint64_t f) {
        return f >= band_cfg->band_start_hz && f <= band_cfg->band_end_hz;
    };
    if (frequency_hz != 0 && !inside(frequency_hz)) {
        return false;
    }
    const uint64_t slot_freq = freq_mgr_->getSlotFrequency(band_cfg->name, slot);
    return slot_freq != 0 ? inside(slot_freq) : frequency_hz != 0;
}

bool App::executeQsyTarget(const std::string& band, int slot, uint32_t frequency_hz) {
    if (!modem_ || !modem_->isConnected()) {
        return false;
    }

    std::string target_band = trimSpaces(band);
    if (target_band.empty()) {
        target_band = trimSpaces(current_band_);
    }
    if (target_band.empty()) {
        target_band = trimSpaces(config_.radio.default_band);
    }
    if (target_band.empty()) {
        return false;
    }

    int target_slot = std::clamp(slot, -120, 120);
    if (target_slot == 0) {
        target_slot = 1;
    }

    if (!qsyTargetInBand(target_band, target_slot, frequency_hz)) {
        return false;
    }

    bool ok = false;
    if (freq_mgr_) {
        ok = radio_ && freq_mgr_->qsyToSlot(*radio_, target_band, target_slot);
    }
    if (!ok) {
        uint64_t freq = frequency_hz;
        if (freq == 0 && freq_mgr_) {
            freq = freq_mgr_->getSlotFrequency(target_band, target_slot);
        }
        if (freq > 0) {
            ok = tuneOperatingFrequency(freq, false);
        }
    }
    if (ok) {
        current_band_ = target_band;
        current_slot_ = target_slot;
        if (freq_mgr_) {
            const uint64_t slot_freq = freq_mgr_->getSlotFrequency(target_band, target_slot);
            if (slot_freq > 0) {
                current_frequency_hz_ = static_cast<uint32_t>(slot_freq);
            }
        }
    }
    return ok;
}

void App::returnToCallingFrequency(const std::string& reason) {
    if (!modem_ || !modem_->isConnected()) {
        return;
    }

    std::string band = trimSpaces(qsy_target_band_);
    if (band.empty()) {
        band = trimSpaces(current_band_);
    }
    if (band.empty()) {
        band = trimSpaces(config_.radio.default_band);
    }
    if (band.empty()) {
        band = "40m";
    }

    bool ok = false;
    uint64_t calling_freq = 0;
    if (freq_mgr_) {
        calling_freq = freq_mgr_->getCallingFrequency(band);
        ok = radio_ && freq_mgr_->qsyToCallingFrequency(*radio_, band);
    }
    if (!ok && calling_freq > 0) {
        ok = tuneOperatingFrequency(calling_freq, false);
    }
    if (!ok) {
        return;
    }

    current_band_ = band;
    current_slot_ = 0;
    if (calling_freq > 0) {
        current_frequency_hz_ = static_cast<uint32_t>(calling_freq);
    }

    if (!reason.empty()) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        if (calling_freq > 0) {
            msg.text = reason + " (" + std::to_string(calling_freq / 1000000.0).substr(0, 7) + " MHz)";
        } else {
            msg.text = reason;
        }
        msg.is_outgoing = false;
        msg.timestamp = unixNowSeconds();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
    }
}

bool App::buildAutoQsyTarget(std::string* band_out, int* slot_out, uint32_t* freq_hz_out) const {
    if (!band_out || !slot_out || !freq_hz_out || !freq_mgr_) {
        return false;
    }

    std::string band = trimSpaces(current_band_);
    if (band.empty()) {
        band = trimSpaces(config_.radio.default_band);
    }
    if (band.empty()) {
        band = "40m";
    }

    int slot = freq_mgr_->findBestSlot(band);
    if (slot == 0) {
        slot = 1;
    }
    uint64_t freq = freq_mgr_->getSlotFrequency(band, slot);
    if (freq == 0) {
        slot = -1;
        freq = freq_mgr_->getSlotFrequency(band, slot);
    }
    if (freq == 0) {
        return false;
    }

    *band_out = band;
    *slot_out = slot;
    *freq_hz_out = static_cast<uint32_t>(freq);
    return true;
}

void App::addHeardStation(const std::string& callsign) {
    const std::string normalized = normalizeCallsign(callsign);
    if (normalized.empty() || normalized == "UNKNOWN" || isLikelyHashId(normalized)) {
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    char time_str[32];
    std::strftime(time_str, sizeof(time_str), "%H:%M:%S", std::localtime(&time_t));

    // Remove if already in list
    heard_stations_.erase(
        std::remove_if(heard_stations_.begin(), heard_stations_.end(),
                       [&](const auto& p) { return p.first == normalized; }),
        heard_stations_.end());

    // Add to front
    heard_stations_.push_front({normalized, time_str});

    // Limit size
    while (heard_stations_.size() > MAX_HEARD_STATIONS) {
        heard_stations_.pop_back();
    }
}

bool App::tuneOperatingFrequency(uint64_t freq_hz, bool update_selection) {
    if (!radio_ || !radio_->isConnected() || freq_hz == 0) {
        return false;
    }
    if (!radio_->setFrequency(freq_hz)) {
        return false;
    }

    if (freq_hz <= std::numeric_limits<uint32_t>::max()) {
        current_frequency_hz_ = static_cast<uint32_t>(freq_hz);
    }

    if (update_selection && freq_mgr_) {
        const auto band = freq_mgr_->frequencyToBand(freq_hz);
        if (band) {
            current_band_ = *band;
            current_slot_ = freq_mgr_->frequencyToSlot(*band, freq_hz);
        }
    }
    return true;
}

void App::applyRadioConfig() {
    if (!radio_) {
        return;
    }

    radio_->stopPttServer();
    radio_->disconnect();
    config_.radio.backend = normalizeRadioBackend(config_.radio.backend);
    radio_->applyConfig(materializeRadioConfigForController(config_.radio));

    if (config_.radio.ptt_server_enabled) {
        (void)radio_->startPttServer();
    }
    if (normalizeRadioBackend(config_.radio.backend) != "none" &&
        config_.radio.auto_connect) {
        (void)radio_->connect();
    }
}

// ============================================================================
// Channel Sniffer
// ============================================================================

void App::startChannelSniffer() {
    if (!modem_ || !freq_mgr_) return;

    channel_sniffer_active_ = true;
    sniffer_current_slot_ = 0;
    slot_status_.clear();

    // Scan the currently selected band.
    sniffer_band_ = current_band_.empty() ? std::string("40m") : current_band_;

    // Initialize slot status
    auto slots = freq_mgr_->getSlots(sniffer_band_);
    for (const auto& slot : slots) {
        slot_status_.push_back({slot.slot_number, false});
    }
    if (slots.empty()) {
        channel_sniffer_active_ = false;
        return;
    }

    // Set to first slot
    if (!tuneOperatingFrequency(slots[0].frequency_hz)) {
        channel_sniffer_active_ = false;
        return;
    }

    sniffer_slot_start_ = std::chrono::steady_clock::now();

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "Channel sniffer started on " + sniffer_band_;
    msg.is_outgoing = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::stopChannelSniffer() {
    channel_sniffer_active_ = false;

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "Channel sniffer stopped";
    msg.is_outgoing = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::updateChannelSniffer() {
    if (!channel_sniffer_active_ || !modem_ || !freq_mgr_) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - sniffer_slot_start_);

    if (elapsed.count() >= SNIFFER_DWELL_MS) {
        auto slots = freq_mgr_->getSlots(sniffer_band_);
        if (slots.empty()) {
            channel_sniffer_active_ = false;
            return;
        }

        // Record busy status for current slot
        if (sniffer_current_slot_ < static_cast<int>(slot_status_.size())) {
            const bool busy = modem_->getStatus().busy_detected;
            slot_status_[sniffer_current_slot_].second = busy;
            const int slot_number = slot_status_[sniffer_current_slot_].first;
            freq_mgr_->updateSlotStatus(sniffer_band_, slot_number, busy ? SlotStatus::InUse : SlotStatus::Available);
        }

        // Move to next slot
        sniffer_current_slot_++;

        if (sniffer_current_slot_ >= static_cast<int>(slots.size())) {
            // Complete - cycle done
            channel_sniffer_active_ = false;

            // Find best (free) slot. Slot numbers may be negative, so track
            // "found" separately from the value.
            int best_slot = 0;
            bool found_free = false;
            for (const auto& status : slot_status_) {
                if (!status.second) {  // Not busy
                    best_slot = status.first;
                    found_free = true;
                    break;
                }
            }

            ChatMessage msg;
            msg.from = "SYSTEM";
            if (found_free) {
                const uint64_t freq_hz = freq_mgr_->getSlotFrequency(sniffer_band_, best_slot);
                const std::string slot_label = (best_slot >= 0 ? "+" : "") + std::to_string(best_slot);
                msg.text = "Sniffer complete on " + sniffer_band_ + ". Best slot: " + slot_label +
                           " (" + std::to_string(freq_hz / 1000) + " kHz)";
            } else {
                msg.text = "Sniffer complete on " + sniffer_band_ + ". All slots busy!";
            }
            msg.is_outgoing = false;
            msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            chat_history_.push_back(msg);
            chat_scroll_to_bottom_ = true;
        } else {
            // Set frequency to next slot
            if (!tuneOperatingFrequency(slots[sniffer_current_slot_].frequency_hz)) {
                channel_sniffer_active_ = false;
                return;
            }
            sniffer_slot_start_ = now;
        }
    }
}

// ============================================================================
// GPS Serial Input
// ============================================================================

bool App::openGPSPort(const std::string& port, int baud) {
#ifdef _WIN32
    // Windows serial port handling
    std::string port_name = port;
    // Prepend \\.\\ for COM ports > 9
    if (port_name.rfind("COM", 0) == 0 && port_name.find("\\\\.\\") != 0) {
        port_name = "\\\\.\\" + port_name;
    }

    gps_handle_ = CreateFileA(
        port_name.c_str(),
        GENERIC_READ,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (gps_handle_ == INVALID_HANDLE_VALUE) {
        std::cerr << "[GPS] Failed to open port: " << port << "\n";
        gps_handle_ = nullptr;
        return false;
    }

    // Configure serial port
    DCB dcb = {0};
    dcb.DCBlength = sizeof(DCB);

    if (!GetCommState(gps_handle_, &dcb)) {
        CloseHandle(gps_handle_);
        gps_handle_ = nullptr;
        return false;
    }

    dcb.BaudRate = baud;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;

    if (!SetCommState(gps_handle_, &dcb)) {
        CloseHandle(gps_handle_);
        gps_handle_ = nullptr;
        return false;
    }

    // Set timeouts for non-blocking read
    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = 0;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 0;

    if (!SetCommTimeouts(gps_handle_, &timeouts)) {
        CloseHandle(gps_handle_);
        gps_handle_ = nullptr;
        return false;
    }

    gps_port_ = port;
    gps_baud_ = baud;
    return true;
#else
    gps_serial_fd_ = ::open(port.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (gps_serial_fd_ < 0) {
        std::cerr << "[GPS] Failed to open port: " << port << "\n";
        return false;
    }

    struct termios tty;
    if (tcgetattr(gps_serial_fd_, &tty) != 0) {
        close(gps_serial_fd_);
        gps_serial_fd_ = -1;
        return false;
    }

    // Set baud rate
    speed_t speed = B9600;
    switch (baud) {
        case 4800: speed = B4800; break;
        case 9600: speed = B9600; break;
        case 19200: speed = B19200; break;
        case 38400: speed = B38400; break;
        case 115200: speed = B115200; break;
    }
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    // 8N1, no flow control
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;

    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    tty.c_oflag &= ~OPOST;

    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(gps_serial_fd_, TCSANOW, &tty) != 0) {
        close(gps_serial_fd_);
        gps_serial_fd_ = -1;
        return false;
    }

    gps_port_ = port;
    gps_baud_ = baud;
    return true;
#endif
}

void App::closeGPSPort() {
#ifdef _WIN32
    if (gps_handle_ != nullptr) {
        CloseHandle(gps_handle_);
        gps_handle_ = nullptr;
    }
#else
    if (gps_serial_fd_ >= 0) {
        close(gps_serial_fd_);
        gps_serial_fd_ = -1;
    }
#endif
    gps_port_.clear();
    gps_fix_valid_ = false;
}

void App::updateGPS() {
#ifdef _WIN32
    if (gps_handle_ == nullptr) return;

    char buf[256];
    DWORD bytes_read = 0;
    if (ReadFile(gps_handle_, buf, sizeof(buf) - 1, &bytes_read, nullptr) && bytes_read > 0) {
        buf[bytes_read] = '\0';
        gps_buffer_ += buf;
        processGPSData();
    }
#else
    if (gps_serial_fd_ < 0) return;

    char buf[256];
    ssize_t n = read(gps_serial_fd_, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        gps_buffer_ += buf;
        processGPSData();
    }
#endif
}

void App::processGPSData() {
    // Process complete NMEA sentences
    size_t pos;
    while ((pos = gps_buffer_.find('\n')) != std::string::npos) {
        std::string sentence = gps_buffer_.substr(0, pos);
        gps_buffer_.erase(0, pos + 1);

        // Remove CR if present
        if (!sentence.empty() && sentence.back() == '\r') {
            sentence.pop_back();
        }

        if (!sentence.empty() && sentence[0] == '$') {
            parseNMEASentence(sentence);
        }
    }

    // Prevent buffer from growing too large
    if (gps_buffer_.size() > 1024) {
        gps_buffer_.clear();
    }
}

void App::parseNMEASentence(const std::string& sentence) {
    // Parse NMEA sentences
    // $GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,...
    // $GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,...

    if (sentence.substr(0, 6) == "$GPGGA" || sentence.substr(0, 6) == "$GNGGA") {
        // Parse GGA sentence
        std::vector<std::string> fields;
        std::string field;
        for (char c : sentence) {
            if (c == ',' || c == '*') {
                fields.push_back(field);
                field.clear();
            } else {
                field += c;
            }
        }
        if (!field.empty()) fields.push_back(field);

        if (fields.size() >= 10) {
            try {
                // Fix quality (0=invalid, 1=GPS, 2=DGPS)
                int fix_quality = std::stoi(fields[6]);
                gps_fix_valid_ = (fix_quality > 0);

                if (gps_fix_valid_) {
                    // Parse latitude: DDMM.MMMM
                    double lat_raw = std::stod(fields[2]);
                    int lat_deg = static_cast<int>(lat_raw / 100);
                    double lat_min = lat_raw - (lat_deg * 100);
                    double lat = lat_deg + lat_min / 60.0;
                    if (fields[3] == "S") lat = -lat;

                    // Parse longitude: DDDMM.MMMM
                    double lon_raw = std::stod(fields[4]);
                    int lon_deg = static_cast<int>(lon_raw / 100);
                    double lon_min = lon_raw - (lon_deg * 100);
                    double lon = lon_deg + lon_min / 60.0;
                    if (fields[5] == "W") lon = -lon;

                    // Update position
                    my_lat_ = lat;
                    my_lon_ = lon;
                    last_gps_update_ = std::chrono::steady_clock::now();

                    // Update APRS if configured
                    if (aprs_) {
                        aprs_->setMyPosition(lat, lon);
                    }
                }
            } catch (const std::exception&) {
                // Malformed NMEA field - ignore this sentence
                gps_fix_valid_ = false;
            }
        }
    } else if (sentence.substr(0, 6) == "$GPRMC" || sentence.substr(0, 6) == "$GNRMC") {
        // Parse RMC sentence (can also provide position)
        std::vector<std::string> fields;
        std::string field;
        for (char c : sentence) {
            if (c == ',' || c == '*') {
                fields.push_back(field);
                field.clear();
            } else {
                field += c;
            }
        }
        if (!field.empty()) fields.push_back(field);

        if (fields.size() >= 10 && fields[2] == "A") {  // A = Active (valid fix)
            try {
                double lat_raw = std::stod(fields[3]);
                int lat_deg = static_cast<int>(lat_raw / 100);
                double lat_min = lat_raw - (lat_deg * 100);
                double lat = lat_deg + lat_min / 60.0;
                if (fields[4] == "S") lat = -lat;

                double lon_raw = std::stod(fields[5]);
                int lon_deg = static_cast<int>(lon_raw / 100);
                double lon_min = lon_raw - (lon_deg * 100);
                double lon = lon_deg + lon_min / 60.0;
                if (fields[6] == "W") lon = -lon;

                gps_fix_valid_ = true;
                my_lat_ = lat;
                my_lon_ = lon;
                last_gps_update_ = std::chrono::steady_clock::now();

                if (aprs_) {
                    aprs_->setMyPosition(lat, lon);
                }
            } catch (const std::exception&) {
                // Malformed NMEA field - ignore this sentence
                gps_fix_valid_ = false;
            }
        }
    }
}

// ============================================================================
// Email Gateway
// ============================================================================

void App::startEmailGateway() {
    email_gateway_active_ = true;
    last_email_check_ = std::chrono::steady_clock::now();
    last_pop3_check_ = last_email_check_;
    last_winlink_check_ = last_email_check_;
    relay_diagnostics_.gateway_health = "active";
    relay_diagnostics_.queue_policy_state = "idle";
    relay_diagnostics_.last_result = "idle";
    relay_diagnostics_.last_error.clear();

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "Email gateway started";
    msg.is_outgoing = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::stopEmailGateway() {
    email_gateway_active_ = false;
    relay_diagnostics_.gateway_health = "stopped";
    relay_diagnostics_.queue_policy_state = "stopped";
    relay_diagnostics_.last_result = "stopped";

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "Email gateway stopped";
    msg.is_outgoing = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::updateEmailGateway() {
    if (!email_gateway_active_) return;
    // One network job at a time: skip this poll while one is running.
    if (email_worker_.busy()) return;

    auto now = std::chrono::steady_clock::now();
    const int pop3_interval_sec = std::clamp(config_.email.pop3_poll_interval_sec, 30, 3600);
    const int winlink_interval_sec = std::clamp(config_.email.winlink_fetch_interval_sec, 30, 3600);

    bool poll_pop3 = false;
    bool poll_winlink = false;

    if (!trimSpaces(config_.email.pop3_host).empty()) {
        auto pop3_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_pop3_check_);
        poll_pop3 = pop3_elapsed.count() >= pop3_interval_sec;
    }

    if (config_.email.winlink_telnet_enabled &&
        !trimSpaces(config_.email.winlink_telnet_host).empty() &&
        !normalizeCallsign(config_.email.winlink_telnet_callsign).empty() &&
        !trimSpaces(config_.email.winlink_telnet_password).empty()) {
        auto winlink_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_winlink_check_);
        poll_winlink = winlink_elapsed.count() >= winlink_interval_sec;
    }

    if (poll_pop3 || poll_winlink) {
        startEmailGatewayJob(poll_pop3, poll_winlink, true);
        last_email_check_ = now;
        if (poll_pop3) {
            last_pop3_check_ = now;
        }
        if (poll_winlink) {
            last_winlink_check_ = now;
        }
    }
}

// Snapshots the request data on the GUI thread and hands only the network
// calls to the worker; pollEmailGatewayJob() applies the results here.
bool App::startEmailGatewayJob(bool poll_pop3, bool poll_winlink, bool send_outbox) {
    if (email_worker_.busy()) {
        if (send_outbox) email_send_requested_ = true;
        return false;
    }

    EmailGatewayJobInput input;

    if (poll_pop3 && !trimSpaces(config_.email.pop3_host).empty()) {
        input.poll_pop3 = true;
        input.pop3.host = trimSpaces(config_.email.pop3_host);
        input.pop3.port = config_.email.pop3_port;
        input.pop3.use_tls = config_.email.pop3_use_tls;
        input.pop3.username = config_.email.pop3_username;
        input.pop3.password = config_.email.pop3_password;
        input.pop3.delete_after_fetch = true;
        input.pop3.max_messages = std::clamp(config_.email.pop3_max_messages, 1, 500);
    }

    const bool winlink_ready =
        config_.email.winlink_telnet_enabled &&
        !trimSpaces(config_.email.winlink_telnet_host).empty() &&
        !normalizeCallsign(config_.email.winlink_telnet_callsign).empty() &&
        !trimSpaces(config_.email.winlink_telnet_password).empty();
    if (poll_winlink && winlink_ready) {
        input.poll_winlink = true;
        input.winlink_fetch.host = trimSpaces(config_.email.winlink_telnet_host);
        input.winlink_fetch.port = config_.email.winlink_telnet_port;
        input.winlink_fetch.use_tls = config_.email.winlink_telnet_use_tls;
        input.winlink_fetch.callsign = normalizeCallsign(config_.email.winlink_telnet_callsign);
        input.winlink_fetch.password = config_.email.winlink_telnet_password;
        input.winlink_fetch.max_messages = std::clamp(config_.email.winlink_fetch_max_messages, 1, 500);
        input.winlink_fetch.delete_after_fetch = true;
    }

    if (send_outbox) {
        email_send_requested_ = false;
        prepareQueuedEmails(input);
    }

    if (!input.poll_pop3 && !input.poll_winlink && input.outgoing.empty()) {
        return false;
    }
    if (!email_worker_.start(std::move(input))) {
        // Could not start a thread: put the messages back for the next try.
        email_outbox_.insert(email_outbox_.begin(), email_inflight_.begin(), email_inflight_.end());
        email_inflight_.clear();
        return false;
    }
    return true;
}

void App::pollEmailGatewayJob() {
    // Fetched mail is stored before the job may delete the server copies.
    if (auto inbound = email_worker_.takeInbound()) {
        applyInboundEmail(*inbound);
        const bool stored = store_ && store_->isOpen() && radio_outbox_.empty();
        email_worker_.ackInbound(stored);
    }
    auto output = email_worker_.poll();
    if (!output) return;
    applyInboundEmail(*output);
    applyEmailSendResults(*output);
    if (email_send_requested_ && !email_outbox_.empty()) {
        startEmailGatewayJob(false, false, true);
    }
}

void App::processIncomingEmail() {
    if (radio_outbox_.empty()) {
        return;
    }
    // Without a store there is nowhere durable to put the mail; keep it
    // queued (the server copy is already gone) and retry next pass.
    if (!store_ || !store_->isOpen()) {
        return;
    }

    // Process emails received from POP3/Winlink and queue them for radio delivery.
    for (auto& email : radio_outbox_) {
        if (!email.message_id.empty() && store_->getMessageById(email.message_id).has_value()) {
            continue;
        }
        email.type = MessageType::Email;
        email.status = MessageStatus::Pending;
        store_->createMessage(email);
    }
    radio_outbox_.clear();
}

void App::keepInboundLocally(const std::string& from,
                             const std::string& to,
                             const std::string& subject,
                             const std::string& body,
                             const std::string& message_id) {
    const std::string mid = trimSpaces(message_id);
    if (!mid.empty()) {
        const bool seen = std::any_of(mail_inbox_.begin(), mail_inbox_.end(),
                                      [&](const MailMessage& m) { return m.message_id == mid; });
        if (seen || (store_ && store_->isOpen() && store_->getMessageById(mid).has_value())) {
            return;
        }
    }

    MailMessage mail;
    mail.message_id = mid;
    mail.from = trimSpaces(from).empty() ? std::string("EMAIL") : trimSpaces(from);
    mail.to = trimSpaces(to);
    mail.subject = trimSpaces(subject).empty() ? std::string("Internet message") : subject;
    mail.body = body;
    mail.display_body = body;
    mail.timestamp = unixNowSeconds();
    mail.read = false;
    mail.status = MessageStatus::Delivered;
    mail_inbox_.push_back(mail);
    if (store_ && store_->isOpen()) {
        const int64_t id = store_->createMessage(makeStoreMailMessage(mail, config_.station.callsign, false));
        if (id > 0) {
            mail_inbox_.back().store_id = id;
        }
    }
    appendSystemChat("[EMAIL] Kept internet message for " + mail.to +
                     " in the local inbox (no radio callsign to relay to)");
}

void App::relayEmailToRadio(const Message& email) {
    // Convert email to store-and-forward message for radio
    Message radio_msg;
    radio_msg.from_callsign = config_.station.callsign;
    radio_msg.to_callsign = email.to_callsign;  // Should be a callsign
    radio_msg.subject = email.subject;
    radio_msg.body = email.body;
    radio_msg.status = MessageStatus::Pending;
    radio_msg.created_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    if (store_) {
        store_->createMessage(radio_msg);
    }
}

void App::relayRadioToEmail(const Message& msg) {
    // Convert radio message to email and queue for SMTP. The store keeps it
    // queued until a confirmed send, so a quit or crash never drops it.
    if (store_ && store_->isOpen() && msg.id > 0) {
        store_->queueInternetRelay(msg.id);
    }
    email_outbox_.push_back(msg);
}

void App::applyInboundEmail(const EmailGatewayJobOutput& output) {
    auto queueInboundForRadio = [&](const std::string& from,
                                    const std::string& to,
                                    const std::string& subject,
                                    const std::string& body,
                                    const std::string& message_id) -> bool {
        const std::string target_callsign = deriveRadioRelayTarget(to);
        if (target_callsign.empty()) {
            // Not addressed to a radio station: the server copy is gone, so
            // keep it in the local inbox rather than dropping it.
            keepInboundLocally(from, to, subject, body, message_id);
            return false;
        }

        const std::string mid = trimSpaces(message_id);
        if (!mid.empty()) {
            const bool already_queued = std::any_of(radio_outbox_.begin(), radio_outbox_.end(),
                                                    [&](const Message& m) { return m.message_id == mid; });
            if (already_queued) {
                return false;
            }
            if (store_ && store_->isOpen() && store_->getMessageById(mid).has_value()) {
                return false;
            }
        }

        Message msg;
        msg.type = MessageType::Email;
        msg.status = MessageStatus::Pending;
        msg.from_callsign = trimSpaces(from).empty() ? "EMAIL" : trimSpaces(from);
        msg.to_callsign = target_callsign;
        msg.subject = trimSpaces(subject).empty() ? ("Internet message for " + target_callsign)
                                                  : subject;
        msg.body = body;
        msg.message_id = mid;
        msg.created_at = unixNowSeconds();
        radio_outbox_.push_back(std::move(msg));
        return true;
    };

    int queued_for_radio = 0;

    if (output.pop3_ran) {
        const POP3FetchResult& pop_result = output.pop3;
        relay_diagnostics_.last_mode = "pop3-fetch";
        relay_diagnostics_.last_target = output.pop3_target;
        relay_diagnostics_.last_attempt_unix = unixNowSeconds();
        if (!pop_result.success) {
            relay_diagnostics_.last_result = "pop3-fail";
            relay_diagnostics_.last_error = pop_result.error;
        } else {
            relay_diagnostics_.last_result = "pop3-ok";
            relay_diagnostics_.last_error.clear();
            int truncated = 0;
            for (const auto& inbound : pop_result.messages) {
                // Truncated messages stay on the server (no DELE) and are
                // fetched again; relaying the partial body would lose mail.
                if (inbound.truncated) {
                    truncated++;
                    continue;
                }
                if (queueInboundForRadio(inbound.from,
                                         inbound.to,
                                         inbound.subject,
                                         inbound.body,
                                         inbound.message_id)) {
                    queued_for_radio++;
                }
            }
            if (truncated > 0) {
                appendSystemChat("[EMAIL] " + std::to_string(truncated) +
                                 " oversized message(s) left on the POP3 server (not relayed)");
            }
        }
    }

    if (output.winlink_ran) {
        const WinlinkTelnetFetchResult& wl_result = output.winlink;
        relay_diagnostics_.last_mode = wl_result.login_mode_used.empty()
            ? "winlink-fetch"
            : ("winlink-fetch/" + wl_result.login_mode_used);
        relay_diagnostics_.last_target = output.winlink_target;
        relay_diagnostics_.last_attempt_unix = unixNowSeconds();
        relay_diagnostics_.last_login_attempts = wl_result.login_attempts;

        if (!wl_result.success) {
            relay_diagnostics_.winlink_fetch_fail++;
            relay_diagnostics_.last_result = "winlink-fetch-fail";
            relay_diagnostics_.last_error = wl_result.error;
            relay_diagnostics_.gateway_health = "winlink-fetch-fail";
        } else {
            relay_diagnostics_.winlink_fetch_success++;
            relay_diagnostics_.last_result = "winlink-fetch-ok";
            relay_diagnostics_.last_error.clear();
            relay_diagnostics_.gateway_health = "active";
            for (const auto& inbound : wl_result.messages) {
                const std::string subject = trimSpaces(inbound.subject).empty()
                    ? std::string("Winlink inbound message")
                    : inbound.subject;
                const std::string body = inbound.body.empty() ? inbound.raw : inbound.body;
                if (queueInboundForRadio(inbound.from,
                                         inbound.to,
                                         subject,
                                         body,
                                         inbound.message_id)) {
                    queued_for_radio++;
                }
            }
        }
    }

    if (!radio_outbox_.empty()) {
        processIncomingEmail();
    }

    if (queued_for_radio > 0) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Queued " + std::to_string(queued_for_radio) +
                   " inbound internet message(s) for radio relay";
        msg.is_outgoing = false;
        msg.timestamp = unixNowSeconds();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
    }
}

void App::sendQueuedEmails() {
    // Delivery runs on the email worker; a send requested while a job is
    // running starts as soon as that job's results are applied.
    if (email_outbox_.empty()) {
        return;
    }
    startEmailGatewayJob(false, false, true);
}

// GUI thread: moves the outbox into the in-flight list and describes the
// deliveries for the worker.
void App::prepareQueuedEmails(EmailGatewayJobInput& input) {
    if (email_outbox_.empty()) {
        return;
    }

    const bool smtp_ready = !trimSpaces(config_.email.smtp_host).empty() &&
                            !trimSpaces(config_.email.smtp_username).empty();
    const bool winlink_telnet_ready =
        config_.email.winlink_telnet_enabled &&
        !trimSpaces(config_.email.winlink_telnet_host).empty() &&
        !normalizeCallsign(config_.email.winlink_telnet_callsign).empty() &&
        !trimSpaces(config_.email.winlink_telnet_password).empty();

    relay_diagnostics_.last_attempt_unix = unixNowSeconds();
    relay_diagnostics_.last_mode = "internet-relay";
    relay_diagnostics_.gateway_health = "active";

    if (!smtp_ready && !winlink_telnet_ready) {
        relay_diagnostics_.last_result = "config-missing";
        relay_diagnostics_.last_error = "No internet relay configured (Winlink Telnet/SMTP)";
        relay_diagnostics_.gateway_health = "config-missing";
        return;
    }

    input.winlink_send_ready = winlink_telnet_ready;
    if (winlink_telnet_ready) {
        input.winlink_send.host = trimSpaces(config_.email.winlink_telnet_host);
        input.winlink_send.port = config_.email.winlink_telnet_port;
        input.winlink_send.use_tls = config_.email.winlink_telnet_use_tls;
        input.winlink_send.callsign = normalizeCallsign(config_.email.winlink_telnet_callsign);
        input.winlink_send.password = config_.email.winlink_telnet_password;
    }
    input.smtp_ready = smtp_ready;
    if (smtp_ready) {
        input.smtp.host = config_.email.smtp_host;
        input.smtp.port = config_.email.smtp_port;
        input.smtp.use_tls = config_.email.smtp_use_tls;
        input.smtp.username = config_.email.smtp_username;
        input.smtp.password = config_.email.smtp_password;
        input.smtp.from = config_.email.smtp_username;  // Use SMTP login as From
    }

    email_inflight_ = std::move(email_outbox_);
    email_outbox_.clear();
    for (const auto& msg : email_inflight_) {
        EmailGatewayOutgoing out;
        out.store_id = msg.id;
        out.to = trimSpaces(msg.to_callsign);
        out.subject = msg.subject.empty()
            ? ("Radio message from " + msg.from_callsign)
            : msg.subject;
        out.body = msg.body;
        input.outgoing.push_back(std::move(out));
    }
}

// GUI thread: records each delivery outcome; failures go back to the
// front of the outbox for the next cycle.
void App::applyEmailSendResults(const EmailGatewayJobOutput& output) {
    if (email_inflight_.empty()) {
        return;
    }

    std::vector<Message> failed;
    for (size_t i = 0; i < email_inflight_.size(); ++i) {
        Message& msg = email_inflight_[i];
        const EmailGatewaySendResult result =
            i < output.sends.size() ? output.sends[i] : EmailGatewaySendResult{};
        if (!result.attempted) {
            // Skipped (job stopped): still queued in the store, retry later.
            msg.status = MessageStatus::Pending;
            failed.push_back(msg);
            continue;
        }
        relay_diagnostics_.last_target = trimSpaces(msg.to_callsign);
        relay_diagnostics_.last_attempt_unix = unixNowSeconds();
        relay_diagnostics_.last_failover_reason.clear();

        if (result.winlink_tried) {
            relay_diagnostics_.last_login_attempts = result.winlink_login_attempts;
            if (result.sent && result.relay_mode == "Winlink Telnet") {
                relay_diagnostics_.last_mode = result.winlink_login_mode.empty()
                    ? "winlink-telnet"
                    : ("winlink-telnet/" + result.winlink_login_mode);
            } else if (result.smtp_tried) {
                relay_diagnostics_.last_failover_reason =
                    "Winlink Telnet send failed, falling back to SMTP";
            }
        }
        if (result.sent && result.relay_mode == "SMTP") {
            if (result.winlink_tried) {
                relay_diagnostics_.smtp_fallback_count++;
                relay_diagnostics_.last_mode = "smtp-fallback";
            } else {
                relay_diagnostics_.last_mode = "smtp";
            }
        }

        if (result.sent) {
            msg.status = MessageStatus::Sent;
            if (store_ && store_->isOpen()) {
                // Confirmed send: marked sent and leaves the relay queue in
                // one transaction.
                if (!store_->markRelayedAndDequeue(msg.id)) {
                    store_->markAsSent(msg.id);
                }
            }
            relay_diagnostics_.total_success++;
            relay_diagnostics_.consecutive_failures = 0;
            relay_diagnostics_.last_success_unix = unixNowSeconds();
            relay_diagnostics_.last_result = result.relay_mode.empty()
                ? "relay-ok"
                : ("relay-ok/" + lowerAscii(result.relay_mode));
            relay_diagnostics_.last_error.clear();
            relay_diagnostics_.gateway_health = "relay-ok";
        } else {
            relay_diagnostics_.total_fail++;
            relay_diagnostics_.consecutive_failures++;
            relay_diagnostics_.last_result = "relay-fail";
            relay_diagnostics_.last_error = result.error;
            relay_diagnostics_.gateway_health = "relay-fail";
            msg.status = MessageStatus::Failed;
            if (store_ && store_->isOpen()) {
                store_->markAsFailed(msg.id);
            }
            failed.push_back(msg);
        }
    }
    email_inflight_.clear();

    // Re-queue failures for retry on next cycle, ahead of newer mail.
    for (auto& f : failed) {
        f.status = MessageStatus::Pending;
    }
    email_outbox_.insert(email_outbox_.begin(),
                         std::make_move_iterator(failed.begin()),
                         std::make_move_iterator(failed.end()));
}

// ============================================================================
// Beacon System
// ============================================================================

void App::recordStationLocation(const std::string& callsign,
                                bool has_coordinates,
                                double latitude,
                                double longitude,
                                const std::string& grid_square,
                                const std::string& details,
                                StationLocationSource source,
                                float snr,
                                bool email_gateway,
                                int64_t timestamp) {
    const std::string normalized = normalizeCallsign(callsign);
    const std::string local_call = normalizeCallsign(config_.station.callsign);
    if (normalized.empty() || normalized == "UNKNOWN" || isLikelyHashId(normalized) ||
        (!local_call.empty() && normalized == local_call)) {
        return;
    }

    StationLocationEntry entry;
    auto existing = std::find_if(station_locations_.begin(), station_locations_.end(),
                                 [&](const StationLocationEntry& candidate) {
                                     return candidate.callsign == normalized;
                                 });
    if (existing != station_locations_.end()) {
        entry = *existing;
        station_locations_.erase(existing);
    }

    entry.callsign = normalized;
    entry.source = source;
    entry.snr_db = snr;
    entry.email_gateway = email_gateway;
    entry.timestamp = timestamp > 0 ? timestamp : unixNowSeconds();

    if (!grid_square.empty()) {
        entry.grid_square = grid_square;
    }
    if (!details.empty()) {
        entry.details = details;
    }

    double resolved_lat = latitude;
    double resolved_lon = longitude;
    bool resolved_coordinates = has_coordinates;
    if (!resolved_coordinates && !entry.grid_square.empty()) {
        resolved_coordinates = gridSquareToLatLon(entry.grid_square, resolved_lat, resolved_lon);
    }
    if (resolved_coordinates) {
        entry.latitude = resolved_lat;
        entry.longitude = resolved_lon;
        entry.has_coordinates = true;
    }

    if (!entry.has_coordinates) {
        return;
    }

    station_locations_.insert(station_locations_.begin(), std::move(entry));
    while (station_locations_.size() > MAX_STATION_LOCATIONS) {
        station_locations_.pop_back();
    }
}

void App::seedStationLocationsFromHistory() {
    if (!store_ || !store_->isOpen()) {
        return;
    }

    auto beacons = store_->loadRecentBeacons(static_cast<int>(MAX_STATION_LOCATIONS));
    for (auto it = beacons.rbegin(); it != beacons.rend(); ++it) {
        recordStationLocation(it->callsign,
                              false,
                              0.0,
                              0.0,
                              it->grid_square,
                              it->message,
                              StationLocationSource::Beacon,
                              it->snr_db,
                              it->email_gateway,
                              it->timestamp);
    }
}

void App::onAprsPositionReceived(const APRSPosition& pos) {
    recordStationLocation(pos.callsign,
                          true,
                          pos.latitude,
                          pos.longitude,
                          "",
                          pos.comment,
                          StationLocationSource::APRS,
                          0.0f,
                          false,
                          pos.timestamp);
}

void App::sendBeacon() {
    if (!modem_) return;

    // Build advanced beacon packet:
    // 0xFA | snr_byte | grid_len | grid_square | flags | message
    // flags byte: bit 0 = email gateway available
    std::vector<uint8_t> packet;
    packet.push_back(0xFA);  // Beacon marker

    // Include current local SNR as signed byte
    float snr = modem_->getStatus().snr_db;
    packet.push_back(static_cast<uint8_t>(static_cast<int8_t>(snr)));

    // Include grid square (length-prefixed for parsing)
    std::string grid = config_.station.grid_square;
    if (grid.size() > 8) grid = grid.substr(0, 8);  // Max 8-char grid
    packet.push_back(static_cast<uint8_t>(grid.size()));
    packet.insert(packet.end(), grid.begin(), grid.end());

    // Flags byte
    uint8_t flags = 0;
    const bool email_gateway_available = isEmailGatewayConfigured(config_);
    if (email_gateway_available) {
        flags |= 0x01;  // Bit 0: email gateway available
    }
    packet.push_back(flags);

    // Include callsign and beacon text.
    // Keep legacy [EG] marker in the text so older clients still detect gateway capability.
    std::string beacon_text = trimSpaces(config_.beacon.beacon_text);
    if (email_gateway_available && beacon_text.find(kBeaconEmailGatewayMarker) == std::string::npos) {
        if (!beacon_text.empty()) {
            beacon_text += " ";
        }
        beacon_text += kBeaconEmailGatewayMarker;
    }

    std::string beacon = config_.station.callsign;
    if (!beacon_text.empty()) {
        beacon += " " + beacon_text;
    }
    packet.insert(packet.end(), beacon.begin(), beacon.end());

    // Send as beacon (uses 4x spreading for maximum range)
    if (!sendBroadcastPayload(packet)) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Beacon send failed";
        msg.is_outgoing = false;
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    last_beacon_sent_ = std::chrono::steady_clock::now();

}

void App::updateBeacon() {
    if (!config_.beacon.enabled || !modem_) return;
    beacon_enabled_ = config_.beacon.enabled;  // Keep member synced

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_beacon_sent_);

    if (elapsed.count() >= config_.beacon.interval_sec) {
        sendBeacon();
    }
}

void App::processReceivedBeacon(const std::string& from, const std::string& grid_square,
                                  const std::string& message, float snr, bool email_gateway) {
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    recordStationLocation(from,
                          false,
                          0.0,
                          0.0,
                          grid_square,
                          message,
                          StationLocationSource::Beacon,
                          snr,
                          email_gateway,
                          now);

    // Check if we have pending messages for this station
    bool has_pending = false;
    if (store_ && config_.beacon.notify_pending) {
        auto pending = store_->getPendingMessages(from);
        has_pending = !pending.empty();
    }

    // Add to heard beacons
    HeardBeacon beacon;
    beacon.callsign = from;
    beacon.grid_square = grid_square;
    beacon.message = message;
    beacon.snr_db = snr;
    beacon.timestamp = now;
    beacon.has_pending_messages = has_pending;
    beacon.email_gateway = email_gateway;

    if (email_gateway) {
        const std::string gateway_call = normalizeCallsign(from);
        auto& stats = gateway_stats_[gateway_call];
        stats.snr_accum += snr;
        stats.snr_samples++;
        stats.last_seen = now;

        const int hour_local = localHourFromUnix(now);
        auto& band_hour = gateway_stats_[gatewayBandHourKey(gateway_call, current_band_, hour_local)];
        band_hour.snr_accum += snr;
        band_hour.snr_samples++;
        band_hour.last_seen = now;
    }

    if (mesh_relay_ && mesh_relay_->isEnabled()) {
        mesh_relay_->processBeaconRoute(normalizeCallsign(from), snr);
    }

    // Remove old entry if exists
    heard_beacons_.erase(
        std::remove_if(heard_beacons_.begin(), heard_beacons_.end(),
                       [&](const auto& b) { return b.callsign == from; }),
        heard_beacons_.end());

    heard_beacons_.push_front(beacon);

    while (heard_beacons_.size() > MAX_HEARD_BEACONS) {
        heard_beacons_.pop_back();
    }

    if (store_ && store_->isOpen()) {
        BeaconRecord rec;
        rec.callsign = from;
        rec.grid_square = grid_square;
        rec.snr_db = snr;
        rec.message = message;
        rec.has_pending = has_pending;
        rec.email_gateway = email_gateway;
        rec.band = current_band_;
        rec.timestamp = now;
        store_->saveBeacon(rec);
    }

    // Queue PSK Reporter spot for beacon/CQ
    if (config_.psk_reporter.enabled && snr >= config_.psk_reporter.min_snr) {
        queuePSKReporterSpot(from, grid_square, snr);
    }

    // If we have pending messages, send notification
    if (has_pending) {
        checkPendingMessagesForStation(from);
    }

    // System message
    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "Heard beacon from " + from;
    if (!grid_square.empty()) {
        msg.text += " [" + grid_square + "]";
    }
    msg.text += " (SNR: " + std::to_string(static_cast<int>(snr)) + " dB)";
    if (has_pending) {
        msg.text += " [MESSAGES WAITING]";
    }
    if (email_gateway) {
        msg.text += " [EMAIL GATEWAY]";
    }
    msg.is_outgoing = false;
    msg.timestamp = now;
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::checkPendingMessagesForStation(const std::string& callsign) {
    if (!modem_ || !store_) return;

    auto pending = store_->getPendingMessages(callsign);
    if (pending.empty()) return;

    // Send notification via broadcast
    std::vector<uint8_t> packet;
    packet.push_back(0xF9);  // Pending message notification marker

    std::string notification = callsign + " DE " + config_.station.callsign +
                               " MSG WAITING (" + std::to_string(pending.size()) + ")";
    packet.insert(packet.end(), notification.begin(), notification.end());

    if (!sendBroadcastPayload(packet)) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Pending message notification send failed";
        msg.is_outgoing = false;
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

}

// ============================================================================
// QSO Management
// ============================================================================

void App::startQSO(const std::string& callsign) {
    qso_active_ = true;
    qso_start_time_ = std::chrono::steady_clock::now();
    snr_exchanged_ = false;
    snr_report_seq_ = 0;
    remote_snr_db_ = 0.0f;
    last_snr_exchange_sent_ = std::chrono::steady_clock::now();
    last_remote_snr_update_ = std::chrono::steady_clock::time_point{};
    if (SNR_KEEPALIVE_ENABLED) {
        const bool lead_side = config_.station.callsign <= callsign;
        next_snr_keepalive_due_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(
            lead_side ? SNR_KEEPALIVE_LEAD_START_MS : SNR_KEEPALIVE_FOLLOW_START_MS);
    } else {
        next_snr_keepalive_due_ = std::chrono::steady_clock::time_point{};
    }

    if (modem_) {
        local_snr_db_ = modem_->getStatus().snr_db;
    }

}

void App::endQSO() {
    qso_active_ = false;
    snr_exchanged_ = false;
    snr_report_seq_ = 0;
    last_snr_exchange_sent_ = std::chrono::steady_clock::time_point{};
    last_remote_snr_update_ = std::chrono::steady_clock::time_point{};
    next_snr_keepalive_due_ = std::chrono::steady_clock::time_point{};
}

void App::exchangeSNR(bool force) {
    if (!SNR_KEEPALIVE_ENABLED) return;
    if (!modem_) return;
    if (!force && snr_exchanged_) return;

    // Get current SNR
    local_snr_db_ = modem_->getStatus().snr_db;

    // Send SNR report packet
    std::vector<uint8_t> packet;
    packet.reserve(8);
    packet.push_back(0xF8);  // SNR report marker

    // Send as 2-byte signed integer (SNR * 10 for one decimal)
    int16_t snr_val = static_cast<int16_t>(local_snr_db_ * 10);
    packet.push_back(static_cast<uint8_t>(snr_val & 0xFF));
    packet.push_back(static_cast<uint8_t>((snr_val >> 8) & 0xFF));
    // Add a tiny fixed trailer so keep-alive packets are not ultra-short payloads.
    // Legacy receivers parse the first 3 bytes and ignore the rest.
    packet.push_back('S');
    packet.push_back('N');
    packet.push_back('R');
    uint16_t seq = snr_report_seq_++;
    packet.push_back(static_cast<uint8_t>(seq & 0xFF));
    packet.push_back(static_cast<uint8_t>((seq >> 8) & 0xFF));

    if (!modem_->sendData(packet)) {
        return;
    }
    snr_exchanged_ = true;
    last_snr_exchange_sent_ = std::chrono::steady_clock::now();

}

void App::processSNRReport(float remote_snr) {
    remote_snr_db_ = remote_snr;
    last_remote_snr_update_ = std::chrono::steady_clock::now();

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "Remote station reports your signal: " + std::to_string(static_cast<int>(remote_snr)) + " dB";
    msg.is_outgoing = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;

}

bool App::canSendSNRKeepAlive() const {
    if (!SNR_KEEPALIVE_ENABLED) {
        return false;
    }
    if (!modem_ || !modem_->isConnected()) {
        return false;
    }
    if (!qso_active_ || modem_->getStatus().link_state != LinkState::Connected) {
        return false;
    }

    const auto& status = modem_->getStatus();
    if (status.ptt_active || status.tx_buffer_bytes > 0 || status.busy_detected) {
        return false;
    }

    if (file_transfer_mgr_ && !file_transfer_mgr_->getActiveTransfers().empty()) {
        return false;
    }
    if (modem_->millisSinceLastDataSend() < SNR_KEEPALIVE_TX_GUARD_MS) {
        return false;
    }

    return true;
}

void App::updateSNRKeepAlive() {
    if (!SNR_KEEPALIVE_ENABLED) {
        return;
    }
    if (!canSendSNRKeepAlive()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (next_snr_keepalive_due_.time_since_epoch().count() == 0) {
        next_snr_keepalive_due_ = now + std::chrono::milliseconds(SNR_KEEPALIVE_LEAD_START_MS);
    }
    if (now < next_snr_keepalive_due_) {
        return;
    }
    if (last_remote_snr_update_.time_since_epoch().count() != 0) {
        auto remote_age = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_remote_snr_update_);
        if (remote_age.count() < SNR_KEEPALIVE_TX_GUARD_MS) {
            next_snr_keepalive_due_ = now + std::chrono::milliseconds(SNR_KEEPALIVE_TX_GUARD_MS);
            return;
        }
    }

    exchangeSNR(true);
    next_snr_keepalive_due_ = now + std::chrono::milliseconds(SNR_KEEPALIVE_INTERVAL_MS);
}

std::string App::formatQSODuration() const {
    if (!qso_active_) return "00:00:00";

    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - qso_start_time_);
    auto total_secs = duration.count();

    int hours = static_cast<int>(total_secs / 3600);
    int mins = static_cast<int>((total_secs % 3600) / 60);
    int secs = static_cast<int>(total_secs % 60);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hours, mins, secs);
    return std::string(buf);
}

// ============================================================================
// Away Mode
// ============================================================================

void App::enableAwayMode() {
    away_mode_active_ = true;

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "Away mode enabled: " + config_.away.away_message;
    msg.is_outgoing = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;

}

void App::disableAwayMode() {
    away_mode_active_ = false;

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "Away mode disabled";
    msg.is_outgoing = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;

}

void App::processAwayModeRequest(const std::string& from, const std::vector<uint8_t>& data) {
    if (!away_mode_active_) return;
    if (data.size() < 2) return;  // Need at least 0xF6 + command byte

    // data[0] is 0xF6 (away mode request marker), data[1] is the actual command
    uint8_t request_type = data[1];

    switch (request_type) {
        case 0x01:  // Query messages
            if (config_.away.allow_message_query && store_) {
                auto messages = store_->getPendingMessages(from);
                std::string response = "You have " + std::to_string(messages.size()) + " pending message(s)";
                sendAwayModeResponse(from, response);
            }
            break;

        case 0x02:  // Query BBS files
            if (config_.away.allow_bbs_access) {
                refreshBBSFiles();
                std::string response = "BBS has " + std::to_string(bbs_files_.size()) + " file(s) available";
                sendAwayModeResponse(from, response);
            }
            break;

        case 0x03:  // Leave message
            if (data.size() > 2) {
                std::string message(data.begin() + 2, data.end());

                // Store the message
                if (store_) {
                    Message msg;
                    msg.type = MessageType::Private;
                    msg.status = MessageStatus::Pending;
                    msg.from_callsign = from;
                    msg.to_callsign = config_.station.callsign;
                    msg.subject = "Away mode message";
                    msg.body = message;
                    msg.created_at = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    store_->createMessage(msg);
                }

                sendAwayModeResponse(from, "Message received and stored");

                ChatMessage chat_msg;
                chat_msg.from = from;
                chat_msg.text = "[AWAY MSG] " + message;
                chat_msg.is_outgoing = false;
                chat_msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                chat_history_.push_back(chat_msg);
                chat_scroll_to_bottom_ = true;
            }
            break;

        case 0x04:  // Email gateway request
            if (config_.away.allow_email_gateway) {
                sendAwayModeResponse(from, "Email gateway available. Send EMAIL:<to>:<subject>:<body>");
            }
            break;

        default:
            sendAwayModeResponse(from, config_.away.away_message);
            break;
    }
}

void App::sendAwayModeResponse(const std::string& to, const std::string& response) {
    if (!modem_) return;

    std::vector<uint8_t> packet;
    packet.push_back(0xF7);  // Away mode response marker
    packet.insert(packet.end(), response.begin(), response.end());

    modem_->sendData(packet);

    ChatMessage msg;
    msg.from = config_.station.callsign;
    msg.text = "[AWAY REPLY to " + to + "] " + response;
    msg.is_outgoing = true;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

// ============================================================================
// Ping (Status Check Without Full QSO)
// ============================================================================

void App::sendPing(const std::string& callsign) {
    if (!modem_) return;
    if (ping_in_progress_) {
        // Already waiting for a ping response
        return;
    }
    std::string target = normalizeCallsign(callsign);
    if (target.empty()) {
        return;
    }

    // Connectionless ping: a broadcast addressed to one station, answered by
    // a broadcast addressed back. Both carry the callsigns themselves: the
    // modem names a sender it has not heard a BEACON from as "?".
    // 0xE0 | target_len | target | origin_len | origin | grid_len | grid
    std::string grid = config_.station.grid_square;
    if (grid.size() > 8) grid = grid.substr(0, 8);
    const std::string self = normalizeCallsign(config_.station.callsign);
    std::vector<uint8_t> request;
    request.push_back(0xE0);
    request.push_back(static_cast<uint8_t>(target.size()));
    request.insert(request.end(), target.begin(), target.end());
    request.push_back(static_cast<uint8_t>(self.size()));
    request.insert(request.end(), self.begin(), self.end());
    request.push_back(static_cast<uint8_t>(grid.size()));
    request.insert(request.end(), grid.begin(), grid.end());

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.is_outgoing = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (!sendBroadcastPayload(request)) {
        msg.text = "Ping to " + target + " could not be sent";
    } else {
        ping_in_progress_ = true;
        ping_target_ = target;
        ping_sent_time_ = std::chrono::steady_clock::now();
        msg.text = "Ping sent to " + target;
    }
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::processPingRequest(const std::string& from, const std::vector<uint8_t>& data) {
    // 0xE0 | target_len | target | origin_len | origin | grid_len | grid
    size_t at = 1;
    auto field = [&](std::string* out) {
        if (data.size() <= at || data.size() < at + 1 + data[at]) return false;
        out->assign(data.begin() + static_cast<std::ptrdiff_t>(at + 1),
                    data.begin() + static_cast<std::ptrdiff_t>(at + 1 + data[at]));
        at += 1 + data[at];
        return true;
    };
    std::string target, origin, remote_grid;
    if (!field(&target) || !field(&origin)) return;
    (void)field(&remote_grid);
    if (normalizeCallsign(target) != normalizeCallsign(config_.station.callsign)) {
        return;  // addressed to another station
    }
    origin = normalizeCallsign(origin);
    if (origin.empty()) origin = normalizeCallsign(from);
    if (origin.empty()) return;

    // 0xE1 | origin_len | origin | responder_len | responder | snr | grid_len | grid
    const std::string self = normalizeCallsign(config_.station.callsign);
    std::vector<uint8_t> response;
    response.push_back(0xE1);
    response.push_back(static_cast<uint8_t>(origin.size()));
    response.insert(response.end(), origin.begin(), origin.end());
    response.push_back(static_cast<uint8_t>(self.size()));
    response.insert(response.end(), self.begin(), self.end());
    float snr = modem_ ? modem_->getStatus().snr_db : 0.0f;
    response.push_back(static_cast<uint8_t>(static_cast<int8_t>(std::clamp(snr, -127.0f, 127.0f))));
    std::string grid = config_.station.grid_square;
    if (grid.size() > 8) grid = grid.substr(0, 8);
    response.push_back(static_cast<uint8_t>(grid.size()));
    response.insert(response.end(), grid.begin(), grid.end());
    (void)sendBroadcastPayload(response);

    // Add to ping results
    PingResult result;
    result.callsign = origin;
    result.grid_square = remote_grid;
    result.snr_db = snr;
    result.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    result.online = true;

    ping_results_.push_front(result);
    while (ping_results_.size() > MAX_PING_RESULTS) {
        ping_results_.pop_back();
    }
    if (isKnownEmailGateway(origin)) {
        const std::string gateway_call = origin;
        auto& stats = gateway_stats_[gateway_call];
        stats.snr_accum += snr;
        stats.snr_samples++;
        stats.last_seen = result.timestamp;

        const int hour_local = localHourFromUnix(result.timestamp);
        auto& band_hour = gateway_stats_[gatewayBandHourKey(gateway_call, current_band_, hour_local)];
        band_hour.snr_accum += snr;
        band_hour.snr_samples++;
        band_hour.last_seen = result.timestamp;
    }
    if (store_ && store_->isOpen()) {
        PingRecord rec;
        rec.callsign = origin;
        rec.snr_db = snr;
        rec.grid_square = remote_grid;
        rec.band = current_band_;
        rec.online = true;
        rec.timestamp = result.timestamp;
        store_->savePingResult(rec);
    }

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "Ping from " + origin;
    if (!remote_grid.empty()) {
        msg.text += " [" + remote_grid + "]";
    }
    msg.text += " - responded";
    msg.is_outgoing = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::processPingResponse(const std::string& from, const std::vector<uint8_t>& data) {
    if (!ping_in_progress_) return;

    // 0xE1 | origin_len | origin | responder_len | responder | snr | grid_len | grid
    size_t at = 1;
    auto field = [&](std::string* out) {
        if (data.size() <= at || data.size() < at + 1 + data[at]) return false;
        out->assign(data.begin() + static_cast<std::ptrdiff_t>(at + 1),
                    data.begin() + static_cast<std::ptrdiff_t>(at + 1 + data[at]));
        at += 1 + data[at];
        return true;
    };
    std::string origin, responder, remote_grid;
    if (!field(&origin) || !field(&responder) || data.size() <= at) return;
    const float remote_snr = static_cast<float>(static_cast<int8_t>(data[at++]));
    (void)field(&remote_grid);
    if (responder.empty()) responder = from;
    if (normalizeCallsign(origin) != normalizeCallsign(config_.station.callsign) ||
        normalizeCallsign(responder) != normalizeCallsign(ping_target_)) {
        return;
    }
    (void)from;

    ping_in_progress_ = false;

    // Add to results
    PingResult result;
    result.callsign = ping_target_;
    result.grid_square = remote_grid;
    result.snr_db = remote_snr;
    result.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    result.online = true;

    ping_results_.push_front(result);
    while (ping_results_.size() > MAX_PING_RESULTS) {
        ping_results_.pop_back();
    }
    if (isKnownEmailGateway(ping_target_)) {
        const std::string gateway_call = normalizeCallsign(ping_target_);
        auto& stats = gateway_stats_[gateway_call];
        stats.snr_accum += remote_snr;
        stats.snr_samples++;
        stats.last_seen = result.timestamp;

        const int hour_local = localHourFromUnix(result.timestamp);
        auto& band_hour = gateway_stats_[gatewayBandHourKey(gateway_call, current_band_, hour_local)];
        band_hour.snr_accum += remote_snr;
        band_hour.snr_samples++;
        band_hour.last_seen = result.timestamp;
    }
    if (store_ && store_->isOpen()) {
        PingRecord rec;
        rec.callsign = ping_target_;
        rec.snr_db = remote_snr;
        rec.grid_square = remote_grid;
        rec.band = current_band_;
        rec.online = true;
        rec.timestamp = result.timestamp;
        store_->savePingResult(rec);
    }

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "Pong from " + ping_target_;
    if (!remote_grid.empty()) {
        msg.text += " [" + remote_grid + "]";
    }
    msg.text += " - SNR: " + std::to_string(static_cast<int>(remote_snr)) + " dB";
    msg.is_outgoing = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::updatePing() {
    if (!ping_in_progress_) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - ping_sent_time_);

    if (elapsed.count() > PING_TIMEOUT_MS) {
        ping_in_progress_ = false;

        // Add timeout result
        PingResult result;
        result.callsign = ping_target_;
        result.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        result.online = false;

        ping_results_.push_front(result);
        while (ping_results_.size() > MAX_PING_RESULTS) {
            ping_results_.pop_back();
        }
        if (store_ && store_->isOpen()) {
            PingRecord rec;
            rec.callsign = ping_target_;
            rec.snr_db = 0.0f;
            rec.grid_square.clear();
            rec.band = current_band_;
            rec.online = false;
            rec.timestamp = result.timestamp;
            store_->savePingResult(rec);
        }

        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Ping to " + ping_target_ + " timed out";
        msg.is_outgoing = false;
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
    }
}

// ============================================================================
// Mail (Asynchronous Messages)
// ============================================================================

bool App::isKnownEmailGateway(const std::string& callsign) const {
    const std::string normalized = normalizeCallsign(callsign);
    if (normalized.empty()) {
        return false;
    }
    for (const auto& beacon : heard_beacons_) {
        if (normalizeCallsign(beacon.callsign) == normalized && beacon.email_gateway) {
            return true;
        }
    }
    return false;
}

bool App::isWeakSignalLink(const std::string& callsign) const {
    if (!config_.email.weak_signal_profile_enabled) {
        return false;
    }

    const float threshold = static_cast<float>(config_.email.weak_signal_snr_threshold_db);
    float score_sum = 0.0f;
    int score_samples = 0;

    if (modem_ && modem_->isConnected()) {
        const ModemStatus status = modem_->getStatus();
        score_sum += status.snr_db;
        score_samples++;
        if (status.busy_detected || status.ptt_active || status.tx_buffer_bytes > 0) {
            // Busy channel at send time often behaves like weak-link delivery.
            score_sum += (threshold - 4.0f);
            score_samples++;
        }
    }
    if (snr_exchanged_ || std::fabs(remote_snr_db_) > 0.01f) {
        score_sum += remote_snr_db_;
        score_samples++;
    }

    const std::string normalized = normalizeCallsign(callsign);
    if (!normalized.empty()) {
        auto it = gateway_stats_.find(normalized);
        if (it != gateway_stats_.end() && it->second.snr_samples > 0) {
            score_sum += it->second.snr_accum / static_cast<float>(it->second.snr_samples);
            score_samples++;
        }
        const int hour_local = localHourFromUnix(unixNowSeconds());
        auto bit = gateway_stats_.find(gatewayBandHourKey(normalized, current_band_, hour_local));
        if (bit != gateway_stats_.end() && bit->second.snr_samples > 0) {
            score_sum += bit->second.snr_accum / static_cast<float>(bit->second.snr_samples);
            score_samples++;
        }
    }

    if (score_samples == 0) {
        return false;
    }
    const float avg_score = score_sum / static_cast<float>(score_samples);
    return avg_score <= threshold;
}

std::string App::selectBestGateway(float* score_out) const {
    std::string best;
    float best_score = -1e9f;
    const int64_t now = unixNowSeconds();
    const int now_hour = localHourFromUnix(now);
    const int hour_offsets[] = {0, -1, 1};
    const float hour_weights[] = {1.0f, 0.55f, 0.55f};
    std::string connected_remote;
    bool connected_link_busy = false;
    float connected_link_snr = 0.0f;
    if (modem_ && modem_->isConnected()) {
        const ModemStatus status = modem_->getStatus();
        connected_remote = normalizeCallsign(status.remote_callsign);
        connected_link_busy = status.busy_detected || status.ptt_active || status.tx_buffer_bytes > 0;
        connected_link_snr = status.snr_db;
    }
    const float weak_threshold = static_cast<float>(config_.email.weak_signal_snr_threshold_db);

    for (const auto& beacon : heard_beacons_) {
        if (!beacon.email_gateway) {
            continue;
        }
        const std::string cs = normalizeCallsign(beacon.callsign);
        if (cs.empty()) {
            continue;
        }

        float score = beacon.snr_db;
        auto it = gateway_stats_.find(cs);
        if (it != gateway_stats_.end()) {
            const GatewayStats& stats = it->second;
            if (stats.snr_samples > 0) {
                score = (score * 0.4f) + ((stats.snr_accum / static_cast<float>(stats.snr_samples)) * 0.6f);
            }
            const int total = stats.relay_success + stats.relay_fail;
            if (total > 0) {
                score += 10.0f * (static_cast<float>(stats.relay_success) / static_cast<float>(total));
                score -= 4.0f * (static_cast<float>(stats.relay_fail) / static_cast<float>(total));
            }
            const int64_t age_sec = std::max<int64_t>(0, now - stats.last_seen);
            score -= static_cast<float>(age_sec) / 3600.0f;  // 1 point/hour staleness.
        }

        // Band/time weighted propagation model:
        // Blend same-hour and adjacent-hour history for the current band.
        double band_weight_total = 0.0;
        double band_snr_weighted = 0.0;
        double band_success_weighted = 0.0;
        double band_fail_weighted = 0.0;
        int64_t band_last_seen = 0;
        for (size_t i = 0; i < 3; ++i) {
            const int hist_hour = (now_hour + hour_offsets[i] + 24) % 24;
            const std::string key = gatewayBandHourKey(cs, current_band_, hist_hour);
            auto bit = gateway_stats_.find(key);
            if (bit == gateway_stats_.end()) {
                continue;
            }
            const GatewayStats& band_stats = bit->second;
            if (band_stats.snr_samples > 0) {
                const float avg = band_stats.snr_accum / static_cast<float>(band_stats.snr_samples);
                band_snr_weighted += static_cast<double>(avg) * hour_weights[i];
                band_weight_total += hour_weights[i];
            }
            band_success_weighted += static_cast<double>(band_stats.relay_success) * hour_weights[i];
            band_fail_weighted += static_cast<double>(band_stats.relay_fail) * hour_weights[i];
            band_last_seen = std::max<int64_t>(band_last_seen, band_stats.last_seen);
        }
        if (band_weight_total > 0.0) {
            const float band_avg = static_cast<float>(band_snr_weighted / band_weight_total);
            score = (score * 0.45f) + (band_avg * 0.55f);
        }
        const double band_total = band_success_weighted + band_fail_weighted;
        if (band_total > 0.0) {
            score += 6.0f * static_cast<float>(band_success_weighted / band_total);
            score -= 3.0f * static_cast<float>(band_fail_weighted / band_total);
        }
        if (band_last_seen > 0) {
            const int64_t age_sec = std::max<int64_t>(0, now - band_last_seen);
            score -= std::min(3.0f, static_cast<float>(age_sec) / 5400.0f);
        }

        if (!connected_remote.empty() && cs == connected_remote) {
            if (connected_link_busy) {
                score -= 8.0f;
            } else {
                score += 1.5f;
            }
            if (connected_link_snr <= weak_threshold) {
                score -= 2.0f;
            }
        }

        if (score > best_score) {
            best_score = score;
            best = cs;
        }
    }

    if (score_out) {
        *score_out = best.empty() ? 0.0f : best_score;
    }
    return best;
}

void App::recordGatewayRelayResult(const std::string& gateway_callsign, bool success) {
    const std::string cs = normalizeCallsign(gateway_callsign);
    if (cs.empty()) {
        return;
    }
    const int64_t now = unixNowSeconds();
    auto& stats = gateway_stats_[cs];
    stats.last_seen = now;
    if (success) {
        stats.relay_success++;
    } else {
        stats.relay_fail++;
    }

    const int hour_local = localHourFromUnix(now);
    auto& band_hour = gateway_stats_[gatewayBandHourKey(cs, current_band_, hour_local)];
    band_hour.last_seen = now;
    if (success) {
        band_hour.relay_success++;
    } else {
        band_hour.relay_fail++;
    }

    relay_diagnostics_.last_attempt_unix = now;
    relay_diagnostics_.last_target = cs;
    relay_diagnostics_.last_mode = "hf-gateway";
    relay_diagnostics_.last_result = success ? "hf-ack" : "hf-nack";
    relay_diagnostics_.gateway_health = success ? "good-opening" : "hf-gateway-fail";
    if (success) {
        relay_diagnostics_.total_success++;
        relay_diagnostics_.consecutive_failures = 0;
        relay_diagnostics_.last_success_unix = now;
        relay_diagnostics_.last_error.clear();
    } else {
        relay_diagnostics_.total_fail++;
        relay_diagnostics_.consecutive_failures++;
        relay_diagnostics_.last_error = "Gateway NACK/timeout from " + cs;
    }
}

bool App::sendMailWirePacket(const MailMessage& mail) {
    if (!modem_) {
        return false;
    }

    const bool to_email = looksLikeEmailAddress(mail.to);
    if (to_email && (modem_->getStatus().link_state != LinkState::Connected)) {
        return false;
    }

    std::vector<uint8_t> packet;
    if (!buildReliableMailWirePacket(mail, config_, &packet)) {
        return false;
    }

    if (mail.is_broadcast) {
        return sendBroadcastPayload(packet);
    }

    if (!modem_->sendData(packet)) {
        return false;
    }

    if (mail.weak_signal_profile) {
        const ModemStatus status = modem_->getStatus();
        const bool channel_clear = !status.busy_detected && !status.ptt_active && status.tx_buffer_bytes == 0;
        if (channel_clear) {
            // Adaptive redundancy: send one extra copy on weak links when channel conditions permit.
            (void)modem_->sendData(packet);
        }
    }
    return true;
}

void App::sendMailAck(const std::string& message_id, bool ack, const std::string& reason) {
    if (!modem_ || message_id.empty()) {
        return;
    }

    std::string mid = message_id;
    if (mid.size() > kMailMaxMessageIdLen) {
        mid.resize(kMailMaxMessageIdLen);
    }
    std::string text = reason;
    if (text.size() > kMailMaxAckReasonLen) {
        text.resize(kMailMaxAckReasonLen);
    }

    std::vector<uint8_t> packet;
    packet.push_back(kMailAckMarker);
    packet.push_back(ack ? kMailAckStatusAck : kMailAckStatusNack);
    packet.push_back(static_cast<uint8_t>(mid.size()));
    packet.insert(packet.end(), mid.begin(), mid.end());
    packet.insert(packet.end(), text.begin(), text.end());
    modem_->sendData(packet);
}

void App::processMailAck(const std::string& from, const std::vector<uint8_t>& data) {
    if (data.size() < 4) return;
    const bool ack = data[1] == kMailAckStatusAck;
    const uint8_t id_len = data[2];
    if (data.size() < 3 + id_len) {
        return;
    }

    const std::string message_id(data.begin() + 3, data.begin() + 3 + id_len);
    std::string reason;
    if (data.size() > 3 + id_len) {
        reason.assign(data.begin() + 3 + id_len, data.end());
    }

    const std::string ack_from = normalizeCallsign(from);
    for (auto& mail : mail_outbox_) {
        if (mail.message_id != message_id) {
            continue;
        }
        // Only the addressee (or, for internet mail, the gateway that
        // carried it) may confirm or refuse delivery.
        bool from_recipient = !ack_from.empty() && ack_from == normalizeCallsign(mail.to);
        if (!from_recipient && looksLikeEmailAddress(mail.to)) {
            const std::string pinned_gateway = normalizeCallsign(auto_gateway_target_);
            // Internet mail only leaves over a link, so the linked peer is
            // the gateway it was routed through.
            from_recipient = !ack_from.empty() &&
                             (isKnownEmailGateway(ack_from) ||
                              (!pinned_gateway.empty() && ack_from == pinned_gateway) ||
                              ack_from == linkedPeerCallsign());
        }
        if (!from_recipient) {
            continue;
        }
        if (ack && mail.status == MessageStatus::Delivered) {
            return;
        }
        mail.status = ack ? MessageStatus::Delivered : MessageStatus::Failed;

        if (store_ && store_->isOpen()) {
            if (mail.store_id > 0) {
                if (ack) {
                    store_->markAsDelivered(mail.store_id);
                } else {
                    store_->markAsFailed(mail.store_id);
                }
            } else {
                auto by_mid = store_->getMessageById(message_id);
                if (by_mid.has_value()) {
                    mail.store_id = by_mid->id;
                    if (ack) {
                        store_->markAsDelivered(mail.store_id);
                    } else {
                        store_->markAsFailed(mail.store_id);
                    }
                }
            }
        }
        if (looksLikeEmailAddress(mail.to)) {
            recordGatewayRelayResult(from, ack);
        }

        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = std::string("Mail ") + (ack ? "ACK" : "NACK") + " from " + from +
                   " for MID " + message_id;
        if (!reason.empty()) {
            msg.text += ": " + reason;
        }
        msg.is_outgoing = false;
        msg.timestamp = unixNowSeconds();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }
}

void App::persistMailReadState(const MailMessage& mail) {
    if (!store_ || !store_->isOpen() || mail.message_id.empty()) {
        return;
    }
    store_->setMailReadState(mail.message_id, mail.read);
}

void App::processQueuedGatewayMail() {
    if (!modem_) {
        return;
    }

    bool has_queued = false;
    for (const auto& mail : mail_outbox_) {
        if (mail.needs_gateway &&
            (mail.status == MessageStatus::Pending || mail.status == MessageStatus::Sent)) {
            has_queued = true;
            break;
        }
    }
    if (!has_queued) {
        relay_diagnostics_.queue_policy_state = "idle";
        relay_diagnostics_.last_hold_reason.clear();
        relay_diagnostics_.last_burst_sent = 0;
        return;
    }

    const int64_t now_unix = unixNowSeconds();
    relay_diagnostics_.last_attempt_unix = now_unix;
    relay_diagnostics_.last_mode = "hf-gateway";
    relay_diagnostics_.gateway_health = "queue-active";

    const ModemStatus status = modem_->getStatus();
    const bool channel_busy = status.busy_detected || status.ptt_active || status.tx_buffer_bytes > 0;
    const float weak_threshold = static_cast<float>(config_.email.weak_signal_snr_threshold_db);
    const bool live_weak = status.snr_db <= weak_threshold;
    const bool connected = status.link_state == LinkState::Connected;
    if (!connected) {
        const auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_gateway_connect_attempt_);
        if (elapsed.count() < GATEWAY_CONNECT_RETRY_MS) {
            relay_diagnostics_.queue_policy_state = "wait-connect-retry";
            relay_diagnostics_.gateway_health = "waiting-connect-retry";
            return;
        }

        float score = 0.0f;
        std::string best = selectBestGateway(&score);
        if (best.empty()) {
            relay_diagnostics_.queue_policy_state = "no-gateway";
            relay_diagnostics_.gateway_health = "no-gateway-heard";
            relay_diagnostics_.last_error = "No email gateway beacon heard";
            return;
        }

        last_gateway_connect_attempt_ = now;
        auto_gateway_target_ = best;
        relay_diagnostics_.last_target = best;
        relay_diagnostics_.last_gateway_score = score;
        relay_diagnostics_.queue_policy_state = "connecting";
        relay_diagnostics_.gateway_health = "connecting";
        connectToStation(best);

        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Auto-connecting to best email gateway " + best +
                   " (score " + std::to_string(score).substr(0, 4) + ")";
        msg.is_outgoing = false;
        msg.timestamp = unixNowSeconds();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }
    if (channel_busy) {
        // Avoid queue drains while the channel is actively occupied.
        relay_diagnostics_.queue_policy_state = "channel-busy";
        relay_diagnostics_.gateway_health = "channel-busy";
        return;
    }

    std::string remote = status.remote_callsign;
    if (remote.empty()) {
        remote = current_chat_partner_;
    }
    remote = normalizeCallsign(remote);
    const std::string pinned_gateway = normalizeCallsign(auto_gateway_target_);
    const bool remote_is_gateway = isKnownEmailGateway(remote) ||
                                   (!pinned_gateway.empty() && remote == pinned_gateway);
    if (!remote_is_gateway) {
        relay_diagnostics_.queue_policy_state = "connected-non-gateway";
        relay_diagnostics_.gateway_health = "searching-gateway";
        const auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_gateway_connect_attempt_);
        if (elapsed.count() >= GATEWAY_CONNECT_RETRY_MS) {
            float score = 0.0f;
            std::string best = selectBestGateway(&score);
            if (!best.empty() && best != remote) {
                modem_->disconnectStation();
                connectToStation(best);
                last_gateway_connect_attempt_ = now;
                auto_gateway_target_ = best;
                relay_diagnostics_.last_target = best;
                relay_diagnostics_.last_gateway_score = score;
            }
        }
        return;
    }

    float selected_score = 0.0f;
    const std::string selected_gateway = selectBestGateway(&selected_score);
    float remote_score = status.snr_db;
    if (!selected_gateway.empty() && selected_gateway == remote) {
        remote_score = selected_score;
    } else {
        auto it = gateway_stats_.find(remote);
        if (it != gateway_stats_.end() && it->second.snr_samples > 0) {
            remote_score = it->second.snr_accum / static_cast<float>(it->second.snr_samples);
        }
        const int hour_local = localHourFromUnix(now_unix);
        auto bh = gateway_stats_.find(gatewayBandHourKey(remote, current_band_, hour_local));
        if (bh != gateway_stats_.end() && bh->second.snr_samples > 0) {
            const float band_avg = bh->second.snr_accum / static_cast<float>(bh->second.snr_samples);
            remote_score = (remote_score * 0.45f) + (band_avg * 0.55f);
        }
    }
    relay_diagnostics_.last_gateway_score = remote_score;
    relay_diagnostics_.last_target = remote;

    const bool poor_opening = live_weak || remote_score <= (weak_threshold - 3.0f);
    const bool strong_opening = !poor_opening && remote_score >= (weak_threshold + 6.0f);
    int burst_budget = strong_opening ? 3 : 1;
    if (strong_opening && remote_score >= (weak_threshold + 10.0f)) {
        burst_budget = 4;
    }

    relay_diagnostics_.queue_policy_state = strong_opening
        ? "burst-opening"
        : (poor_opening ? "hold-low-priority" : "normal");
    relay_diagnostics_.gateway_health = poor_opening ? "weak-opening" : "good-opening";

    int sent_this_cycle = 0;
    int held_this_cycle = 0;
    for (auto& mail : mail_outbox_) {
        if (!mail.needs_gateway ||
            (mail.status != MessageStatus::Pending && mail.status != MessageStatus::Sent)) {
            continue;
        }
        if (sent_this_cycle >= burst_budget) {
            break;
        }
        if (!mail.weak_signal_profile && !remote.empty()) {
            mail.weak_signal_profile = isWeakSignalLink(remote) || live_weak;
        } else if (!mail.weak_signal_profile) {
            mail.weak_signal_profile = live_weak;
        }
        if (looksLikeEmailAddress(mail.to) && config_.email.winlink_compat_mode) {
            mail.winlink_payload = true;
        }

        if (poor_opening && !isHighPriorityGatewayMail(mail)) {
            const int64_t queued_age_sec = std::max<int64_t>(0, now_unix - mail.timestamp);
            if (queued_age_sec < 1800) {
                held_this_cycle++;
                relay_diagnostics_.held_low_priority_count++;
                relay_diagnostics_.last_hold_reason =
                    "Weak opening on " + current_band_ + ", holding low-priority queue";
                continue;
            }
        }

        if (!sendMailWirePacket(mail)) {
            continue;
        }
        mail.status = MessageStatus::Sent;
        mail.needs_gateway = false;
        mail.timestamp = now_unix;
        if (store_ && store_->isOpen() && mail.store_id > 0) {
            store_->markAsSent(mail.store_id);
        }
        sent_this_cycle++;
        relay_diagnostics_.last_result = "hf-queued";
        relay_diagnostics_.last_error.clear();
    }

    relay_diagnostics_.last_burst_sent = sent_this_cycle;
    if (!poor_opening || held_this_cycle == 0) {
        relay_diagnostics_.last_hold_reason.clear();
    }
}

void App::updateReliableMail() {
    if (!modem_) {
        return;
    }

    const int64_t now = unixNowSeconds();
    const ModemStatus status = modem_->getStatus();
    const bool channel_busy = status.busy_detected || status.ptt_active || status.tx_buffer_bytes > 0;
    const float weak_threshold = static_cast<float>(config_.email.weak_signal_snr_threshold_db);
    const float live_snr_db = status.snr_db;
    const bool live_weak = live_snr_db <= weak_threshold;

    std::string remote = normalizeCallsign(status.remote_callsign);
    if (remote.empty()) {
        remote = normalizeCallsign(current_chat_partner_);
    }
    const std::string pinned_gateway = normalizeCallsign(auto_gateway_target_);
    const bool connected_gateway =
        status.link_state == LinkState::Connected &&
        (!remote.empty() && (isKnownEmailGateway(remote) ||
                             (!pinned_gateway.empty() && remote == pinned_gateway)));
    const int weak_timeout_sec = std::max(5, config_.email.weak_signal_ack_timeout_ms / 1000);
    const int weak_max_retries = std::max(MAIL_MAX_RETRIES, config_.email.weak_signal_max_retries);
    auto adaptiveAckTimeoutSec = [&](const MailMessage& mail) {
        int timeout = mail.weak_signal_profile ? weak_timeout_sec : (MAIL_ACK_TIMEOUT_MS / 1000);
        if (live_weak) {
            timeout += 10;
        }
        if (live_snr_db <= (weak_threshold - 4.0f)) {
            timeout += 10;
        }
        if (channel_busy) {
            timeout += 5;
        }
        return std::max(5, timeout);
    };
    auto adaptiveMaxRetries = [&](const MailMessage& mail) {
        int retries = mail.weak_signal_profile ? weak_max_retries : MAIL_MAX_RETRIES;
        if (live_weak) {
            retries += 1;
        }
        if (live_snr_db <= (weak_threshold - 4.0f)) {
            retries += 1;
        }
        if (channel_busy) {
            retries += 1;
        }
        return std::max(MAIL_MAX_RETRIES, retries);
    };

    for (auto& mail : mail_outbox_) {
        if (mail.status == MessageStatus::Pending && !mail.needs_gateway && !mail.is_broadcast) {
            if (!mail.weak_signal_profile) {
                std::string weak_ref = remote;
                if (looksLikeEmailAddress(mail.to) && !pinned_gateway.empty()) {
                    weak_ref = pinned_gateway;
                }
                if (!weak_ref.empty()) {
                    mail.weak_signal_profile = isWeakSignalLink(weak_ref) || live_weak || channel_busy;
                } else {
                    mail.weak_signal_profile = live_weak || channel_busy;
                }
            }
            if (looksLikeEmailAddress(mail.to) && !connected_gateway) {
                mail.needs_gateway = true;
                continue;
            }
            if (channel_busy) {
                continue;
            }
            if ((now - mail.timestamp) < 2) {
                continue;
            }
            if (sendMailWirePacket(mail)) {
                mail.status = MessageStatus::Sent;
                mail.timestamp = now;
                if (store_ && store_->isOpen() && mail.store_id > 0) {
                    store_->markAsSent(mail.store_id);
                }
            } else {
                mail.timestamp = now;
            }
            continue;
        }
        if (mail.status != MessageStatus::Sent || mail.is_broadcast) {
            continue;
        }
        const int ack_timeout_sec = adaptiveAckTimeoutSec(mail);
        if (now - mail.timestamp < ack_timeout_sec) {
            continue;
        }

        const int max_retries = adaptiveMaxRetries(mail);
        if (mail.retry_count >= max_retries) {
            mail.status = MessageStatus::Failed;
            if (store_ && store_->isOpen() && mail.store_id > 0) {
                store_->markAsFailed(mail.store_id);
            }
            continue;
        }

        if (channel_busy) {
            continue;
        }
        if (!sendMailWirePacket(mail)) {
            if (looksLikeEmailAddress(mail.to)) {
                mail.needs_gateway = true;
            }
            mail.timestamp = now;
            continue;
        }
        mail.retry_count++;
        mail.timestamp = now;
        if (store_ && store_->isOpen() && mail.store_id > 0) {
            store_->incrementRetry(mail.store_id);
            store_->markAsSent(mail.store_id);
        }
    }
}

void App::processReliableMailReceived(const std::string& from, const std::vector<uint8_t>& data, bool is_broadcast,
                                      bool via_link, const std::string& relayed_by) {
    if (data.size() < 5) return;

    size_t pos = 1;
    const uint8_t id_len = data[pos++];
    if (id_len == 0 || id_len > kMailMaxMessageIdLen || data.size() < pos + id_len + 1) {
        return;
    }
    std::string message_id(data.begin() + pos, data.begin() + pos + id_len);
    pos += id_len;
    const uint8_t flags = data[pos++];

    MailMessage mail;
    mail.message_id = message_id;
    mail.from = normalizeCallsign(from);
    mail.timestamp = unixNowSeconds();
    mail.read = false;
    mail.is_broadcast = is_broadcast;
    mail.status = MessageStatus::Delivered;
    mail.weak_signal_profile = (flags & kMailFlagWeakSignalProfile) != 0;
    mail.winlink_payload = (flags & kMailFlagWinlinkEnvelope) != 0;
    mail.compressed_payload = (flags & kMailFlagCompressedPayload) != 0;
    mail.priority = decodeMailPriorityFlag(flags);

    if (is_broadcast) {
        if (data.size() < pos + 1) {
            return;
        }
        const uint8_t subj_len = data[pos++];
        if (data.size() < pos + subj_len) {
            return;
        }
        mail.subject.assign(data.begin() + pos, data.begin() + pos + subj_len);
        pos += subj_len;
        if (data.size() > pos) {
            mail.body.assign(data.begin() + pos, data.end());
        }
        mail.to = kMailBroadcastTarget;
    } else {
        if (data.size() < pos + 1) {
            sendMailAck(message_id, false, "Malformed private mail");
            return;
        }
        const uint8_t to_len = data[pos++];
        if (data.size() < pos + to_len + 1) {
            sendMailAck(message_id, false, "Malformed destination");
            return;
        }
        mail.to.assign(data.begin() + pos, data.begin() + pos + to_len);
        pos += to_len;

        const uint8_t subj_len = data[pos++];
        if (data.size() < pos + subj_len) {
            sendMailAck(message_id, false, "Malformed subject");
            return;
        }
        mail.subject.assign(data.begin() + pos, data.begin() + pos + subj_len);
        pos += subj_len;
        if (data.size() > pos) {
            mail.body.assign(data.begin() + pos, data.end());
        }
    }

    std::string wire_body = mail.body;
    if (mail.compressed_payload) {
        std::vector<uint8_t> compressed(wire_body.begin(), wire_body.end());
        LZHCodec codec;
        auto decompressed = codec.decompress(compressed);
        if (decompressed.empty()) {
            if (!is_broadcast && (flags & kMailFlagAckRequired) != 0) {
                sendMailAck(message_id, false, "Compressed payload decode failed");
            }
            return;
        }
        wire_body.assign(decompressed.begin(), decompressed.end());
    }

    if (mail.winlink_payload) {
        std::string decoded_from = mail.from;
        std::string decoded_to = mail.to;
        std::string decoded_subject = mail.subject;
        std::string decoded_body;
        if (!decodeWinlinkEnvelopePayload(wire_body,
                                          decoded_from,
                                          decoded_to,
                                          decoded_subject,
                                          decoded_body)) {
            if (!is_broadcast && (flags & kMailFlagAckRequired) != 0) {
                sendMailAck(message_id, false, "Winlink envelope decode failed");
            }
            return;
        }
        if (!decoded_from.empty()) {
            mail.from = decoded_from;
        }
        if (!decoded_to.empty()) {
            mail.to = decoded_to;
        }
        if (!decoded_subject.empty()) {
            mail.subject = decoded_subject;
        }
        mail.body = decoded_body;
    } else {
        mail.body = wire_body;
    }

    // Dedupe by message_id.
    auto seen_in_memory = [&]() {
        return std::any_of(mail_inbox_.begin(), mail_inbox_.end(),
                           [&](const MailMessage& m) { return m.message_id == message_id; }) ||
               std::any_of(mail_outbox_.begin(), mail_outbox_.end(),
                           [&](const MailMessage& m) { return m.message_id == message_id; });
    };
    if (seen_in_memory()) {
        if (!is_broadcast && (flags & kMailFlagAckRequired) != 0) {
            sendMailAck(message_id, true, "Duplicate");
        }
        return;
    }

    if (store_ && store_->isOpen()) {
        auto existing = store_->getMessageById(message_id);
        if (existing.has_value()) {
            if (!is_broadcast && (flags & kMailFlagAckRequired) != 0) {
                sendMailAck(message_id, true, "Duplicate");
            }
            return;
        }
    }

    std::string decoded_template;
    std::string decoded_body;
    if ((flags & kMailFlagTemplatePayload) != 0 &&
        decodeTrafficTemplatePayload(mail.body, decoded_template, decoded_body)) {
        mail.template_name = decoded_template;
        mail.display_body = decoded_body;
    } else {
        decodeTrafficTemplatePayload(mail.body, mail.template_name, mail.display_body);
    }

    const bool to_email = looksLikeEmailAddress(mail.to);
    const bool to_me = normalizeCallsign(mail.to) == normalizeCallsign(config_.station.callsign);
    if (!is_broadcast && !to_email && !to_me) {
        bool relayed = false;
        // Mesh-delivered packets were already routed by the relay layer;
        // only mail handed to us over the link starts a new relay.
        if (config_.email.hf_store_forward_enabled && via_link) {
            relayed = forwardPacketViaMesh(data, mail.to, message_id);
        }
        if ((flags & kMailFlagAckRequired) != 0) {
            sendMailAck(message_id,
                        relayed,
                        relayed ? "Relayed" : "Not recipient");
        }
        return;
    }

    if (to_email) {
        const std::string normalized_from = normalizeCallsign(mail.from);
        const bool gateway_ready = isEmailGatewayConfigured(config_);
        // The relay allowlist applies to the station on the other end of
        // the radio link, never to the From inside its own envelope.
        const std::string radio_sender = normalizeCallsign(from);
        const bool sender_allowed = via_link && !radio_sender.empty() && radio_sender != "UNKNOWN" &&
                                    senderAllowedForGateway(config_, radio_sender);
        const bool gateway_permitted = !away_mode_active_ || config_.away.allow_email_gateway;
        if (!gateway_ready || !sender_allowed || !gateway_permitted) {
            sendMailAck(message_id, false, "Gateway unavailable");
            return;
        }

        Message outbound_email;
        outbound_email.type = MessageType::Email;
        outbound_email.status = MessageStatus::Pending;
        outbound_email.from_callsign = normalized_from;
        outbound_email.to_callsign = trimSpaces(mail.to);
        outbound_email.subject = mail.subject;
        outbound_email.body = mail.body;
        outbound_email.priority = static_cast<int>(mail.priority);
        outbound_email.created_at = mail.timestamp;
        outbound_email.message_id = message_id;
        if (store_) {
            outbound_email.id = store_->createMessage(outbound_email);
        }
        relayRadioToEmail(outbound_email);
        sendQueuedEmails();
        sendMailAck(message_id, true, "Queued for internet relay");
        return;
    }

    // Relayed mail: the subject carries the marker so it survives the store.
    mail.via = relayed_by;
    mail.subject = app_detail::relayAttributionMarker(relayed_by) + mail.subject;
    mail_inbox_.push_back(mail);
    if (store_ && store_->isOpen()) {
        const int64_t id = store_->createMessage(makeStoreMailMessage(mail, config_.station.callsign, false));
        if (id > 0) {
            mail_inbox_.back().store_id = id;
        }
    }
    persistMailReadState(mail_inbox_.back());
    if (!is_broadcast && (flags & kMailFlagAckRequired) != 0) {
        sendMailAck(message_id, true, "Delivered");
    }

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "New mail from " + from + ": " + mail.subject;  // subject carries any relay marker
    if (mail.is_broadcast) {
        msg.text += " [BROADCAST]";
    }
    msg.is_outgoing = false;
    msg.timestamp = mail.timestamp;
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::sendMail(const std::string& to, const std::string& subject, const std::string& body) {
    if (!modem_) return;

    const std::string to_raw = trimSpaces(to);
    if (to_raw.empty()) {
        return;
    }
    const bool to_email = looksLikeEmailAddress(to_raw);
    const std::string to_field = to_email ? to_raw : normalizeCallsign(to_raw);
    if (to_field.empty() || to_field.size() > kMailMaxAddressLen) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Mail destination is invalid or too long";
        msg.is_outgoing = false;
        msg.timestamp = unixNowSeconds();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    MailMessage mail;
    mail.from = normalizeCallsign(config_.station.callsign);
    mail.to = to_field;
    mail.subject = subject.substr(0, 127);
    mail.body = body;
    mail.display_body = body;
    decodeTrafficTemplatePayload(mail.body, mail.template_name, mail.display_body);
    mail.timestamp = unixNowSeconds();
    mail.read = true;
    mail.is_broadcast = false;
    mail.status = MessageStatus::Pending;
    mail.priority = clampMailPriority(mail_priority_input_);
    mail.winlink_payload = to_email && config_.email.winlink_compat_mode;
    mail.message_id = MessageStore::generateMessageId(mail.from.empty() ? "N0CALL" : mail.from);
    if (mail.message_id.size() > kMailMaxMessageIdLen) {
        mail.message_id.resize(kMailMaxMessageIdLen);
    }

    if (store_ && store_->isOpen()) {
        const int64_t id = store_->createMessage(makeStoreMailMessage(mail, config_.station.callsign, true));
        if (id > 0) {
            mail.store_id = id;
        }
    }

    const ModemStatus status = modem_->getStatus();
    const bool channel_busy = status.busy_detected || status.ptt_active || status.tx_buffer_bytes > 0;
    const float weak_threshold = static_cast<float>(config_.email.weak_signal_snr_threshold_db);
    const bool live_weak = status.snr_db <= weak_threshold;

    bool sent = false;
    if (to_email) {
        const bool connected = status.link_state == LinkState::Connected;
        std::string remote = normalizeCallsign(status.remote_callsign);
        if (remote.empty()) {
            remote = normalizeCallsign(current_chat_partner_);
        }
        const std::string pinned_gateway = normalizeCallsign(auto_gateway_target_);
        const bool connected_gateway = isKnownEmailGateway(remote) ||
                                       (!pinned_gateway.empty() && remote == pinned_gateway);
        if (!remote.empty()) {
            mail.weak_signal_profile = isWeakSignalLink(remote) || live_weak || channel_busy;
        } else if (!pinned_gateway.empty()) {
            mail.weak_signal_profile = isWeakSignalLink(pinned_gateway) || live_weak || channel_busy;
        } else {
            mail.weak_signal_profile = live_weak || channel_busy;
        }

        // If currently linked, try sending via that station even if gateway beacons were missed.
        // The remote station can still return an explicit NACK if gateway relay is unavailable.
        if (!channel_busy && connected && !remote.empty()) {
            sent = sendMailWirePacket(mail);
            if (!connected_gateway) {
                relay_diagnostics_.last_error =
                    "Connected station not marked as gateway; attempted relay anyway";
            }
        } else {
            mail.needs_gateway = true;
            sent = false;
            if (!connected) {
                float score = 0.0f;
                std::string gateway = selectBestGateway(&score);
                if (!gateway.empty()) {
                    auto_gateway_target_ = gateway;
                    mail.weak_signal_profile = isWeakSignalLink(gateway) || live_weak || channel_busy;
                    connectToStation(gateway);
                    last_gateway_connect_attempt_ = std::chrono::steady_clock::now();
                    relay_diagnostics_.last_target = gateway;
                    relay_diagnostics_.last_gateway_score = score;
                    relay_diagnostics_.last_mode = "hf-gateway";
                    relay_diagnostics_.queue_policy_state = "connecting";
                }
            }
        }
    } else {
        std::string direct_remote = normalizeCallsign(modem_->getStatus().remote_callsign);
        if (direct_remote.empty()) {
            direct_remote = normalizeCallsign(current_chat_partner_);
        }
        if (!direct_remote.empty()) {
            mail.weak_signal_profile = isWeakSignalLink(direct_remote);
        }
        mail.weak_signal_profile = mail.weak_signal_profile || live_weak || channel_busy;
        sent = !channel_busy && sendMailWirePacket(mail);
        if (!sent && config_.email.hf_store_forward_enabled && !channel_busy) {
            std::vector<uint8_t> wire_packet;
            if (buildReliableMailWirePacket(mail, config_, &wire_packet)) {
                sent = forwardPacketViaMesh(wire_packet, to_field, mail.message_id);
            }
        }
    }

    if (to_email && !sent) {
        mail.needs_gateway = true;
    }

    if (sent) {
        mail.status = MessageStatus::Sent;
        if (store_ && store_->isOpen() && mail.store_id > 0) {
            store_->markAsSent(mail.store_id);
        }
    }

    mail_outbox_.push_back(mail);

    ChatMessage msg;
    msg.from = "SYSTEM";
    if (to_email) {
        msg.text = sent
            ? ("Email relay message sent to gateway for " + to_field + " [MID " + mail.message_id + "]")
            : ("Email relay queued for gateway delivery to " + to_field + " [MID " + mail.message_id + "]");
    } else {
        msg.text = sent
            ? ("Mail sent to " + to_field + " [MID " + mail.message_id + "]")
            : ((status.link_state == LinkState::Connected && channel_busy)
                   ? ("Mail queued (channel busy) for " + to_field + " [MID " + mail.message_id + "]")
                   : ("Mail queued (link unavailable) for " + to_field + " [MID " + mail.message_id + "]"));
    }
    msg.is_outgoing = false;
    msg.timestamp = unixNowSeconds();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::sendBroadcastMail(const std::string& subject, const std::string& body) {
    if (!modem_) return;

    MailMessage mail;
    mail.from = normalizeCallsign(config_.station.callsign);
    mail.to = kMailBroadcastTarget;
    mail.subject = subject.substr(0, 127);
    mail.body = body;
    mail.display_body = body;
    decodeTrafficTemplatePayload(mail.body, mail.template_name, mail.display_body);
    mail.timestamp = unixNowSeconds();
    mail.read = true;
    mail.is_broadcast = true;
    mail.status = MessageStatus::Sent;
    mail.priority = clampMailPriority(mail_priority_input_);
    mail.message_id = MessageStore::generateMessageId(mail.from.empty() ? "N0CALL" : mail.from);
    if (mail.message_id.size() > kMailMaxMessageIdLen) {
        mail.message_id.resize(kMailMaxMessageIdLen);
    }

    const bool sent = sendMailWirePacket(mail);
    if (!sent) {
        mail.status = MessageStatus::Failed;
    }

    if (store_ && store_->isOpen()) {
        const int64_t id = store_->createMessage(makeStoreMailMessage(mail, config_.station.callsign, true));
        if (id > 0) {
            mail.store_id = id;
            if (sent) {
                store_->markAsSent(id);
            } else {
                store_->markAsFailed(id);
            }
        }
    }
    mail_outbox_.push_back(mail);

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = sent
        ? ("Broadcast mail sent: " + mail.subject + " [MID " + mail.message_id + "]")
        : ("Broadcast mail send failed: modem not connected or TX failed [MID " + mail.message_id + "]");
    msg.is_outgoing = false;
    msg.timestamp = unixNowSeconds();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::processMailReceived(const std::string& from, const std::vector<uint8_t>& data, bool via_link,
                              const std::string& relayed_by) {
    if (data.size() < 2) return;

    // Legacy packets carry no message id: suppress repeats of the same
    // packet from the same sender inside a window.
    {
        const int64_t now = unixNowSeconds();
        for (auto it = legacy_mail_seen_.begin(); it != legacy_mail_seen_.end();) {
            if (now - it->second > LEGACY_MAIL_DEDUP_WINDOW_SEC) {
                it = legacy_mail_seen_.erase(it);
            } else {
                ++it;
            }
        }
        const std::string key = normalizeCallsign(from) + "|" +
            std::to_string(std::hash<std::string>{}(std::string(data.begin(), data.end())));
        if (legacy_mail_seen_.count(key) > 0) {
            return;
        }
        if (legacy_mail_seen_.size() >= 512) {
            legacy_mail_seen_.clear();
        }
        legacy_mail_seen_[key] = now;
    }

    // Backward-compatible decoder for legacy 0xE2/0xE3 packets.
    const uint8_t mail_type = data[0];
    MailMessage mail;
    mail.message_id = MessageStore::generateMessageId(normalizeCallsign(from));
    mail.from = normalizeCallsign(from);
    mail.timestamp = unixNowSeconds();
    mail.read = false;
    mail.status = MessageStatus::Delivered;

    if (mail_type == 0xE2 && data.size() >= 4) {
        uint8_t to_len = data[1];
        if (data.size() < 2 + to_len + 1) return;
        mail.to.assign(data.begin() + 2, data.begin() + 2 + to_len);

        size_t subj_pos = 2 + to_len;
        uint8_t subj_len = data[subj_pos];
        if (data.size() < subj_pos + 1 + subj_len) return;
        mail.subject.assign(data.begin() + subj_pos + 1, data.begin() + subj_pos + 1 + subj_len);

        size_t body_pos = subj_pos + 1 + subj_len;
        if (data.size() > body_pos) {
            mail.body.assign(data.begin() + body_pos, data.end());
        }
        mail.is_broadcast = false;

        const bool to_email = looksLikeEmailAddress(mail.to);
        const bool to_me = normalizeCallsign(mail.to) == normalizeCallsign(config_.station.callsign);
        if (!to_email && !to_me) {
            return;
        }
        if (to_email) {
            if (!via_link || !isEmailGatewayConfigured(config_) ||
                !senderAllowedForGateway(config_, mail.from) ||
                (away_mode_active_ && !config_.away.allow_email_gateway)) {
                return;
            }
            Message outbound_email;
            outbound_email.type = MessageType::Email;
            outbound_email.status = MessageStatus::Pending;
            outbound_email.from_callsign = mail.from;
            outbound_email.to_callsign = trimSpaces(mail.to);
            outbound_email.subject = mail.subject;
            outbound_email.body = mail.body;
            outbound_email.priority = static_cast<int>(mail.priority);
            outbound_email.created_at = mail.timestamp;
            outbound_email.message_id = mail.message_id;
            if (store_) {
                outbound_email.id = store_->createMessage(outbound_email);
            }
            relayRadioToEmail(outbound_email);
            sendQueuedEmails();
            return;
        }
    } else if (mail_type == 0xE3 && data.size() >= 2) {
        uint8_t subj_len = data[1];
        if (data.size() < 2 + subj_len) return;
        mail.subject.assign(data.begin() + 2, data.begin() + 2 + subj_len);
        mail.to = kMailBroadcastTarget;
        size_t body_pos = 2 + subj_len;
        if (data.size() > body_pos) {
            mail.body.assign(data.begin() + body_pos, data.end());
        }
        mail.is_broadcast = true;
    } else {
        return;
    }

    decodeTrafficTemplatePayload(mail.body, mail.template_name, mail.display_body);
    // Relayed mail: the subject carries the marker so it survives the store.
    mail.via = relayed_by;
    mail.subject = app_detail::relayAttributionMarker(relayed_by) + mail.subject;
    mail_inbox_.push_back(mail);
    if (store_ && store_->isOpen()) {
        const int64_t id = store_->createMessage(makeStoreMailMessage(mail, config_.station.callsign, false));
        if (id > 0) {
            mail_inbox_.back().store_id = id;
        }
    }
    persistMailReadState(mail_inbox_.back());

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "New mail from " + from + ": " + mail.subject;  // subject carries any relay marker
    if (mail.is_broadcast) {
        msg.text += " [BROADCAST]";
    }
    msg.is_outgoing = false;
    msg.timestamp = mail.timestamp;
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::loadMailFromStore() {
    mail_inbox_.clear();
    mail_outbox_.clear();

    if (!store_ || !store_->isOpen()) {
        return;
    }

    const std::string my_call = normalizeCallsign(config_.station.callsign);
    if (my_call.empty()) {
        return;
    }

    std::map<std::string, bool> read_state;
    read_state = store_->loadMailReadStates(10000);

    MessageFilter filter;
    filter.limit = 5000;
    auto messages = store_->getMessages(filter);
    if (messages.empty()) {
        return;
    }

    std::sort(messages.begin(), messages.end(), [](const Message& a, const Message& b) {
        if (a.created_at != b.created_at) {
            return a.created_at < b.created_at;
        }
        return a.id < b.id;
    });

    for (const auto& msg : messages) {
        if (msg.type != MessageType::Private &&
            msg.type != MessageType::Bulletin &&
            msg.type != MessageType::Email) {
            continue;
        }

        const std::string from = normalizeCallsign(msg.from_callsign);
        const std::string to = normalizeCallsign(msg.to_callsign);
        const bool from_me = (from == my_call);
        const bool to_me = (to == my_call);
        const bool to_all = (to == kMailBroadcastTarget);

        if (!from_me && !to_me && !to_all) {
            continue;
        }

        bool is_read = true;
        if (!msg.message_id.empty()) {
            auto it = read_state.find(msg.message_id);
            if (it != read_state.end()) {
                is_read = it->second;
            } else if (!from_me) {
                is_read = false;
            }
        } else if (!from_me) {
            is_read = false;
        }

        MailMessage mail = makeUiMailMessage(msg, is_read);
        if (mail.display_body.empty()) {
            decodeTrafficTemplatePayload(mail.body, mail.template_name, mail.display_body);
        }
        if (looksLikeEmailAddress(mail.to)) {
            mail.winlink_payload = config_.email.winlink_compat_mode;
            if (mail.status == MessageStatus::Pending || mail.status == MessageStatus::Sent) {
                mail.needs_gateway = true;
            }
        }
        if (from_me) {
            mail_outbox_.push_back(std::move(mail));
        } else {
            mail_inbox_.push_back(std::move(mail));
        }
    }
}

void App::saveMailToStore() {
    if (!store_ || !store_->isOpen()) {
        return;
    }

    auto persist = [this](MailMessage& mail, bool outgoing) {
        if (mail.store_id <= 0) {
            const int64_t id = store_->createMessage(
                makeStoreMailMessage(mail, config_.station.callsign, outgoing));
            if (id > 0) {
                mail.store_id = id;
            }
        } else {
            Message updated = makeStoreMailMessage(mail, config_.station.callsign, outgoing);
            updated.id = mail.store_id;
            store_->updateMessage(updated);
        }
        persistMailReadState(mail);
    };

    for (auto& mail : mail_outbox_) {
        persist(mail, true);
    }
    for (auto& mail : mail_inbox_) {
        persist(mail, false);
    }
}

bool App::deleteStoredMessage(int64_t store_id, const std::string& message_id) {
    if (!store_ || !store_->isOpen()) {
        return false;
    }

    int64_t id = store_id;
    if (id <= 0 && !message_id.empty()) {
        auto by_mid = store_->getMessageById(message_id);
        if (by_mid.has_value()) {
            id = by_mid->id;
        }
    }
    if (id <= 0) {
        return false;
    }
    return store_->deleteMessage(id);
}

// ============================================================================
// Weather Information Functions
// ============================================================================

bool App::isStationLinkConnected(bool refresh) {
    if (!modem_ || !modem_->isConnected()) {
        return false;
    }

    if (modem_->getStatus().link_state == LinkState::Connected) {
        return true;
    }

    // Fallback: async CONNECTED notifications update this immediately.
    if (!current_chat_partner_.empty()) {
        return true;
    }

    if (!refresh) {
        return false;
    }

    // Ask for a fresh state sample so a retry sees it, but do not wait for
    // it on the GUI thread: STATUS is already polled every 500 ms and link
    // changes arrive as events, so the cached state is at most that old.
    modem_->sendCommand("STATE");
    return false;
}

void App::requestWeatherForecast(const std::string& location) {
    if (!modem_) return;

    // Must be connected to a remote station
    if (!isStationLinkConnected(true)) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Weather request failed: Not connected to a remote station";
        msg.is_outgoing = false;
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    // Build weather request packet: 0xE4 | 0x80 | type(1) | location
    // 0x80 flag indicates REQUEST (vs response)
    std::vector<uint8_t> packet;
    packet.push_back(0xE4);  // Weather marker
    packet.push_back(0x81);  // Type: Forecast REQUEST (0x80 | 0x01)

    packet.insert(packet.end(), location.begin(), location.end());

    modem_->sendData(packet);

    weather_request_pending_ = true;
    weather_request_time_ = std::chrono::steady_clock::now();

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "Requesting weather forecast for " + location + "...";
    msg.is_outgoing = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::requestMETAR(const std::string& airport_code) {
    if (!modem_) return;

    // Must be connected to a remote station
    if (!isStationLinkConnected(true)) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Weather request failed: Not connected to a remote station";
        msg.is_outgoing = false;
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    // Build METAR request packet: 0xE4 | 0x82 | airport_code
    std::vector<uint8_t> packet;
    packet.push_back(0xE4);  // Weather marker
    packet.push_back(0x82);  // Type: METAR REQUEST (0x80 | 0x02)

    std::string code = airport_code;
    std::transform(code.begin(), code.end(), code.begin(), ::toupper);
    packet.insert(packet.end(), code.begin(), code.end());

    modem_->sendData(packet);

    weather_request_pending_ = true;
    weather_request_time_ = std::chrono::steady_clock::now();

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "Requesting METAR for " + code + "...";
    msg.is_outgoing = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::requestTAF(const std::string& airport_code) {
    if (!modem_) return;

    // Must be connected to a remote station
    if (!isStationLinkConnected(true)) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Weather request failed: Not connected to a remote station";
        msg.is_outgoing = false;
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    // Build TAF request packet: 0xE4 | 0x83 | airport_code
    std::vector<uint8_t> packet;
    packet.push_back(0xE4);  // Weather marker
    packet.push_back(0x83);  // Type: TAF REQUEST (0x80 | 0x03)

    std::string code = airport_code;
    std::transform(code.begin(), code.end(), code.begin(), ::toupper);
    packet.insert(packet.end(), code.begin(), code.end());

    modem_->sendData(packet);

    weather_request_pending_ = true;
    weather_request_time_ = std::chrono::steady_clock::now();

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "Requesting TAF for " + code + "...";
    msg.is_outgoing = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::requestMarineWeather(const std::string& zone) {
    if (!modem_) return;

    // Must be connected to a remote station
    if (!isStationLinkConnected(true)) {
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Weather request failed: Not connected to a remote station";
        msg.is_outgoing = false;
        msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;
        return;
    }

    // Build marine weather request packet: 0xE4 | 0x84 | zone
    std::vector<uint8_t> packet;
    packet.push_back(0xE4);  // Weather marker
    packet.push_back(0x84);  // Type: Marine REQUEST (0x80 | 0x04)

    std::string zone_code = zone;
    std::transform(zone_code.begin(), zone_code.end(), zone_code.begin(), ::toupper);
    packet.insert(packet.end(), zone_code.begin(), zone_code.end());

    modem_->sendData(packet);

    weather_request_pending_ = true;
    weather_request_time_ = std::chrono::steady_clock::now();

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "Requesting marine weather for zone " + zone_code + "...";
    msg.is_outgoing = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;
}

void App::processWeatherMessage(const std::string& from, const std::vector<uint8_t>& data) {
    // Weather packet format: 0xE4 | type_byte | ...
    // If type_byte has 0x80 bit set, it's a REQUEST, otherwise RESPONSE
    if (data.size() < 2) return;

    uint8_t type_byte = data[1];
    bool is_request = (type_byte & 0x80) != 0;
    uint8_t wx_type = type_byte & 0x0F;

    if (is_request) {
        // This is a weather REQUEST from the connected station
        // Extract the query (location/airport/zone)
        std::string query;
        if (data.size() > 2) {
            query = std::string(data.begin() + 2, data.end());
        }
        // Requests are answered over the link, so only the linked peer
        // may make one.
        const std::string requester = normalizeCallsign(from);
        if (requester.empty() || requester != linkedPeerCallsign()) {
            return;
        }
        handleWeatherRequest(wx_type, query, requester);
    } else {
        // This is a weather RESPONSE
        // Format: 0xE4 | type | location_len | location | report
        if (data.size() < 3) return;

        weather_request_pending_ = false;

        uint8_t loc_len = data[2];
        if (data.size() < 3 + loc_len) return;

        std::string location(data.begin() + 3, data.begin() + 3 + loc_len);
        std::string report;
        if (data.size() > 3 + loc_len) {
            report = std::string(data.begin() + 3 + loc_len, data.end());
        }
        std::string sender = normalizeCallsign(from);
        if (sender.empty()) {
            sender = "UNKNOWN";
        }

        WeatherReport wx_report;
        wx_report.location = location;
        wx_report.report = report;
        wx_report.source_station = sender;
        wx_report.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        wx_report.is_request_pending = false;

        switch (wx_type) {
            case 0x01: wx_report.type = "FORECAST"; break;
            case 0x02: wx_report.type = "METAR"; break;
            case 0x03: wx_report.type = "TAF"; break;
            case 0x04: wx_report.type = "MARINE"; break;
            case 0x05: wx_report.type = "ALERT"; break;
            default: wx_report.type = "UNKNOWN"; break;
        }
        if (wx_report.type == "ALERT") {
            wx_report.severity = inferWeatherAlertSeverity(report);
            wx_report.expires_utc = extractWeatherAlertExpiryUtc(report);
        } else {
            wx_report.severity = "N/A";
            wx_report.expires_utc = "N/A";
        }

        // Add to reports (limit to 50)
        weather_reports_.insert(weather_reports_.begin(), wx_report);
        while (weather_reports_.size() > 50) {
            weather_reports_.pop_back();
        }

        // System notification
        ChatMessage msg;
        msg.from = "SYSTEM";
        msg.text = "Weather " + wx_report.type + " received from " + sender + " for " + location;
        if (wx_report.type == "ALERT") {
            std::string excerpt = report;
            if (excerpt.size() > 120) {
                excerpt.resize(120);
                excerpt += "...";
            }
            msg.text = "[WEATHER ALERT] " + location +
                       " | From " + sender +
                       " | UTC " + utcDateTimeStringFromUnix(wx_report.timestamp) +
                       " | Severity " + wx_report.severity +
                       " | Expires " + wx_report.expires_utc +
                       " | " + excerpt;
        }
        msg.is_outgoing = false;
        msg.timestamp = wx_report.timestamp;
        chat_history_.push_back(msg);
        chat_scroll_to_bottom_ = true;

    }
}

void App::handleWeatherRequest(uint8_t request_type, const std::string& query, const std::string& requester) {
    // Handle incoming weather request from connected station
    // Fetch data from internet asynchronously and send response

    std::string type_name;
    switch (request_type) {
        case 0x01: type_name = "FORECAST"; break;
        case 0x02: type_name = "METAR"; break;
        case 0x03: type_name = "TAF"; break;
        case 0x04: type_name = "MARINE"; break;
        default: type_name = "UNKNOWN"; break;
    }

    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = "Weather " + type_name + " request from " + requester + " for: " +
               app_detail::sanitizeRemoteText(query, 64);
    msg.is_outgoing = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history_.push_back(msg);
    chat_scroll_to_bottom_ = true;

    // Start async fetch
    fetchWeatherAsync(request_type, query, requester);
}

void App::sendWeatherResponse(uint8_t type, const std::string& location, const std::string& report) {
    if (!modem_) return;

    // Build response packet: 0xE4 | type | location_len | location | report
    std::vector<uint8_t> packet;
    packet.push_back(0xE4);  // Weather marker
    packet.push_back(type);  // Type (no 0x80 flag = response)

    // Location (length-prefixed)
    uint8_t loc_len = static_cast<uint8_t>(std::min(location.size(), size_t(255)));
    packet.push_back(loc_len);
    packet.insert(packet.end(), location.begin(), location.begin() + loc_len);

    // Report data
    packet.insert(packet.end(), report.begin(), report.end());

    modem_->sendData(packet);
}

// Convert Maidenhead grid square to lat/lon
bool App::gridSquareToLatLon(const std::string& grid, double& lat, double& lon) {
    if (grid.size() < 4) return false;

    std::string g = grid;
    std::transform(g.begin(), g.end(), g.begin(), ::toupper);

    // Field (first two chars: A-R)
    if (g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R') return false;
    int field_lon = g[0] - 'A';
    int field_lat = g[1] - 'A';

    // Square (next two chars: 0-9)
    if (g[2] < '0' || g[2] > '9' || g[3] < '0' || g[3] > '9') return false;
    int square_lon = g[2] - '0';
    int square_lat = g[3] - '0';

    // Calculate base coordinates
    lon = (field_lon * 20.0) - 180.0 + (square_lon * 2.0) + 1.0;  // Center of square
    lat = (field_lat * 10.0) - 90.0 + (square_lat * 1.0) + 0.5;   // Center of square

    // Subsquare (optional, chars 5-6: a-x)
    if (grid.size() >= 6) {
        char sub_lon = std::tolower(g[4]);
        char sub_lat = std::tolower(g[5]);
        if (sub_lon >= 'a' && sub_lon <= 'x' && sub_lat >= 'a' && sub_lat <= 'x') {
            lon = (field_lon * 20.0) - 180.0 + (square_lon * 2.0) + ((sub_lon - 'a') * (2.0/24.0)) + (1.0/24.0);
            lat = (field_lat * 10.0) - 90.0 + (square_lat * 1.0) + ((sub_lat - 'a') * (1.0/24.0)) + (0.5/24.0);
        }
    }

    return true;
}

// Simple HTTP GET using sockets (cross-platform, no curl dependency)
std::string App::httpGet(const std::string& host, const std::string& path, bool use_ssl) {
    std::string current_host = host;
    std::string current_path = path.empty() ? "/" : path;
    bool current_ssl = use_ssl;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    auto trim_ascii = [](const std::string& s) -> std::string {
        size_t start = 0;
        while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
            ++start;
        }
        size_t end = s.size();
        while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
            --end;
        }
        return s.substr(start, end - start);
    };

    auto to_lower_ascii = [](std::string s) -> std::string {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };

    auto find_header_value = [&](const std::string& headers, const std::string& header_name) -> std::string {
        const std::string wanted = to_lower_ascii(header_name);
        size_t line_start = 0;
        while (line_start < headers.size()) {
            size_t line_end = headers.find("\r\n", line_start);
            std::string line = headers.substr(
                line_start,
                (line_end == std::string::npos) ? std::string::npos : (line_end - line_start));
            if (line.empty()) {
                break;
            }

            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                if (to_lower_ascii(key) == wanted) {
                    return trim_ascii(line.substr(colon + 1));
                }
            }

            if (line_end == std::string::npos) {
                break;
            }
            line_start = line_end + 2;
        }
        return "";
    };

    auto parse_status_code = [](const std::string& headers) -> int {
        size_t line_end = headers.find("\r\n");
        std::string status_line = headers.substr(0, line_end);
        size_t sp1 = status_line.find(' ');
        if (sp1 == std::string::npos) {
            return 0;
        }
        size_t sp2 = status_line.find(' ', sp1 + 1);
        std::string code_str = status_line.substr(
            sp1 + 1,
            (sp2 == std::string::npos) ? std::string::npos : (sp2 - (sp1 + 1)));
        try {
            return std::stoi(code_str);
        } catch (...) {
            return 0;
        }
    };

    auto apply_redirect = [&](const std::string& location) -> bool {
        std::string loc = trim_ascii(location);
        if (loc.empty()) {
            return false;
        }

        if (loc.rfind("http://", 0) == 0 || loc.rfind("https://", 0) == 0) {
            bool new_ssl = (loc.rfind("https://", 0) == 0);
            size_t scheme_end = loc.find("://");
            if (scheme_end == std::string::npos) {
                return false;
            }
            size_t authority_start = scheme_end + 3;
            size_t path_start = loc.find('/', authority_start);
            std::string authority = (path_start == std::string::npos)
                ? loc.substr(authority_start)
                : loc.substr(authority_start, path_start - authority_start);
            if (authority.empty()) {
                return false;
            }

            // Drop user-info and port components, keep only host.
            size_t at = authority.rfind('@');
            if (at != std::string::npos) {
                authority = authority.substr(at + 1);
            }
            size_t colon = authority.find(':');
            if (colon != std::string::npos) {
                authority = authority.substr(0, colon);
            }
            if (authority.empty()) {
                return false;
            }

            current_host = authority;
            current_path = (path_start == std::string::npos) ? "/" : loc.substr(path_start);
            current_ssl = new_ssl;
            return true;
        }

        if (loc.rfind("//", 0) == 0) {
            size_t path_start = loc.find('/', 2);
            std::string authority = (path_start == std::string::npos) ? loc.substr(2)
                                                                       : loc.substr(2, path_start - 2);
            if (authority.empty()) {
                return false;
            }
            size_t colon = authority.find(':');
            if (colon != std::string::npos) {
                authority = authority.substr(0, colon);
            }
            if (authority.empty()) {
                return false;
            }
            current_host = authority;
            current_path = (path_start == std::string::npos) ? "/" : loc.substr(path_start);
            return true;
        }

        // Relative redirect on same host.
        current_path = (loc[0] == '/') ? loc : ("/" + loc);
        return true;
    };

    constexpr int kMaxRedirects = 3;
    constexpr size_t kMaxHttpResponseBytes = 262144;

    for (int redirect_count = 0; redirect_count <= kMaxRedirects; ++redirect_count) {
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        std::string port = current_ssl ? "443" : "80";
        if (getaddrinfo(current_host.c_str(), port.c_str(), &hints, &res) != 0) {
            return "";
        }

        int sock = -1;
        for (auto* ai = res; ai != nullptr; ai = ai->ai_next) {
            int candidate = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (candidate < 0) {
                continue;
            }

            // Set timeout
            struct timeval tv;
            tv.tv_sec = 10;
            tv.tv_usec = 0;
            setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
            setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

            if (::connect(candidate, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
                sock = candidate;
                break;
            }
#ifdef _WIN32
            closesocket(candidate);
#else
            close(candidate);
#endif
        }
        freeaddrinfo(res);
        if (sock < 0) {
            return "";
        }

#ifdef SKYWAVE_LINK_HAS_OPENSSL
        SSL_CTX* ctx = nullptr;
        SSL* ssl = nullptr;
#endif

        if (current_ssl) {
#ifdef SKYWAVE_LINK_HAS_OPENSSL
            ctx = SSL_CTX_new(TLS_client_method());
            if (!ctx) {
#ifdef _WIN32
                closesocket(sock);
#else
                close(sock);
#endif
                return "";
            }

            // Enable certificate verification using system trust store
            SSL_CTX_set_default_verify_paths(ctx);
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);

            ssl = SSL_new(ctx);
            if (!ssl) {
                SSL_CTX_free(ctx);
#ifdef _WIN32
                closesocket(sock);
#else
                close(sock);
#endif
                return "";
            }
            SSL_set_fd(ssl, sock);
            SSL_set_tlsext_host_name(ssl, current_host.c_str());
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
            if (SSL_set1_host(ssl, current_host.c_str()) != 1) {
                SSL_free(ssl);
                SSL_CTX_free(ctx);
#ifdef _WIN32
                closesocket(sock);
#else
                close(sock);
#endif
                return "";
            }
#else
            X509_VERIFY_PARAM* verify = SSL_get0_param(ssl);
            if (verify == nullptr ||
                X509_VERIFY_PARAM_set1_host(verify, current_host.c_str(), 0) != 1) {
                SSL_free(ssl);
                SSL_CTX_free(ctx);
#ifdef _WIN32
                closesocket(sock);
#else
                close(sock);
#endif
                return "";
            }
#endif

            if (SSL_connect(ssl) <= 0) {
                SSL_free(ssl);
                SSL_CTX_free(ctx);
#ifdef _WIN32
                closesocket(sock);
#else
                close(sock);
#endif
                return "";
            }
            if (SSL_get_verify_result(ssl) != X509_V_OK) {
                SSL_free(ssl);
                SSL_CTX_free(ctx);
#ifdef _WIN32
                closesocket(sock);
#else
                close(sock);
#endif
                return "";
            }
#else
#ifdef _WIN32
            closesocket(sock);
#else
            close(sock);
#endif
            return "";
#endif
        }

        // Build HTTP request
        std::string request = "GET " + current_path + " HTTP/1.1\r\n";
        request += "Host: " + current_host + "\r\n";
        request += "User-Agent: Skywave-Link/1.0\r\n";
        request += "Accept: */*\r\n";
        request += "Connection: close\r\n\r\n";

        if (current_ssl) {
#ifdef SKYWAVE_LINK_HAS_OPENSSL
            if (SSL_write(ssl, request.c_str(), static_cast<int>(request.size())) <= 0) {
                SSL_shutdown(ssl);
                SSL_free(ssl);
                SSL_CTX_free(ctx);
#ifdef _WIN32
                closesocket(sock);
#else
                close(sock);
#endif
                return "";
            }
#endif
        } else {
            if (::send(sock, request.c_str(), static_cast<int>(request.size()), 0) <= 0) {
#ifdef _WIN32
                closesocket(sock);
#else
                close(sock);
#endif
                return "";
            }
        }

        // Read response
        char buffer[4096];
        int bytes_read;
        std::string response;

        while (true) {
            if (current_ssl) {
#ifdef SKYWAVE_LINK_HAS_OPENSSL
                bytes_read = SSL_read(ssl, buffer, sizeof(buffer) - 1);
#else
                bytes_read = -1;
#endif
            } else {
                bytes_read = recv(sock, buffer, sizeof(buffer) - 1, 0);
            }

            if (bytes_read <= 0) break;
            buffer[bytes_read] = '\0';
            response += buffer;

            // Limit response size
            if (response.size() > kMaxHttpResponseBytes) break;
        }

        if (current_ssl) {
#ifdef SKYWAVE_LINK_HAS_OPENSSL
            SSL_shutdown(ssl);
            SSL_free(ssl);
            SSL_CTX_free(ctx);
#endif
        }

#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif

        // Extract headers and body
        size_t header_end = response.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            return "";
        }

        std::string headers = response.substr(0, header_end);
        std::string body = response.substr(header_end + 4);
        int status_code = parse_status_code(headers);

        const bool is_redirect =
            (status_code == 301 || status_code == 302 ||
             status_code == 303 || status_code == 307 || status_code == 308);
        if (is_redirect && redirect_count < kMaxRedirects) {
            std::string location = find_header_value(headers, "location");
            if (!location.empty() && apply_redirect(location)) {
                continue;
            }
        }

        return body;
    }

    return "";
}

// Parse and format weather responses for radio transmission
std::string App::parseWeatherResponse(uint8_t type, const std::string& raw_response) {
    if (raw_response.empty()) {
        return "No data available";
    }

    switch (type) {
        case 0x02:  // METAR - already in raw format from aviation API
        case 0x03:  // TAF - already in raw format
            // These come as plain text, just clean up
            {
                std::string cleaned = raw_response;
                // Remove extra whitespace
                while (cleaned.find("  ") != std::string::npos) {
                    size_t pos = cleaned.find("  ");
                    cleaned.replace(pos, 2, " ");
                }
                // Limit length but try to keep complete
                if (cleaned.size() > 800) {
                    cleaned = cleaned.substr(0, 800);
                }
                return cleaned;
            }

        case 0x01:  // Forecast - parse JSON from NWS API
            return extractForecastText(raw_response);

        case 0x04:  // Marine - parse JSON from NWS API
            return extractForecastText(raw_response);

        default:
            return raw_response.substr(0, 800);
    }
}

// Extract forecast text from NWS API JSON response
std::string App::extractForecastText(const std::string& json) {
    // Simple JSON parsing to extract forecast periods
    // Look for "detailedForecast" fields
    std::string result;

    // Find "periods" array
    size_t periods_pos = json.find("\"periods\"");
    if (periods_pos == std::string::npos) {
        // Try to find error message
        size_t detail_pos = json.find("\"detail\"");
        if (detail_pos != std::string::npos) {
            size_t start = json.find("\"", detail_pos + 9) + 1;
            size_t end = json.find("\"", start);
            if (start != std::string::npos && end != std::string::npos) {
                return json.substr(start, end - start);
            }
        }
        return "Could not parse forecast data";
    }

    // Extract first 2-3 periods
    int periods_found = 0;
    size_t search_pos = periods_pos;

    while (periods_found < 3) {
        // Find next period name
        size_t name_pos = json.find("\"name\"", search_pos);
        if (name_pos == std::string::npos) break;

        size_t name_start = json.find("\"", name_pos + 7) + 1;
        size_t name_end = json.find("\"", name_start);
        if (name_start == std::string::npos || name_end == std::string::npos) break;

        std::string name = json.substr(name_start, name_end - name_start);

        // Find detailedForecast
        size_t detail_pos = json.find("\"detailedForecast\"", name_end);
        if (detail_pos == std::string::npos) break;

        size_t detail_start = json.find("\"", detail_pos + 19) + 1;
        size_t detail_end = json.find("\"", detail_start);
        if (detail_start == std::string::npos || detail_end == std::string::npos) break;

        std::string forecast = json.substr(detail_start, detail_end - detail_start);

        // Add to result
        if (!result.empty()) result += "\n\n";
        result += name + ": " + forecast;

        search_pos = detail_end;
        periods_found++;
    }

    if (result.empty()) {
        return "Could not extract forecast text";
    }

    // Limit total size
    if (result.size() > 1500) {
        result = result.substr(0, 1500) + "...";
    }

    return result;
}

// Async weather fetch to avoid blocking UI
// Blocking fetch of one weather product. Static: it touches no App state,
// so a worker thread can run it after the App is gone.
std::string App::fetchWeatherReport(uint8_t type, const std::string& query) {
    std::string result;
    std::string host, path;

    auto trim = [](const std::string& s) -> std::string {
        size_t start = 0;
        while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
            ++start;
        }
        size_t end = s.size();
        while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
            --end;
        }
        return s.substr(start, end - start);
    };

    auto parse_lat_lon = [&trim](const std::string& text, double& lat, double& lon) -> bool {
        size_t comma = text.find(',');
        if (comma == std::string::npos) {
            return false;
        }
        std::string lat_str = trim(text.substr(0, comma));
        std::string lon_str = trim(text.substr(comma + 1));
        if (lat_str.empty() || lon_str.empty()) {
            return false;
        }

        try {
            size_t lat_used = 0;
            size_t lon_used = 0;
            double parsed_lat = std::stod(lat_str, &lat_used);
            double parsed_lon = std::stod(lon_str, &lon_used);
            if (lat_used != lat_str.size() || lon_used != lon_str.size()) {
                return false;
            }
            if (parsed_lat < -90.0 || parsed_lat > 90.0 ||
                parsed_lon < -180.0 || parsed_lon > 180.0) {
                return false;
            }
            lat = parsed_lat;
            lon = parsed_lon;
            return true;
        } catch (...) {
            return false;
        }
    };

    const auto url_encode = [](const std::string& value) { return app_detail::urlEncode(value); };

    auto geocode_location = [&](const std::string& text, double& lat, double& lon) -> bool {
        std::string geo_host = "nominatim.openstreetmap.org";
        std::string geo_path = "/search?format=jsonv2&limit=1&q=" + url_encode(text);
        std::string geo_response = httpGet(geo_host, geo_path, true);
        if (geo_response.empty()) {
            return false;
        }

        const std::string lat_key = "\"lat\":\"";
        const std::string lon_key = "\"lon\":\"";
        size_t lat_pos = geo_response.find(lat_key);
        size_t lon_pos = geo_response.find(lon_key);
        if (lat_pos == std::string::npos || lon_pos == std::string::npos) {
            return false;
        }

        size_t lat_start = lat_pos + lat_key.size();
        size_t lon_start = lon_pos + lon_key.size();
        size_t lat_end = geo_response.find('"', lat_start);
        size_t lon_end = geo_response.find('"', lon_start);
        if (lat_end == std::string::npos || lon_end == std::string::npos) {
            return false;
        }

        try {
            lat = std::stod(geo_response.substr(lat_start, lat_end - lat_start));
            lon = std::stod(geo_response.substr(lon_start, lon_end - lon_start));
            return (lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0);
        } catch (...) {
            return false;
        }
    };

    auto format_coords = [](double lat, double lon) -> std::string {
        // NWS /points endpoint redirects high-precision coordinates (301).
        // Use 4-decimal precision to request a supported point directly.
        std::ostringstream oss;
        oss.setf(std::ios::fixed);
        oss << std::setprecision(4) << lat << "," << lon;
        return oss.str();
    };

    auto extract_json_string = [](const std::string& json, size_t quote_pos,
                                  std::string& out, size_t* end_quote_pos = nullptr) -> bool {
        if (quote_pos == std::string::npos || quote_pos >= json.size() || json[quote_pos] != '"') {
            return false;
        }

        out.clear();
        bool escaped = false;
        for (size_t i = quote_pos + 1; i < json.size(); ++i) {
            const char c = json[i];
            if (escaped) {
                switch (c) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u':
                        if (i + 4 < json.size()) {
                            i += 4;
                        }
                        out.push_back('?');
                        break;
                    default:
                        out.push_back(c);
                        break;
                }
                escaped = false;
                continue;
            }

            if (c == '\\') {
                escaped = true;
                continue;
            }
            if (c == '"') {
                if (end_quote_pos) {
                    *end_quote_pos = i;
                }
                return true;
            }
            out.push_back(c);
        }
        return false;
    };

    auto find_json_string_field = [&](const std::string& json, const std::string& key,
                                      std::string& value) -> bool {
        const std::string pattern = "\"" + key + "\"";
        size_t key_pos = json.find(pattern);
        if (key_pos == std::string::npos) {
            return false;
        }
        size_t colon = json.find(':', key_pos + pattern.size());
        if (colon == std::string::npos) {
            return false;
        }
        size_t quote = json.find('"', colon + 1);
        if (quote == std::string::npos) {
            return false;
        }
        return extract_json_string(json, quote, value, nullptr);
    };

    auto find_first_json_array_string = [&](const std::string& json, const std::string& key,
                                            std::string& value) -> bool {
        const std::string pattern = "\"" + key + "\"";
        size_t key_pos = json.find(pattern);
        if (key_pos == std::string::npos) {
            return false;
        }
        size_t colon = json.find(':', key_pos + pattern.size());
        if (colon == std::string::npos) {
            return false;
        }
        size_t array_start = json.find('[', colon + 1);
        if (array_start == std::string::npos) {
            return false;
        }
        size_t array_end = json.find(']', array_start + 1);
        if (array_end == std::string::npos) {
            return false;
        }
        size_t quote = json.find('"', array_start + 1);
        if (quote == std::string::npos || quote > array_end) {
            return false;
        }
        return extract_json_string(json, quote, value, nullptr);
    };

    auto extract_product_ids = [&](const std::string& json, size_t max_ids) -> std::vector<std::string> {
        std::vector<std::string> ids;
        size_t pos = 0;
        while (ids.size() < max_ids) {
            pos = json.find("\"id\"", pos);
            if (pos == std::string::npos) {
                break;
            }
            size_t colon = json.find(':', pos + 4);
            if (colon == std::string::npos) {
                break;
            }
            size_t quote = json.find('"', colon + 1);
            if (quote == std::string::npos) {
                break;
            }

            std::string id;
            size_t end_quote = quote;
            if (!extract_json_string(json, quote, id, &end_quote)) {
                break;
            }
            pos = end_quote + 1;

            // Product IDs are UUID-like and contain dashes.
            if (id.size() < 20 || id.find('-') == std::string::npos) {
                continue;
            }
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) {
                ids.push_back(id);
            }
        }
        return ids;
    };

    auto normalize_zone_code = [&](const std::string& zone_text) -> std::string {
        std::string zone = trim(zone_text);
        std::transform(zone.begin(), zone.end(), zone.begin(), ::toupper);
        return zone;
    };

    auto extract_zone_section = [&](const std::string& product_text, const std::string& zone_code) -> std::string {
        if (product_text.empty() || zone_code.empty()) {
            return "";
        }

        size_t marker = product_text.find(zone_code + "-");
        if (marker == std::string::npos) {
            marker = product_text.find(zone_code);
            if (marker == std::string::npos) {
                return "";
            }
        }

        size_t section_start = product_text.rfind('\n', marker);
        section_start = (section_start == std::string::npos) ? 0 : section_start + 1;

        size_t section_end = product_text.find("\n$$", marker);
        if (section_end == std::string::npos) {
            section_end = std::min(product_text.size(), section_start + static_cast<size_t>(1800));
        }

        std::string section = trim(product_text.substr(section_start, section_end - section_start));
        if (section.size() > 1500) {
            section.resize(1500);
            section += "...";
        }
        return section;
    };

    auto marine_fallback_from_products = [&](const std::string& zone_code) -> std::string {
        if (zone_code.size() < 3) {
            return "";
        }

        const std::string wx_host = "api.weather.gov";
        std::string zone_meta = httpGet(wx_host, "/zones/forecast/" + url_encode(zone_code), true);
        std::string cwa;
        if (zone_meta.empty() || !find_first_json_array_string(zone_meta, "cwa", cwa) || cwa.empty()) {
            return "";
        }

        std::vector<std::string> product_types;
        const bool great_lakes_zone =
            zone_code.rfind("LEZ", 0) == 0 || zone_code.rfind("LMZ", 0) == 0 ||
            zone_code.rfind("LHZ", 0) == 0 || zone_code.rfind("LOZ", 0) == 0 ||
            zone_code.rfind("LSZ", 0) == 0 || zone_code.rfind("SLZ", 0) == 0;
        if (great_lakes_zone) {
            product_types = {"NSH", "MWW", "CWF"};
        } else {
            product_types = {"CWF", "MWW", "NSH"};
        }

        for (const auto& product_type : product_types) {
            std::string product_list = httpGet(wx_host,
                                               "/products?location=" + url_encode(cwa) + "&type=" + product_type,
                                               true);
            if (product_list.empty()) {
                continue;
            }

            auto product_ids = extract_product_ids(product_list, 8);
            for (const auto& product_id : product_ids) {
                std::string product_json = httpGet(wx_host, "/products/" + url_encode(product_id), true);
                if (product_json.empty()) {
                    continue;
                }

                std::string product_text;
                if (!find_json_string_field(product_json, "productText", product_text) ||
                    product_text.empty()) {
                    continue;
                }

                std::string section = extract_zone_section(product_text, zone_code);
                if (!section.empty()) {
                    return section;
                }
            }
        }

        return "";
    };

    switch (type) {
        case 0x01: {
            // Forecast: grid square, numeric lat/lon, or geocoded location text.
            double lat, lon;
            std::string coords;

            if (gridSquareToLatLon(query, lat, lon)) {
                coords = format_coords(lat, lon);
            } else if (parse_lat_lon(query, lat, lon)) {
                coords = format_coords(lat, lon);
            } else if (geocode_location(query, lat, lon)) {
                coords = format_coords(lat, lon);
            } else {
                result = "Could not resolve location: " + query;
                break;
            }

            // First get the forecast URL from the points API
            host = "api.weather.gov";
            path = "/points/" + coords;
            std::string points_response = httpGet(host, path, true);

            std::string forecast_url;
            if (find_json_string_field(points_response, "forecast", forecast_url) &&
                !forecast_url.empty()) {
                size_t scheme_pos = forecast_url.find("://");
                size_t path_start = (scheme_pos == std::string::npos)
                    ? forecast_url.find('/')
                    : forecast_url.find('/', scheme_pos + 3);
                if (path_start != std::string::npos) {
                    path = forecast_url.substr(path_start);
                } else {
                    path = forecast_url;
                }

                std::string raw = httpGet(host, path, true);
                result = parseWeatherResponse(type, raw);
            }

            if (result.empty()) {
                result = "Could not get forecast for " + query;
            }
            break;
        }

        case 0x02: {
            // METAR
            host = "aviationweather.gov";
            path = "/api/data/metar?ids=" + url_encode(query) + "&format=raw";
            std::string raw = httpGet(host, path, true);
            result = parseWeatherResponse(type, raw);
            break;
        }

        case 0x03: {
            // TAF
            host = "aviationweather.gov";
            path = "/api/data/taf?ids=" + url_encode(query) + "&format=raw";
            std::string raw = httpGet(host, path, true);
            result = parseWeatherResponse(type, raw);
            break;
        }

        case 0x04: {
            // Marine
            std::string zone_code = normalize_zone_code(query);
            if (zone_code.empty()) {
                result = "Marine zone code is required";
                break;
            }

            host = "api.weather.gov";
            path = "/zones/forecast/" + url_encode(zone_code) + "/forecast";
            std::string raw = httpGet(host, path, true);
            result = parseWeatherResponse(type, raw);

            const bool marine_endpoint_unsupported =
                raw.empty() ||
                raw.find("Marine Forecast Not Supported") != std::string::npos ||
                raw.find("Zone Does Not Exist") != std::string::npos ||
                result.find("not yet supported by this API") != std::string::npos ||
                result.find("Marine Forecast Not Supported") != std::string::npos ||
                result == "Could not parse forecast data" ||
                result == "Could not extract forecast text";

            if (marine_endpoint_unsupported) {
                std::string fallback = marine_fallback_from_products(zone_code);
                if (!fallback.empty()) {
                    result = fallback;
                } else if (result.empty() ||
                           result == "Could not parse forecast data" ||
                           result == "Could not extract forecast text" ||
                           result.find("not yet supported by this API") != std::string::npos ||
                           result.find("Marine Forecast Not Supported") != std::string::npos) {
                    result = "Marine forecast unavailable for zone " + zone_code;
                }
            }
            break;
        }

        default:
            result = "Unknown weather type";
            break;
    }

    return result;
}

void App::fetchWeatherAsync(uint8_t type, const std::string& query, const std::string& requester) {
    // Remote queries end up in URLs and HTTP request lines.
    if (query.empty() || query.size() > 64 || app_detail::containsControlChars(query)) {
        appendSystemChat("Weather request from " + requester + " rejected: invalid query");
        return;
    }

    auto worker = weather_worker_;
    bool start_thread = false;
    {
        std::lock_guard<std::mutex> lock(worker->mutex);
        // One request in flight (or queued) per requester, bounded overall.
        const bool requester_busy =
            worker->active_requester == requester ||
            std::any_of(worker->queue.begin(), worker->queue.end(),
                        [&](const WeatherJob& job) { return job.requester == requester; });
        if (requester_busy || worker->queue.size() >= WeatherWorker::kMaxQueued) {
            worker->rejected.push_back(requester);
        } else {
            WeatherJob job;
            job.type = type;
            job.query = query;
            job.requester = requester;
            worker->queue.push_back(std::move(job));
            if (!worker->running) {
                worker->running = true;
                start_thread = true;
            }
        }
    }

    if (!start_thread) {
        return;
    }

    // The thread owns a reference to the shared worker state, never to the
    // App, so shutting down while a fetch is in flight is safe.
    std::thread([worker]() {
        for (;;) {
            WeatherJob job;
            {
                std::lock_guard<std::mutex> lock(worker->mutex);
                if (worker->queue.empty()) {
                    worker->running = false;
                    worker->active_requester.clear();
                    return;
                }
                job = std::move(worker->queue.front());
                worker->queue.pop_front();
                worker->active_requester = job.requester;
            }
            job.result = fetchWeatherReport(job.type, job.query);
            {
                std::lock_guard<std::mutex> lock(worker->mutex);
                worker->active_requester.clear();
                worker->done.push_back(std::move(job));
            }
        }
    }).detach();
}

// Deliver completed weather fetches to the station that asked for them.
void App::processWeatherFetchResult() {
    std::vector<WeatherJob> done;
    std::vector<std::string> rejected;
    {
        std::lock_guard<std::mutex> lock(weather_worker_->mutex);
        done.swap(weather_worker_->done);
        rejected.swap(weather_worker_->rejected);
    }

    for (const auto& requester : rejected) {
        appendSystemChat("Weather request from " + requester + " dropped: a request is already pending");
    }

    for (const auto& job : done) {
        // The response goes over the link; only send it if the requester is
        // still the station on the other end.
        if (linkedPeerCallsign() != job.requester) {
            appendSystemChat("Weather result for " + job.requester + " (" + job.query +
                             ") discarded: station no longer linked");
            continue;
        }
        if (!job.result.empty()) {
            sendWeatherResponse(job.type, job.query, job.result);
        } else {
            sendWeatherResponse(job.type, job.query, "Weather data not available for " + job.query);
        }
    }
}

// ============================================================================
// File Transfer Helpers
// ============================================================================

void App::syncTransfersFromManager() {
    if (!file_transfer_mgr_) {
        return;
    }

    std::vector<FileTransfer> ui_transfers;
    auto active = file_transfer_mgr_->getActiveTransfers();
    auto completed = file_transfer_mgr_->getCompletedTransfers();
    ui_transfers.reserve(active.size() + completed.size());

    auto append_transfers = [&ui_transfers](const std::vector<TransferProgress>& list) {
        for (const auto& t : list) {
            FileTransfer ui{};
            ui.filename = t.filename;
            ui.remote_callsign = t.remote_callsign;
            ui.total_bytes = t.total_bytes;
            ui.transferred_bytes = t.transferred_bytes;
            ui.is_upload = (t.direction == TransferDirection::Upload);
            ui.completed = (t.state == TransferState::Completed);
            ui.failed = (t.state == TransferState::Failed || t.state == TransferState::Cancelled);
            ui.error = t.error_message;
            ui.started_at = t.started_at;
            ui_transfers.push_back(std::move(ui));
        }
    };

    append_transfers(active);
    append_transfers(completed);
    transfers_ = std::move(ui_transfers);
}

bool App::isImageFile(const std::string& filename) const {
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    return lower.ends_with(".jpg") || lower.ends_with(".jpeg") ||
           lower.ends_with(".png") || lower.ends_with(".gif") ||
           lower.ends_with(".bmp") || lower.ends_with(".webp");
}

std::string App::getFileTypeDescription(const std::string& filename) const {
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower.ends_with(".jpg") || lower.ends_with(".jpeg")) return "JPEG Image";
    if (lower.ends_with(".png")) return "PNG Image";
    if (lower.ends_with(".gif")) return "GIF Image";
    if (lower.ends_with(".bmp")) return "Bitmap Image";
    if (lower.ends_with(".webp")) return "WebP Image";
    if (lower.ends_with(".pdf")) return "PDF Document";
    if (lower.ends_with(".txt")) return "Text File";
    if (lower.ends_with(".log")) return "Log File";
    if (lower.ends_with(".adi") || lower.ends_with(".adif")) return "ADIF Log";
    if (lower.ends_with(".cabrillo") || lower.ends_with(".cbr")) return "Cabrillo Log";
    if (lower.ends_with(".qsl")) return "QSL Card";

    // Extract extension
    size_t dot_pos = lower.find_last_of('.');
    if (dot_pos != std::string::npos) {
        return lower.substr(dot_pos + 1) + " file";
    }
    return "Unknown";
}

// ============================================================================
// PSK Reporter Functions
// ============================================================================

void App::queuePSKReporterSpot(const std::string& callsign, const std::string& grid,
                                float snr, uint32_t freq_hz) {
    if (!config_.psk_reporter.enabled) return;
    if (callsign.empty()) return;

    std::lock_guard<std::mutex> lock(psk_reporter_mutex_);

    PSKReporterSpot spot;
    spot.callsign = callsign;
    spot.grid_square = grid;
    spot.snr_db = snr;
    spot.frequency_hz = (freq_hz > 0) ? freq_hz : current_frequency_hz_;
    spot.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Avoid duplicates within short time window (30 seconds)
    auto it = std::find_if(psk_reporter_queue_.begin(), psk_reporter_queue_.end(),
        [&](const PSKReporterSpot& s) {
            return s.callsign == callsign &&
                   (spot.timestamp - s.timestamp) < 30;
        });

    if (it == psk_reporter_queue_.end()) {
        psk_reporter_queue_.push_back(spot);
    }
}

void App::updatePSKReporter() {
    if (!config_.psk_reporter.enabled) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - last_psk_reporter_upload_);

    if (elapsed.count() >= config_.psk_reporter.upload_interval_sec) {
        uploadPSKReporterSpots();
        last_psk_reporter_upload_ = now;
    }
}

void App::uploadPSKReporterSpots() {
    std::vector<PSKReporterSpot> spots_to_upload;

    {
        std::lock_guard<std::mutex> lock(psk_reporter_mutex_);
        if (psk_reporter_queue_.empty()) return;
        spots_to_upload = std::move(psk_reporter_queue_);
        psk_reporter_queue_.clear();
    }

    if (config_.station.callsign.empty()) {
        std::cerr << "[PSKReporter] Cannot upload: callsign not configured\n";
        return;
    }

    std::string payload = generatePSKReporterPayload(
        config_.station.callsign,
        config_.station.grid_square,
        config_.psk_reporter.antenna,
        spots_to_upload
    );

    if (!sendPSKReporterUDP(payload)) {
        std::cerr << "[PSKReporter] Upload failed, re-queueing spots\n";
        std::lock_guard<std::mutex> lock(psk_reporter_mutex_);
        for (const auto& spot : spots_to_upload) {
            psk_reporter_queue_.push_back(spot);
        }
    }
}

std::string App::generatePSKReporterPayload(const std::string& reporter_call,
                                             const std::string& reporter_grid,
                                             const std::string& antenna,
                                             const std::vector<PSKReporterSpot>& spots) {
    // PSK Reporter uses a binary format over UDP
    // Format documentation: https://pskreporter.info/pskdev.html
    // Header: 0x00 0x0A (version), then TLV records

    std::string payload;

    // Header (version 0x000A)
    payload.push_back(0x00);
    payload.push_back(0x0A);

    // Sequence number (4 bytes, little endian)
    static uint32_t sequence = 0;
    sequence++;
    payload.push_back(sequence & 0xFF);
    payload.push_back((sequence >> 8) & 0xFF);
    payload.push_back((sequence >> 16) & 0xFF);
    payload.push_back((sequence >> 24) & 0xFF);

    // Random ID (4 bytes) - use a fixed ID per session
    static uint32_t session_id = static_cast<uint32_t>(
        std::chrono::system_clock::now().time_since_epoch().count() & 0xFFFFFFFF);
    payload.push_back(session_id & 0xFF);
    payload.push_back((session_id >> 8) & 0xFF);
    payload.push_back((session_id >> 16) & 0xFF);
    payload.push_back((session_id >> 24) & 0xFF);

    // Receiver info record (type 0x99 0x92)
    std::string rx_record;
    rx_record.push_back(0x99);
    rx_record.push_back(0x92);

    // Receiver callsign (length-prefixed string)
    rx_record.push_back(static_cast<char>(reporter_call.length()));
    rx_record += reporter_call;

    // Receiver locator (length-prefixed string)
    rx_record.push_back(static_cast<char>(reporter_grid.length()));
    rx_record += reporter_grid;

    // Software name and version
    std::string sw_name = "Skywave Link";
    std::string sw_version = "1.0";
    rx_record.push_back(static_cast<char>(sw_name.length()));
    rx_record += sw_name;
    rx_record.push_back(static_cast<char>(sw_version.length()));
    rx_record += sw_version;

    // Antenna (length-prefixed)
    rx_record.push_back(static_cast<char>(antenna.length()));
    rx_record += antenna;

    // Record length (2 bytes, big endian) at position 2-3
    uint16_t rx_len = static_cast<uint16_t>(rx_record.length());
    std::string rx_header;
    rx_header.push_back((rx_len >> 8) & 0xFF);
    rx_header.push_back(rx_len & 0xFF);
    payload += rx_header + rx_record;

    // Sender info records (type 0x99 0x93)
    for (const auto& spot : spots) {
        std::string tx_record;
        tx_record.push_back(0x99);
        tx_record.push_back(0x93);

        // Sender callsign
        tx_record.push_back(static_cast<char>(spot.callsign.length()));
        tx_record += spot.callsign;

        // Frequency (4 bytes, big endian)
        uint32_t freq = spot.frequency_hz;
        tx_record.push_back((freq >> 24) & 0xFF);
        tx_record.push_back((freq >> 16) & 0xFF);
        tx_record.push_back((freq >> 8) & 0xFF);
        tx_record.push_back(freq & 0xFF);

        // SNR (1 byte, signed)
        int8_t snr_byte = static_cast<int8_t>(std::max(-128.0f, std::min(127.0f, spot.snr_db)));
        tx_record.push_back(static_cast<char>(snr_byte));

        // Mode (length-prefixed string)
        std::string mode = "SKYWAVE";
        tx_record.push_back(static_cast<char>(mode.length()));
        tx_record += mode;

        // Sender locator (if known)
        tx_record.push_back(static_cast<char>(spot.grid_square.length()));
        if (!spot.grid_square.empty()) {
            tx_record += spot.grid_square;
        }

        // Timestamp (4 bytes, Unix time, big endian)
        uint32_t ts = static_cast<uint32_t>(spot.timestamp);
        tx_record.push_back((ts >> 24) & 0xFF);
        tx_record.push_back((ts >> 16) & 0xFF);
        tx_record.push_back((ts >> 8) & 0xFF);
        tx_record.push_back(ts & 0xFF);

        // Record length header
        uint16_t tx_len = static_cast<uint16_t>(tx_record.length());
        std::string tx_header;
        tx_header.push_back((tx_len >> 8) & 0xFF);
        tx_header.push_back(tx_len & 0xFF);
        payload += tx_header + tx_record;
    }

    return payload;
}

bool App::sendPSKReporterUDP(const std::string& payload) {
    // PSK Reporter server: report.pskreporter.info port 4739
    const char* host = "report.pskreporter.info";
    const int port = 4739;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[PSKReporter] WSAStartup failed\n";
        return false;
    }
#endif

    // Create UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        std::cerr << "[PSKReporter] Failed to create socket\n";
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }

    // Resolve hostname
    struct hostent* he = gethostbyname(host);
    if (!he) {
        std::cerr << "[PSKReporter] Failed to resolve " << host << "\n";
#ifdef _WIN32
        closesocket(sock);
        WSACleanup();
#else
        close(sock);
#endif
        return false;
    }

    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    std::memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);

    // Send payload
    ssize_t sent = sendto(sock, payload.data(), payload.size(), 0,
                          reinterpret_cast<struct sockaddr*>(&server_addr),
                          sizeof(server_addr));

#ifdef _WIN32
    closesocket(sock);
    WSACleanup();
#else
    close(sock);
#endif

    if (sent != static_cast<ssize_t>(payload.size())) {
        std::cerr << "[PSKReporter] Failed to send UDP packet\n";
        return false;
    }

    return true;
}

} // namespace skylink


// ---------------------------------------------------------------------------
// Scripted test hook (docs/TESTING_HOOK.md)
// ---------------------------------------------------------------------------

namespace {
std::string trimCopy(const std::string& v) {
    size_t a = v.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = v.find_last_not_of(" \t\r\n");
    return v.substr(a, b - a + 1);
}
std::vector<std::string> splitBar(const std::string& v) {
    std::vector<std::string> out;
    size_t p = 0;
    while (true) {
        size_t q = v.find('|', p);
        out.push_back(trimCopy(v.substr(p, q == std::string::npos ? std::string::npos : q - p)));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return out;
}
std::string jsonEsc(const std::string& v) {
    // Valid UTF-8 passes through; any other byte becomes U+FFFD so the
    // report always parses.
    std::string o;
    size_t i = 0;
    while (i < v.size()) {
        const unsigned char c = static_cast<unsigned char>(v[i]);
        if (c < 0x80) {
            if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
            else if (c == '\n') o += "\\n";
            else if (c < 0x20) o += ' ';
            else o += static_cast<char>(c);
            ++i;
            continue;
        }
        const size_t len = (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        bool ok = len != 0 && i + len <= v.size();
        for (size_t k = 1; ok && k < len; ++k) {
            ok = (static_cast<unsigned char>(v[i + k]) & 0xC0) == 0x80;
        }
        if (ok) {
            o.append(v, i, len);
            i += len;
        } else {
            o += "\xEF\xBF\xBD";
            ++i;
        }
    }
    return o;
}
}  // namespace

void skylink::App::testLog(const std::string& line) {
    std::cout << "TEST: " << line << std::endl;
    if (test_log_.is_open()) {
        test_log_ << line << std::endl;
    }
}

void skylink::App::loadTestScript() {
    const char* path = std::getenv("SKYLINK_TEST_SCRIPT");
    if (!path || !*path) return;
    std::ifstream in(path);
    if (!in) {
        std::cerr << "TEST: cannot open script " << path << "\n";
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        line = trimCopy(line);
        if (line.empty() || line[0] == '#') continue;
        test_script_.push_back(line);
    }
    if (const char* log = std::getenv("SKYLINK_TEST_LOG")) test_log_.open(log);
    testLog("script loaded: " + std::to_string(test_script_.size()) + " actions");
}

void skylink::App::runTestScript() {
    if (test_script_.empty() || test_pc_ >= test_script_.size()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < test_wait_until_) return;
    if (!test_wait_cond_.empty()) {
        bool met = false;
        if (test_wait_cond_ == "modem") met = modem_ && modem_->isConnected();
        else if (test_wait_cond_ == "connected") met = isStationLinkConnected(false);
        else if (test_wait_cond_ == "disconnected") met = !isStationLinkConnected(false);
        else if (test_wait_cond_ == "file_offer")
            met = file_transfer_mgr_ && !file_transfer_mgr_->getPendingOffers().empty();
        if (!met && now < test_cond_deadline_) return;
        testLog(std::string("wait ") + test_wait_cond_ + (met ? " -> ok" : " -> TIMEOUT"));
        test_wait_cond_.clear();
    }
    const std::string line = test_script_[test_pc_++];
    const size_t sp = line.find(' ');
    const std::string cmd = line.substr(0, sp);
    const std::string arg = sp == std::string::npos ? "" : trimCopy(line.substr(sp + 1));
    testLog("> " + line);
    if (cmd == "wait") {
        test_wait_until_ = now + std::chrono::milliseconds(int64_t(std::atof(arg.c_str()) * 1000.0));
    } else if (cmd == "wait_modem" || cmd == "wait_connected" || cmd == "wait_disconnected" ||
               cmd == "wait_file_offer") {
        test_wait_cond_ = cmd.substr(5);
        test_cond_deadline_ = now + std::chrono::seconds(arg.empty() ? 60 : std::atoi(arg.c_str()));
    } else if (cmd == "connect_modem") {
        connectToModem();
    } else if (cmd == "disconnect_modem") {
        disconnectFromModem();
    } else if (cmd == "connect") {
        connectToStation(arg);
    } else if (cmd == "disconnect") {
        if (modem_) modem_->sendCommand("DISCONNECT");
    } else if (cmd == "chat") {
        sendChatMessage(arg);
    } else if (cmd == "file") {
        sendFile(arg);
    } else if (cmd == "file_accept" || cmd == "file_decline") {
        if (!decideNewestPendingFileOffer(cmd == "file_accept")) testLog("no pending file offer");
    } else if (cmd == "ping") {
        sendPing(arg);
    } else if (cmd == "cq_start") {
        startCQ();
    } else if (cmd == "cq_stop") {
        stopCQ();
    } else if (cmd == "broadcast") {
        sendBroadcast(arg);
    } else if (cmd == "beacon") {
        sendBeacon();
    } else if (cmd == "mail") {
        auto f = splitBar(arg);
        if (f.size() >= 3) sendMail(f[0], f[1], f[2]);
        else testLog("mail needs TO|SUBJECT|BODY");
    } else if (cmd == "broadcast_mail") {
        auto f = splitBar(arg);
        if (f.size() >= 2) sendBroadcastMail(f[0], f[1]);
    } else if (cmd == "weather") {
        requestWeatherForecast(arg);
    } else if (cmd == "metar") {
        requestMETAR(arg);
    } else if (cmd == "bbs_upload") {
        auto f = splitBar(arg);
        uploadToBBS(f[0], f.size() > 1 ? f[1] : "");
    } else if (cmd == "bbs_announce") {
        for (const auto& b : bbs_files_) {
            if (!b.remote) announceBBSFile(b);
        }
    } else if (cmd == "bbs_fetch") {
        auto f = splitBar(arg);
        if (f.size() >= 2) requestRemoteBBSDownload(f[0], f[1]);
    } else if (cmd == "snr") {
        exchangeSNR(true);
    } else if (cmd == "position") {
        auto f = splitBar(arg);
        if (f.size() >= 2) updatePosition(std::atof(f[0].c_str()), std::atof(f[1].c_str()));
    } else if (cmd == "modem") {
        if (modem_) modem_->sendCommand(arg);
    } else if (cmd == "screenshot") {
        test_shot_path_ = arg;
    } else if (cmd == "show") {
        static const std::pair<const char*, AppTab> kTabs[] = {
            {"chat", AppTab::Chat}, {"files", AppTab::Files}, {"bbs", AppTab::BBS},
            {"frequency", AppTab::Frequency}, {"messages", AppTab::Messages},
            {"mail", AppTab::Mail}, {"weather", AppTab::Weather},
            {"position", AppTab::Position}, {"map", AppTab::StationMap},
        };
        if (arg == "settings") {
            show_settings_ = true;
        } else {
            bool found = false;
            for (const auto& [name, tab] : kTabs) {
                if (arg == name) {
                    current_tab_ = tab;
                    found = true;
                }
            }
            if (!found) testLog("unknown window: " + arg);
        }
    } else if (cmd == "theme") {
        config_.ui.dark_mode = (arg != "light");
        setupStyle();
    } else if (cmd == "dump") {
        dumpTestReport(arg);
    } else if (cmd == "quit") {
        running_ = false;
    } else {
        testLog("unknown action: " + cmd);
    }
}

void skylink::App::dumpTestReport(const std::string& path) {
    std::ofstream o(path);
    o << "{\n  \"callsign\": \"" << jsonEsc(config_.station.callsign) << "\",\n";
    o << "  \"modem_connected\": " << (modem_ && modem_->isConnected() ? "true" : "false") << ",\n";
    o << "  \"link_connected\": " << (isStationLinkConnected(false) ? "true" : "false") << ",\n";
    o << "  \"chat\": [";
    for (size_t i = 0; i < chat_history_.size(); ++i) {
        const auto& m = chat_history_[i];
        o << (i ? "," : "") << "\n    {\"from\": \"" << jsonEsc(m.from) << "\", \"to\": \"" << jsonEsc(m.to)
          << "\", \"out\": " << (m.is_outgoing ? "true" : "false") << ", \"bcast\": "
          << (m.is_broadcast ? "true" : "false") << ", \"text\": \"" << jsonEsc(m.text) << "\"}";
    }
    o << "\n  ],\n  \"transfers\": [";
    for (size_t i = 0; i < transfers_.size(); ++i) {
        const auto& t = transfers_[i];
        o << (i ? "," : "") << "\n    {\"file\": \"" << jsonEsc(t.filename) << "\", \"peer\": \""
          << jsonEsc(t.remote_callsign) << "\", \"upload\": " << (t.is_upload ? "true" : "false")
          << ", \"bytes\": " << t.transferred_bytes << ", \"total\": " << t.total_bytes
          << ", \"completed\": " << (t.completed ? "true" : "false") << ", \"failed\": "
          << (t.failed ? "true" : "false") << ", \"error\": \"" << jsonEsc(t.error) << "\"}";
    }
    auto mails = [&](const char* name, const std::vector<MailMessage>& v) {
        o << "\n  ],\n  \"" << name << "\": [";
        for (size_t i = 0; i < v.size(); ++i) {
            const auto& m = v[i];
            o << (i ? "," : "") << "\n    {\"from\": \"" << jsonEsc(m.from) << "\", \"to\": \""
              << jsonEsc(m.to) << "\", \"subject\": \"" << jsonEsc(m.subject) << "\", \"body\": \""
              << jsonEsc(m.body) << "\", \"status\": " << int(m.status) << ", \"bcast\": "
              << (m.is_broadcast ? "true" : "false") << "}";
        }
    };
    mails("mail_inbox", mail_inbox_);
    mails("mail_outbox", mail_outbox_);
    o << "\n  ],\n  \"heard_stations\": [";
    for (size_t i = 0; i < heard_stations_.size(); ++i) {
        o << (i ? ", " : "") << "\"" << jsonEsc(heard_stations_[i].first) << "\"";
    }
    o << "],\n  \"heard_beacons\": [";
    for (size_t i = 0; i < heard_beacons_.size(); ++i) {
        o << (i ? ", " : "") << "{\"call\": \"" << jsonEsc(heard_beacons_[i].callsign)
          << "\", \"grid\": \"" << jsonEsc(heard_beacons_[i].grid_square) << "\"}";
    }
    o << "],\n  \"ping_results\": [";
    for (size_t i = 0; i < ping_results_.size(); ++i) {
        o << (i ? ", " : "") << "{\"call\": \"" << jsonEsc(ping_results_[i].callsign)
          << "\", \"online\": " << (ping_results_[i].online ? "true" : "false") << "}";
    }
    o << "],\n  \"weather\": [";
    for (size_t i = 0; i < weather_reports_.size(); ++i) {
        const auto& w = weather_reports_[i];
        o << (i ? ", " : "") << "{\"type\": \"" << jsonEsc(w.type) << "\", \"loc\": \""
          << jsonEsc(w.location) << "\", \"from\": \"" << jsonEsc(w.source_station)
          << "\", \"pending\": " << (w.is_request_pending ? "true" : "false") << ", \"len\": "
          << w.report.size() << "}";
    }
    o << "],\n  \"bbs_files\": [";
    for (size_t i = 0; i < bbs_files_.size(); ++i) {
        const auto& b = bbs_files_[i];
        o << (i ? ", " : "") << "{\"file\": \"" << jsonEsc(b.filename) << "\", \"remote\": "
          << (b.remote ? "true" : "false") << ", \"uploader\": \"" << jsonEsc(b.uploader) << "\"}";
    }
    o << "]\n}\n";
    testLog("report written: " + path);
}
