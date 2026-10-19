// Skywave Link Application - Main application class

#pragma once

#include <fstream>

#include "notification_sound.hpp"
#include "config.hpp"
#include "modem_client.hpp"
#include "message_store.hpp"
#include "freq_manager.hpp"
#include "radio_controller.hpp"
#include "aprs_client.hpp"
#include "file_transfer.hpp"
#include "mesh_relay.hpp"
#include "shared_state.hpp"
#include "window_base.hpp"
#include "email_gateway_job.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <optional>
#include <unordered_set>
#include <type_traits>
#include <utility>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <map>
#include <deque>
#include <mutex>
#include <thread>
#include <future>
#include <unordered_map>

// Forward declarations for ImGui
struct SDL_Window;
typedef struct SDL_KeyboardEvent SDL_KeyboardEvent;
typedef void* SDL_GLContext;

namespace skylink {

// Pure receive-path policy helpers used by App (header-only so they can be
// unit tested without the GUI).
namespace app_detail {

// Payload types a station may send while unlinked (BCAST_DATA path). The
// sender is the broadcast's source as reported by the modem.
inline bool isDisconnectedBroadcastPayload(uint8_t msg_type) {
    switch (msg_type) {
        case 0xFB:  // Text broadcast
        case 0xFC:  // CQ
        case 0xFA:  // Advanced beacon
        case 0xF4:  // Position beacon
        case 0xF9:  // Pending-message notification
        case 0xE3:  // Legacy broadcast mail
        case 0xE0:  // Ping request (addressed broadcast)
        case 0xE1:  // Ping response (addressed broadcast)
        case 0xE5:  // Mesh relay packet
        case 0xE7:  // Reliable broadcast mail
        case 0xD0:  // BBS announce
        case 0xD1:  // BBS remove
            return true;
        default:
            return false;
    }
}

// Payload types that act on the current session and are honoured only
// when they arrive over the link from the connected peer.
inline bool isSessionOnlyPayload(uint8_t msg_type) {
    if (msg_type >= 0x10 && msg_type <= 0x18) {  // File transfer
        return true;
    }
    switch (msg_type) {
        case 0xD2:  // BBS file request
        case 0xF1:  // QSY confirm
        case 0xF2:  // QSY suggestion
        case 0xF5:  // Position relay request (relayed to APRS-IS)
        case 0xF6:  // Away-mode request
        case 0xE8:  // Mail ACK/NACK
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Application payload type bytes. Every payload this app sends starts with
// one of these; anything else is dropped on receive.
//
// Chat text has its own type byte, 0xC0 (never a valid UTF-8 byte), so text
// that happens to start with a UTF-8 lead byte (0xE0-0xF4: "€", CJK, emoji)
// can no longer be mistaken for mail, ping, weather or a control frame.
//   0xC0 | UTF-8 text                         one-frame chat line
//   0xFD | frag_idx | total_frags | bytes     chat line in fragments
// ---------------------------------------------------------------------------
inline constexpr uint8_t kChatTextMarker = 0xC0;
inline constexpr uint8_t kChatFragmentMarker = 0xFD;
inline constexpr size_t kMaxChatFragments = 255;

// Inner payloads a mesh relay packet addressed to us may deliver: chat
// text and mail only. Never another relay packet or a control type.
inline bool isMeshDeliverablePayload(uint8_t msg_type) {
    switch (msg_type) {
        case 0xE2:  // Legacy private mail
        case 0xE3:  // Legacy broadcast mail
        case 0xE6:  // Reliable private mail
        case 0xE7:  // Reliable broadcast mail
        case 0xFB:  // Text broadcast
        case kChatTextMarker:
            return true;
        default:
            return false;
    }
}

// Smallest well-formed payload for each type this app understands, or 0
// for a type it does not know. A payload shorter than its minimum, or of an
// unknown type, is malformed and is dropped (never shown as chat text).
inline size_t minimumPayloadSize(uint8_t msg_type) {
    if (msg_type >= 0x10 && msg_type <= 0x18) {
        return 1;  // File transfer: FileTransferManager validates the rest
    }
    switch (msg_type) {
        case kChatTextMarker: return 2;      // marker + at least one byte
        case kChatFragmentMarker: return 4;  // marker, index, count, data
        case 0xD0: return 8;   // BBS announce
        case 0xD1: return 3;   // BBS remove
        case 0xD2: return 3;   // BBS file request
        case 0xE0: return 3;   // Ping request
        case 0xE1: return 3;   // Ping response
        case 0xE2: return 4;   // Legacy private mail
        case 0xE3: return 2;   // Legacy broadcast mail
        case 0xE4: return 2;   // Weather request/response
        case 0xE5: return 2;   // Mesh relay packet (header checked by MeshRelay)
        case 0xE6: return 6;   // Reliable private mail
        case 0xE7: return 5;   // Reliable broadcast mail
        case 0xE8: return 4;   // Mail ACK/NACK
        case 0xF1: return 9;   // QSY confirm
        case 0xF2: return 8;   // QSY suggestion
        case 0xF3: return 2;   // Position relay ACK
        case 0xF4: return 18;  // Position beacon
        case 0xF5: return 18;  // Position relay request
        case 0xF6: return 2;   // Away-mode request
        case 0xF7: return 2;   // Away-mode response
        case 0xF9: return 2;   // Pending-message notification
        case 0xFA: return 3;   // Station beacon: marker, grid_len, flags
        case 0xFB: return 2;   // Text broadcast
        case 0xFC: return 1;   // CQ
        default: return 0;     // Unknown (incl. the removed 0xF8 SNR and 0xFE typing frames)
    }
}

inline bool isWellFormedPayload(const std::vector<uint8_t>& data) {
    if (data.empty()) {
        return false;
    }
    const size_t min = minimumPayloadSize(data[0]);
    return min != 0 && data.size() >= min;
}

// Number of bytes at the front of `text` (at most `max_bytes`) that end on
// a UTF-8 character boundary, so a fragment never splits a character.
inline size_t utf8PrefixLength(const std::string& text, size_t start, size_t max_bytes) {
    size_t len = std::min(max_bytes, text.size() - start);
    if (start + len >= text.size()) {
        return len;
    }
    size_t cut = len;
    // Step back over continuation bytes (10xxxxxx) to a lead byte.
    while (cut > 0 && (static_cast<unsigned char>(text[start + cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return cut > 0 ? cut : len;  // a single over-long "character": cut anyway
}

// Wire packets for one chat line: one 0xC0 packet, or 0xFD fragments of at
// most `max_fragment_bytes` text bytes each. Empty when the text is empty
// or needs more than kMaxChatFragments fragments.
inline std::vector<std::vector<uint8_t>> buildChatPackets(const std::string& text,
                                                          size_t max_fragment_bytes) {
    std::vector<std::vector<uint8_t>> packets;
    if (text.empty() || max_fragment_bytes == 0) {
        return packets;
    }
    if (text.size() <= max_fragment_bytes) {
        std::vector<uint8_t> packet;
        packet.reserve(text.size() + 1);
        packet.push_back(kChatTextMarker);
        packet.insert(packet.end(), text.begin(), text.end());
        packets.push_back(std::move(packet));
        return packets;
    }
    std::vector<std::string> chunks;
    for (size_t start = 0; start < text.size();) {
        const size_t len = utf8PrefixLength(text, start, max_fragment_bytes);
        chunks.push_back(text.substr(start, len));
        start += len;
        if (chunks.size() > kMaxChatFragments) {
            return {};
        }
    }
    for (size_t i = 0; i < chunks.size(); ++i) {
        std::vector<uint8_t> packet;
        packet.reserve(chunks[i].size() + 3);
        packet.push_back(kChatFragmentMarker);
        packet.push_back(static_cast<uint8_t>(i));
        packet.push_back(static_cast<uint8_t>(chunks.size()));
        packet.insert(packet.end(), chunks[i].begin(), chunks[i].end());
        packets.push_back(std::move(packet));
    }
    return packets;
}

// Text of a one-frame chat packet, or false when it is not one.
inline bool decodeChatPacket(const std::vector<uint8_t>& data, std::string* text) {
    if (data.size() < 2 || data[0] != kChatTextMarker) {
        return false;
    }
    if (text) {
        text->assign(data.begin() + 1, data.end());
    }
    return true;
}

inline std::string upperTrimmedCallsign(const std::string& call) {
    const size_t a = call.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) {
        return "";
    }
    const size_t b = call.find_last_not_of(" \t\r\n");
    std::string out = call.substr(a, b - a + 1);
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

// Private mail to a station goes straight over the link only when that
// station is the linked peer; otherwise the peer would refuse it ("Not
// recipient") and the mail would be resent until it failed.
inline bool privateMailGoesDirect(const std::string& linked_peer, const std::string& recipient) {
    const std::string peer = upperTrimmedCallsign(linked_peer);
    return !peer.empty() && peer != "UNKNOWN" && peer == upperTrimmedCallsign(recipient);
}

// Email gateway relay allowlist. Fails closed: with no list, nobody may
// send internet email through this station's account.
inline bool relaySenderAllowed(const std::vector<std::string>& allowed_relays,
                               const std::string& sender) {
    const std::string from = upperTrimmedCallsign(sender);
    if (from.empty() || from == "UNKNOWN") {
        return false;
    }
    for (const auto& allowed : allowed_relays) {
        if (upperTrimmedCallsign(allowed) == from) {
            return true;
        }
    }
    return false;
}

// Away-mode email command sent as chat text by the linked station:
//   EMAIL:<address>:<subject>:<body>
// The body may contain ':'; address and subject may not. The verb is
// case-insensitive.
struct AwayEmailCommand {
    std::string to;
    std::string subject;
    std::string body;
};

inline bool parseAwayEmailCommand(const std::string& text, AwayEmailCommand* out) {
    const size_t a = text.find_first_not_of(" \t\r\n");
    if (a == std::string::npos || text.size() - a < 6) {
        return false;
    }
    std::string verb = text.substr(a, 6);
    for (char& c : verb) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (verb != "EMAIL:") {
        return false;
    }
    const size_t to_start = a + 6;
    const size_t c1 = text.find(':', to_start);
    if (c1 == std::string::npos) {
        return false;
    }
    const size_t c2 = text.find(':', c1 + 1);
    if (c2 == std::string::npos) {
        return false;
    }
    auto trim = [](std::string s) {
        const size_t x = s.find_first_not_of(" \t\r\n");
        if (x == std::string::npos) return std::string();
        const size_t y = s.find_last_not_of(" \t\r\n");
        return s.substr(x, y - x + 1);
    };
    AwayEmailCommand cmd;
    cmd.to = trim(text.substr(to_start, c1 - to_start));
    cmd.subject = trim(text.substr(c1 + 1, c2 - c1 - 1));
    cmd.body = trim(text.substr(c2 + 1));
    const size_t at = cmd.to.find('@');
    if (cmd.to.empty() || at == std::string::npos || at == 0 || at + 1 >= cmd.to.size() ||
        cmd.to.find_first_of(" \t") != std::string::npos || cmd.body.empty()) {
        return false;
    }
    if (out) {
        *out = std::move(cmd);
    }
    return true;
}

// Station beacon payload (0xFA). The beacon carries no SNR: nothing the
// sender knows is a measurement of how it is heard.
//   0xFA | grid_len | grid | flags | "<CALLSIGN> <text>"
// flags bit 0: email gateway available.
struct StationBeacon {
    std::string grid;
    bool email_gateway = false;
    std::string text;  // callsign + beacon text
};

inline std::vector<uint8_t> encodeStationBeacon(const StationBeacon& beacon) {
    std::string grid = beacon.grid.substr(0, 8);
    std::vector<uint8_t> packet;
    packet.push_back(0xFA);
    packet.push_back(static_cast<uint8_t>(grid.size()));
    packet.insert(packet.end(), grid.begin(), grid.end());
    packet.push_back(beacon.email_gateway ? 0x01 : 0x00);
    packet.insert(packet.end(), beacon.text.begin(), beacon.text.end());
    return packet;
}

inline bool decodeStationBeacon(const std::vector<uint8_t>& data, StationBeacon* out) {
    if (data.size() < 3 || data[0] != 0xFA) {
        return false;
    }
    const size_t grid_len = data[1];
    if (grid_len > 8 || data.size() < 2 + grid_len + 1) {
        return false;
    }
    StationBeacon beacon;
    beacon.grid.assign(data.begin() + 2, data.begin() + 2 + static_cast<std::ptrdiff_t>(grid_len));
    const uint8_t flags = data[2 + grid_len];
    beacon.email_gateway = (flags & 0x01) != 0;
    beacon.text.assign(data.begin() + 3 + static_cast<std::ptrdiff_t>(grid_len), data.end());
    if (out) {
        *out = std::move(beacon);
    }
    return true;
}

// Exponential backoff for automatic gateway calls: 10 s, 20 s, 40 s ...
// capped at 10 minutes.
inline int gatewayRetryDelaySec(int attempts_made) {
    int delay = 10;
    for (int i = 1; i < attempts_made && delay < 600; ++i) {
        delay *= 2;
    }
    return std::min(delay, 600);
}

// Pull "key": "value" string fields from a JSON fragment (first match).
inline bool jsonStringField(const std::string& json, const std::string& key, std::string* value) {
    const std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    while (pos != std::string::npos) {
        size_t p = pos + pattern.size();
        while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r')) ++p;
        if (p < json.size() && json[p] == ':') {
            ++p;
            while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r')) ++p;
            if (p >= json.size() || json[p] != '"') {
                return false;  // null, number, object: not a string
            }
            std::string out;
            for (size_t i = p + 1; i < json.size(); ++i) {
                const char c = json[i];
                if (c == '\\' && i + 1 < json.size()) {
                    const char e = json[++i];
                    switch (e) {
                        case 'n': out.push_back('\n'); break;
                        case 't': out.push_back(' '); break;
                        case 'r': break;
                        case 'u': i = std::min(i + 4, json.size() - 1); out.push_back('?'); break;
                        default: out.push_back(e); break;
                    }
                    continue;
                }
                if (c == '"') {
                    if (value) *value = out;
                    return true;
                }
                out.push_back(c);
            }
            return false;
        }
        pos = json.find(pattern, pos + 1);
    }
    return false;
}

// Short radio-friendly text for an api.weather.gov /alerts/active answer
// (GeoJSON FeatureCollection). Each alert becomes
//   "<event> | Severity: <severity> | Expires: <expires>\n<headline>".
inline std::string formatWeatherAlerts(const std::string& json, const std::string& where,
                                       size_t max_alerts = 5, size_t max_len = 1500) {
    if (json.find("\"features\"") == std::string::npos) {
        return "";
    }
    std::string out;
    size_t count = 0;
    size_t total = 0;
    size_t pos = json.find("\"properties\"");
    while (pos != std::string::npos) {
        const size_t next = json.find("\"properties\"", pos + 1);
        const std::string chunk = json.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        pos = next;
        std::string event;
        if (!jsonStringField(chunk, "event", &event) || event.empty()) {
            continue;
        }
        ++total;
        if (count >= max_alerts) {
            continue;
        }
        std::string severity, expires, headline;
        jsonStringField(chunk, "severity", &severity);
        jsonStringField(chunk, "expires", &expires);
        jsonStringField(chunk, "headline", &headline);
        if (!out.empty()) out += "\n\n";
        out += event + " | Severity: " + (severity.empty() ? "Unknown" : severity) +
               " | Expires: " + (expires.empty() ? "N/A" : expires);
        if (!headline.empty()) out += "\n" + headline;
        ++count;
    }
    if (total == 0) {
        return "No active weather alerts for " + where;
    }
    if (total > count) {
        out += "\n\n(+" + std::to_string(total - count) + " more)";
    }
    if (out.size() > max_len) {
        out.resize(max_len);
        out += "...";
    }
    return out;
}

// Prefix for chat/mail a mesh relay delivered to us. The source callsign
// comes from the relay header, which the relaying station can set freely,
// so it is shown as unverified alongside the station that relayed it.
inline std::string relayAttributionMarker(const std::string& relayed_by) {
    if (relayed_by.empty()) {
        return "";
    }
    return "[relayed via " + relayed_by + ", sender unverified] ";
}

inline bool isValidLatLon(double lat, double lon) {
    return std::isfinite(lat) && std::isfinite(lon) &&
           lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

// Remote text headed for a line-oriented service (APRS-IS): drop CR/LF and
// every other control character, then bound the length.
inline std::string sanitizeRemoteText(const std::string& text, size_t max_len) {
    std::string out;
    out.reserve(std::min(text.size(), max_len));
    for (unsigned char c : text) {
        if (out.size() >= max_len) {
            break;
        }
        if (c < 0x20 || c == 0x7F) {
            continue;
        }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

inline bool containsControlChars(const std::string& text) {
    for (unsigned char c : text) {
        if (c < 0x20 || c == 0x7F) {
            return true;
        }
    }
    return false;
}

// RFC 3986 percent-encoding of everything outside the unreserved set.
inline std::string urlEncode(const std::string& text) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (unsigned char c : text) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

// Per-sender reassembly of 0xFD chat fragments. A message is identified by
// its total fragment count and starts afresh whenever fragment 0 arrives
// again or the count changes; incomplete messages expire.
class ChatFragmentAssembler {
public:
    static constexpr int64_t kTimeoutSec = 120;
    static constexpr size_t kMaxSenders = 32;

    // Returns true and fills *complete when `from`'s message is finished.
    bool add(const std::string& from, uint8_t frag_idx, uint8_t total_frags,
             const std::string& data, int64_t now_sec, std::string* complete) {
        if (from.empty() || from == "UNKNOWN" || total_frags == 0 || frag_idx >= total_frags) {
            return false;
        }
        expire(now_sec);
        auto it = partial_.find(from);
        if (it != partial_.end()) {
            Partial& p = it->second;
            const bool restart = p.total != total_frags ||
                                 (frag_idx == 0 && p.frags.count(0) > 0);
            if (restart) {
                partial_.erase(it);
                it = partial_.end();
            }
        }
        if (it == partial_.end()) {
            if (partial_.size() >= kMaxSenders) {
                evictOldest();
            }
            Partial fresh;
            fresh.total = total_frags;
            fresh.started = now_sec;
            it = partial_.emplace(from, std::move(fresh)).first;
        }
        Partial& p = it->second;
        p.last = now_sec;
        if (p.frags.count(frag_idx) > 0) {
            return false;  // Duplicate fragment
        }
        p.frags[frag_idx] = data;
        if (p.frags.size() < p.total) {
            return false;
        }
        std::string full;
        for (const auto& frag : p.frags) {
            full += frag.second;
        }
        partial_.erase(it);
        if (complete) {
            *complete = std::move(full);
        }
        return true;
    }

    void expire(int64_t now_sec) {
        for (auto it = partial_.begin(); it != partial_.end();) {
            if (now_sec - it->second.last > kTimeoutSec) {
                it = partial_.erase(it);
            } else {
                ++it;
            }
        }
    }

    size_t pendingSenders() const { return partial_.size(); }

private:
    struct Partial {
        uint8_t total = 0;
        int64_t started = 0;
        int64_t last = 0;
        std::map<uint8_t, std::string> frags;
    };

    void evictOldest() {
        auto oldest = partial_.begin();
        for (auto it = partial_.begin(); it != partial_.end(); ++it) {
            if (it->second.last < oldest->second.last) {
                oldest = it;
            }
        }
        if (oldest != partial_.end()) {
            partial_.erase(oldest);
        }
    }

    std::map<std::string, Partial> partial_;
};

}  // namespace app_detail

// Forward declarations for modular windows
class FileWindow;
class MessageWindow;
class PositionWindow;
class SettingsWindow;
class WeatherWindow;
class MailWindow;
class BBSWindow;
class FrequencyWindow;
class MainContentRenderer;
class StationMapWindow;
class PskReporterClient;  // src/core/psk_reporter.hpp (R1)

class App {
public:
    App();
    ~App();

    // Non-copyable
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Lifecycle
    bool initialize();
    void run();
    void shutdown();

    // Configuration
    AppConfig& config() { return config_; }
    const AppConfig& config() const { return config_; }

private:
    // SDL/OpenGL/ImGui
    SDL_Window* window_ = nullptr;
    SDL_GLContext gl_context_ = nullptr;

    // Core components
    AppConfig config_;
    std::string config_path_;   // where settings are saved
    std::unique_ptr<ModemClient> modem_;
    std::unique_ptr<RadioController> radio_;
    std::unique_ptr<MessageStore> store_;
    std::unique_ptr<FrequencyManager> freq_mgr_;
    std::unique_ptr<APRSClient> aprs_;
    std::unique_ptr<FileTransferManager> file_transfer_mgr_;
    std::unique_ptr<MeshRelay> mesh_relay_;

    // Shared state for modular windows
    SharedAppState shared_state_;

    // Modular windows
    std::unique_ptr<FileWindow> file_window_;
    std::unique_ptr<MessageWindow> message_window_;
    std::unique_ptr<PositionWindow> position_window_;
    std::unique_ptr<SettingsWindow> settings_window_;
    std::unique_ptr<WeatherWindow> weather_window_;
    std::unique_ptr<MailWindow> mail_window_;
    std::unique_ptr<BBSWindow> bbs_window_;
    std::unique_ptr<FrequencyWindow> frequency_window_;
    std::unique_ptr<MainContentRenderer> main_content_;
    std::unique_ptr<StationMapWindow> station_map_window_;

    // UI state
    AppTab current_tab_ = AppTab::Chat;
    bool show_settings_ = false;
    bool close_tool_windows_ = false;   // close every floating window next frame
    bool running_ = true;
    bool shutdown_done_ = false;
    bool imgui_initialized_ = false;
    std::chrono::steady_clock::time_point app_start_time_;
    int auto_exit_after_ms_ = 0;

    // Chat state
    std::vector<ChatMessage> chat_history_;
    char chat_input_[1024] = {0};
    std::string current_chat_partner_;
    bool chat_scroll_to_bottom_ = false;

    static constexpr size_t MAX_CHAT_HISTORY = 2000;

    // Message fragmentation
    static constexpr size_t MAX_CHAT_FRAGMENT_SIZE = 200;
    app_detail::ChatFragmentAssembler fragment_assembler_;

    // Incoming chat/mail sound (ui.sound_notifications)
    NotificationSound notification_sound_;
    void playNotificationSound();

    // File transfer state
    std::vector<FileTransfer> transfers_;
    int unread_file_notifications_ = 0;
    char file_path_input_[512] = {0};

    // Message state
    std::vector<Message> inbox_messages_;
    std::vector<Message> outbox_messages_;
    int selected_message_ = -1;

    // Position state
    double my_lat_ = 0.0;
    double my_lon_ = 0.0;
    char position_comment_[256] = {0};

    // Settings state
    char callsign_input_[32] = {0};
    char modem_host_input_[256] = {0};
    // Last FREQINFO pushed to the modem; kDialUnreported forces a push (of
    // the rig-confirmed frequency, or 0 = unknown) after a modem connect.
    static constexpr uint32_t kDialUnreported = 0xFFFFFFFFu;
    uint32_t last_reported_dial_hz_ = kDialUnreported;
    // Modem FSM state last announced in the log (dedupes STATE echoes).
    std::string last_logged_fsm_state_;
    int modem_port_input_ = kDefaultCommandPort;

    // Live voice state
    bool voice_auto_started_ = false;
    bool voice_start_pending_ = false;
    bool voice_start_inflight_ = false;
    bool voice_local_ptt_ = false;
    bool voice_ptt_after_start_ = false;
    bool voice_ui_ptt_active_ = false;
    bool voice_hotkey_capture_active_ = false;
    bool voice_hotkey_ptt_active_ = false;
    uint32_t voice_playback_samples_queued_ = 0;
    bool voice_playback_started_ = false;
    std::deque<int16_t> voice_playback_buffer_;
    std::chrono::steady_clock::time_point last_voice_status_poll_;
    std::chrono::steady_clock::time_point voice_start_attempted_at_{};
    uint32_t voice_capture_device_id_ = 0;
    uint32_t voice_playback_device_id_ = 0;
    bool voice_audio_ready_ = false;
    // Uplink PCM records ModemClient::sendVoicePcm refused (backlog full or
    // no voice socket), reported to the operator at most every 10 s (K8).
    uint64_t voice_tx_dropped_ = 0;
    uint64_t voice_tx_dropped_reported_ = 0;
    std::chrono::steady_clock::time_point voice_tx_drop_reported_at_{};
    void noteVoiceSendResult(bool queued);
    // Voice devices open on a helper thread, and only while voice can be
    // used: opening them at start-up blocked the first frame inside
    // CoreAudio (microphone permission) and kept the mic live all day.
    struct VoiceOpenJob {
        std::atomic<bool> done{false};
        uint32_t capture = 0, playback = 0;
        std::string error;
    };
    std::shared_ptr<VoiceOpenJob> voice_open_job_;
    // Modem auto-reconnect (maybeReconnectModem).
    bool modem_auto_paused_ = false;
    bool modem_unreachable_logged_ = false;
    std::chrono::steady_clock::time_point modem_next_attempt_{};
    void startModemConnect(bool announce);
    void maybeReconnectModem();
    std::chrono::steady_clock::time_point voice_open_retry_at_{};
    bool voiceAudioWanted();
    void updateVoiceAudioDevices();
    static constexpr size_t VOICE_PLAYBACK_PREBUFFER_SAMPLES = 3200;
    static constexpr size_t VOICE_PLAYBACK_REBUFFER_SAMPLES = 1600;
    static constexpr size_t VOICE_PLAYBACK_MIN_CHUNK_SAMPLES = 800;

    // CQ/Broadcast state
    bool cq_active_ = false;
    int cq_repeat_count_ = 1;
    int cq_current_repeat_ = 0;
    std::chrono::steady_clock::time_point last_cq_sent_;
    static constexpr int CQ_INTERVAL_MS = 10000;
    char broadcast_message_[512] = {0};
    // "Last heard CQ calls" with band/family/SNR (G19).
    std::deque<HeardCall> heard_calls_;
    static constexpr size_t MAX_HEARD_CALLS = 50;

    // Channel sniffer state
    bool channel_sniffer_active_ = false;
    int sniffer_current_slot_ = 0;
    std::string sniffer_band_ = "20m";
    std::vector<std::pair<int, bool>> slot_status_;
    uint64_t sniffer_return_freq_hz_ = 0;  // dial frequency to go back to

    // GPS state
#ifdef _WIN32
    void* gps_handle_ = nullptr;
#else
    int gps_serial_fd_ = -1;
#endif
    std::string gps_port_;
    int gps_baud_ = 9600;
    std::string gps_buffer_;
    bool gps_fix_valid_ = false;
    std::chrono::steady_clock::time_point last_gps_update_;
    static constexpr int GPS_FIX_STALE_MS = 10000;

    // Email gateway state
    std::vector<Message> email_outbox_;
    std::vector<Message> email_inflight_;   // being delivered by email_worker_
    bool email_send_requested_ = false;
    EmailGatewayWorker email_worker_;
    std::vector<Message> radio_outbox_;
    bool email_gateway_active_ = false;
    std::chrono::steady_clock::time_point last_email_check_;
    std::chrono::steady_clock::time_point last_pop3_check_;
    std::chrono::steady_clock::time_point last_winlink_check_;
    // Outbound internet mail is retried on its own timer, independent of
    // the POP3/Winlink fetch schedule (SMTP-only setups never fetch).
    std::chrono::steady_clock::time_point last_outbox_retry_;
    static constexpr int EMAIL_OUTBOX_RETRY_SEC = 60;

    // BBS state
    struct PendingRemoteBbsDownload {
        std::string filename;
        std::string owner_callsign;
        int64_t requested_at = 0;
        bool request_sent = false;
    };
    std::vector<BBSFile> bbs_files_;
    std::unordered_map<std::string, BBSFile> remote_bbs_files_;
    static constexpr size_t MAX_REMOTE_BBS_FILES = 500;
    std::unordered_map<std::string, PendingRemoteBbsDownload> pending_remote_bbs_downloads_;
    std::vector<PendingBbsDownload> pending_bbs_downloads_ui_;
    static constexpr int64_t REMOTE_BBS_DOWNLOAD_REQUEST_TTL_SEC = 180;
    char bbs_upload_path_[512] = {0};
    char bbs_file_desc_[256] = {0};

    // Frequency management state
    std::string current_band_ = "20m";
    int current_slot_ = 0;
    bool qsy_suggestion_pending_ = false;
    std::string qsy_suggestion_from_;
    std::string qsy_suggestion_band_;
    int qsy_suggestion_slot_ = 0;
    uint32_t qsy_suggestion_frequency_hz_ = 0;
    int64_t qsy_suggestion_received_unix_ = 0;
    enum class QsyWorkflowState {
        Idle,
        AwaitingAck,
        SidebandActive,
        ReconnectPending,
        ReconnectConnecting
    };
    QsyWorkflowState qsy_workflow_state_ = QsyWorkflowState::Idle;
    bool qsy_initiator_ = false;
    bool pending_connect_auto_qsy_ = false;
    std::string qsy_workflow_peer_;
    std::string pending_connect_target_;
    std::string qsy_target_band_;
    int qsy_target_slot_ = 0;
    uint32_t qsy_target_frequency_hz_ = 0;
    int qsy_reconnect_attempts_ = 0;
    std::chrono::steady_clock::time_point qsy_ack_deadline_;
    std::chrono::steady_clock::time_point qsy_sideband_started_;
    std::chrono::steady_clock::time_point qsy_reconnect_due_;
    std::chrono::steady_clock::time_point qsy_connect_attempt_started_;

    // Beacon state
    std::deque<HeardBeacon> heard_beacons_;
    static constexpr size_t MAX_HEARD_BEACONS = 100;
    std::chrono::steady_clock::time_point last_beacon_sent_;
    bool beacon_enabled_ = false;
    std::vector<StationLocationEntry> station_locations_;
    static constexpr size_t MAX_STATION_LOCATIONS = 250;

    // QSO state
    std::chrono::steady_clock::time_point qso_start_time_;
    bool qso_active_ = false;

    // Away mode state
    bool away_mode_active_ = false;

    // Ping state
    std::deque<PingResult> ping_results_;
    static constexpr size_t MAX_PING_RESULTS = 50;
    bool ping_in_progress_ = false;
    std::string ping_target_;
    std::chrono::steady_clock::time_point ping_sent_time_;
    static constexpr int PING_TIMEOUT_MS = 120000;  // two broadcast frames at a robust mode

    // Mail state
    std::vector<MailMessage> mail_inbox_;
    std::vector<MailMessage> mail_outbox_;
    // Legacy 0xE2/0xE3 mail has no id: sender|content-hash -> first seen.
    std::unordered_map<std::string, int64_t> legacy_mail_seen_;
    static constexpr int64_t LEGACY_MAIL_DEDUP_WINDOW_SEC = 600;
    // Private mail handed to the mesh: no ACK can come back over a link, so
    // it is not retried on an ACK timeout.
    std::unordered_set<std::string> mesh_sent_mail_ids_;
    char mail_to_input_[256] = {0};
    char mail_subject_input_[128] = {0};
    char mail_body_input_[2048] = {0};
    int mail_priority_input_ = 0;  // 0=routine, 1=priority, 2=emergency
    int selected_mail_ = -1;
    static constexpr int MAIL_ACK_TIMEOUT_MS = 15000;
    static constexpr int MAIL_MAX_RETRIES = 3;
    // Automatic gateway calls (queued internet mail): never while a link
    // or call is in progress, at most GATEWAY_MAX_CALL_ATTEMPTS per batch,
    // with exponential backoff (app_detail::gatewayRetryDelaySec).
    static constexpr int GATEWAY_MAX_CALL_ATTEMPTS = 5;
    int gateway_call_attempts_ = 0;
    bool gateway_gave_up_logged_ = false;
    bool gateway_waiting_logged_ = false;
    std::chrono::steady_clock::time_point gateway_next_call_{};
    std::string auto_gateway_target_;
    struct GatewayStats {
        float snr_accum = 0.0f;
        int snr_samples = 0;
        int relay_success = 0;
        int relay_fail = 0;
        int64_t last_seen = 0;
    };
    std::map<std::string, GatewayStats> gateway_stats_;
    RelayDiagnostics relay_diagnostics_;
    void recordGatewaySighting(const std::string& callsign, std::optional<float> snr_db,
                               int64_t when);
    bool tryAutoConnectGateway(const std::string& why);

    // Mesh relay: forwards that could not be handed to the modem are
    // retried a few times before they are dropped (with a message).
    struct MeshRetry {
        RelayQueueEntry entry;
        int attempts = 0;
        std::chrono::steady_clock::time_point next_try{};
    };
    std::deque<MeshRetry> mesh_retry_;
    static constexpr int MESH_MAX_SEND_ATTEMPTS = 3;
    std::chrono::steady_clock::time_point last_mesh_prune_{};
    static constexpr int MESH_ROUTE_MAX_AGE_SEC = 1800;

    // Store maintenance (store.auto_cleanup / message_expiry_days)
    std::chrono::steady_clock::time_point next_store_maintenance_{};
    void runStoreMaintenance();

    // Timing
    std::chrono::steady_clock::time_point last_poll_;
    std::chrono::steady_clock::time_point last_status_update_;

    // Initialization
    bool initSDL();
    bool initImGui();
    void shutdownVoiceAudio();
    void pumpVoiceAudio();
    void applyVoiceSettingsToModem(bool restart_if_open);
    void applyModemSettingsToModem();
    // Ask the modem to switch bandwidth family (ULTRA ON first when needed).
    // On refusal the setting reverts to the modem's actual family and the
    // reason is logged.
    void setModemFamily(const std::string& family);
    // Read the modem's FAMILY into the settings (the modem is the authority).
    void syncFamilyFromModem();
    // Tell the modem to key the rig through this app's PTT server.
    void pushPttSettingsToModem();
    // Log a refused/unanswered modem command in plain words.
    void logCommandFailure(const std::string& what, const std::string& cmd,
                           const std::string& response);
    bool setVoicePttState(bool enabled);
    bool handleVoiceHotkeyEvent(const SDL_KeyboardEvent& key, bool pressed);
    bool handleShortcutKey(const SDL_KeyboardEvent& key);
    bool openVoiceAudioDevices();
    void closeVoiceAudioDevices();
    void setupStyle();
    void initializeWindows();
    void initializeSharedState();
    bool saveConfig(bool announce);

    // Main loop
    void processEvents();
    void update();
    // Scripted test hook (SKYLINK_TEST_SCRIPT=<file>): runs one action
    // per line through the app's own methods from the update loop, so
    // end-to-end tests drive exactly the code the GUI drives. See
    // docs/TESTING_HOOK.md.
    void loadTestScript();
    void runTestScript();
    void dumpTestReport(const std::string& path);
    std::vector<std::string> test_script_;
    size_t test_pc_ = 0;
    std::chrono::steady_clock::time_point test_wait_until_{};
    std::string test_wait_cond_;
    std::chrono::steady_clock::time_point test_cond_deadline_{};
    std::ofstream test_log_;
    void testLog(const std::string& line);
    std::string test_shot_path_;  // "screenshot" action: read back next frame
    void render();

    // UI rendering
    void renderMenuBar();
    void renderStatusBar();

    // Where a received payload came from. Only Link traffic is from the
    // connected peer; Broadcast carries the modem-reported source; Mesh is
    // an inner payload delivered by a relay packet.
    enum class RxOrigin { Link, Broadcast, Mesh };

    // Event handlers
    void onModemStateChange(const std::string& state);
    void onDataReceived(const std::vector<uint8_t>& data);
    void onMonitorFrame(const MonitorFrame& frame);
    // Fires once a BCAST_DATA fragment chain has been reassembled.
    void onBroadcastMessage(const std::string& sender,
                            const std::vector<uint8_t>& payload);
    void onConnected(const std::string& callsign);
    void onDisconnected(const std::string& reason);
    void onModemEvent(const std::string& name, const std::string& args);

    // Actions
    void connectToModem();
    void pollModemConnect();   // finishes a background connectToModem()
    void pollAPRSConnect();    // finishes a background APRS-IS login
    void applyAPRSConfig();    // (re)connect APRS-IS per the settings
    // Native pickers run on a helper thread; the result is handed back to
    // the GUI thread by pollPathPicker(). One picker at a time.
    std::string imgui_ini_path_;   // must outlive the ImGui context
    struct PathPickerJob;
    std::shared_ptr<PathPickerJob> path_picker_job_;
    PathPickedCallback path_picker_done_;
    void startPathPicker(bool folder, PathPickedCallback done);
    void pollPathPicker();
    void onModemConnected();
    void disconnectFromModem();
    void connectToStation(const std::string& callsign);
    bool sendChatMessage(const std::string& text);
    // Offers a file to the linked station; kind BbsUpload asks it to store
    // the file in its BBS folder (K21). False, with the reason logged, when
    // no transfer was started.
    bool sendFile(const std::string& path, FileOfferKind kind = FileOfferKind::Direct);
    void updatePosition(double lat, double lon);
    void applyRadioConfig();

    // BBS functions
    void refreshBBSFiles();
    void uploadToBBS(const std::string& filepath, const std::string& description);
    void downloadFromBBS(const BBSFile& file);
    void downloadFromBBS(const std::string& filename);
    void deleteBBSFile(const std::string& filename);
    void announceBBSFile(const BBSFile& file);
    void announceBBSFileRemoval(const std::string& filename);
    void processBBSAnnouncement(const std::string& from, const std::vector<uint8_t>& data);
    void processBBSRemoval(const std::string& from, const std::vector<uint8_t>& data);
    void processBBSFileRequest(const std::string& from, const std::vector<uint8_t>& data);
    void requestRemoteBBSDownload(const std::string& from, const std::string& filename);
    bool sendRemoteBbsFileRequest(const std::string& owner, const std::string& filename, bool emit_chat);
    bool flushPendingRemoteBbsDownloadsForPeer(const std::string& peer, bool emit_chat = true);
    void prunePendingRemoteBbsDownloads();
    bool shouldAcceptRemoteBbsOffer(const std::string& from, const std::string& filename);
    void syncPendingBbsDownloadsForUi();
    void upsertRemoteBBSFile(const std::string& from, const BBSFile& file);
    void removeRemoteBBSFile(const std::string& from, const std::string& filename);

    // Message handling
    void refreshMessages();
    void sendPendingMessages();
    void processIncomingMessage(const std::vector<uint8_t>& data, const std::string& from_override = "",
                                RxOrigin origin = RxOrigin::Link, int relay_depth = 0,
                                const std::string& relayed_by = "");
    void receiveChatText(const std::string& from, const std::string& text,
                         const std::string& relayed_by, bool via_link);
    bool isLinkConnected() const;
    std::string linkedPeerCallsign() const;
    void appendSystemChat(const std::string& text);
    // Deferred incoming file offers (Incoming File prompt, test hook).
    bool decidePendingFileOffer(const std::string& transfer_id, bool accept);
    bool decideNewestPendingFileOffer(bool accept);
    void dropPendingFileOffers();
    void renderPendingFileOffers();
    void pruneFragmentBuffers();
    void gracefulDisconnectModem();

    // Chat fragmentation
    bool sendFragmentedMessage(const std::string& text);
    void processMessageFragment(const std::string& from, const std::vector<uint8_t>& data);

    // CQ and broadcast
    void startCQ();
    void stopCQ();
    void updateCQ();
    void sendBroadcast(const std::string& message);
    void processCQResponse(const std::string& from, const std::string& family = "");
    bool sendBroadcastPayload(const std::vector<uint8_t>& payload);
    void sendQsySuggestion(const std::string& band, int slot, uint32_t frequency_hz);
    void processQsySuggestion(const std::string& from, const std::vector<uint8_t>& data);
    void processQsyConfirm(const std::string& from, const std::vector<uint8_t>& data);
    void respondToQsySuggestion(bool accept);
    void updateQsyWorkflow();
    void resetQsyWorkflow(bool clear_peer);
    bool executeQsyTarget(const std::string& band, int slot, uint32_t frequency_hz,
                          std::string* why = nullptr);
    bool qsyTargetInBand(const std::string& band, int slot, uint32_t frequency_hz) const;
    bool returnToCallingFrequency(const std::string& reason);
    bool buildAutoQsyTarget(std::string* band_out, int* slot_out, uint32_t* freq_hz_out) const;
    void recordHeardCall(const std::string& callsign, const std::string& family,
                         std::optional<float> snr);

    // Radio tuning. radioCanTune() is true only for a rig that really
    // moves (connected CAT control, not dry-run); `why` says what is missing.
    bool radioCanTune(std::string* why = nullptr) const;
    // The rig-confirmed dial frequency, if there is one.
    std::optional<uint64_t> knownDialFrequencyHz() const;
    bool tuneOperatingFrequency(uint64_t freq_hz, bool update_selection = true,
                                std::string* why = nullptr);
    bool setOperatingFrequencyFromUi(uint64_t freq_hz);

    // Channel sniffer
    void startChannelSniffer();
    void stopChannelSniffer();
    void finishChannelSniffer(const std::string& message);
    void updateChannelSniffer();

    // GPS
    bool openGPSPort(const std::string& port, int baud);
    void closeGPSPort();
    void updateGPS();
    void processGPSData();
    void parseNMEASentence(const std::string& sentence);

    // Email gateway
    void startEmailGateway();
    void stopEmailGateway();
    void updateEmailGateway();
    void processIncomingEmail();
    void keepInboundLocally(const std::string& from, const std::string& to, const std::string& subject,
                            const std::string& body, const std::string& message_id);
    void relayRadioToEmail(const Message& msg);
    // Queue an internet email from this station (Messages window). Returns
    // true only when it was validated and queued; otherwise false with the
    // reason in *error (when given) and in the log.
    bool queueOutgoingEmail(const std::string& to, const std::string& subject,
                            const std::string& body, const std::string& from,
                            std::string* error = nullptr);
    // Gateway network I/O runs on email_worker_; these run on the GUI thread.
    bool startEmailGatewayJob(bool poll_pop3, bool poll_winlink, bool send_outbox);
    void pollEmailGatewayJob();
    void applyInboundEmail(const EmailGatewayJobOutput& output);
    void prepareQueuedEmails(EmailGatewayJobInput& input);
    void applyEmailSendResults(const EmailGatewayJobOutput& output);
    void sendQueuedEmails();   // starts (or requests) a delivery job

    // Beacon system
    void sendBeacon();
    void updateBeacon();
    void processReceivedBeacon(const std::string& from, const std::string& grid_square,
                               const std::string& message, std::optional<float> snr,
                               bool email_gateway = false, const std::string& family = "");
    void checkPendingMessagesForStation(const std::string& callsign);
    void recordStationLocation(const std::string& callsign,
                               bool has_coordinates,
                               double latitude,
                               double longitude,
                               const std::string& grid_square,
                               const std::string& details,
                               StationLocationSource source,
                               std::optional<float> snr = std::nullopt,
                               bool email_gateway = false,
                               int64_t timestamp = 0);
    void seedStationLocationsFromHistory();
    void onAprsPositionReceived(const APRSPosition& pos);

    // QSO management
    void startQSO(const std::string& callsign);
    void endQSO();

    // Away mode
    void enableAwayMode();
    void disableAwayMode();
    void processAwayModeRequest(const std::string& from, const std::vector<uint8_t>& data);
    void sendAwayModeResponse(const std::string& to, const std::string& response);
    // EMAIL:<to>:<subject>:<body> from the linked station while away.
    // Returns the reply sent to the requester.
    std::string handleAwayEmailCommand(const std::string& from, const app_detail::AwayEmailCommand& cmd);
    std::string awayCommandHelp() const;

    // Ping
    void sendPing(const std::string& callsign);
    void processPingRequest(const std::string& from, const std::vector<uint8_t>& data);
    void processPingResponse(const std::string& from, const std::vector<uint8_t>& data);
    void updatePing();

    // Mail
    void sendMail(const std::string& to, const std::string& subject, const std::string& body);
    void sendBroadcastMail(const std::string& subject, const std::string& body);
    void processMailReceived(const std::string& from, const std::vector<uint8_t>& data, bool via_link,
                             const std::string& relayed_by = "");
    void processReliableMailReceived(const std::string& from, const std::vector<uint8_t>& data, bool is_broadcast,
                                     bool via_link,
                                     const std::string& relayed_by = "");
    void processMailAck(const std::string& from, const std::vector<uint8_t>& data);
    void sendMailAck(const std::string& message_id, bool ack, const std::string& reason = "");
    void updateReliableMail();
    void processQueuedGatewayMail();
    bool sendMailWirePacket(const MailMessage& mail);
    void recordGatewayRelayResult(const std::string& gateway_callsign, bool success);
    bool isKnownEmailGateway(const std::string& callsign) const;
    bool isWeakSignalLink(const std::string& callsign) const;
    std::string selectBestGateway(float* score_out = nullptr) const;
    void persistMailReadState(const MailMessage& mail);
    void loadMailFromStore();
    void saveMailToStore();
    bool deleteStoredMessage(int64_t store_id, const std::string& message_id);
    bool forwardPacketViaMesh(const std::vector<uint8_t>& payload,
                              const std::string& destination_callsign,
                              const std::string& message_id = "");
    void applyMeshRelaySettings();
    void flushMeshRelayQueue();

    // File transfer helpers
    void syncTransfersFromManager();

    // Weather functions. ALERT (0x05) asks the linked station for the
    // active weather alerts at a location (grid, "lat,lon", place name or
    // two-letter US state).
    bool requestWeather(uint8_t type, const std::string& query);
    void requestWeatherForecast(const std::string& location) { requestWeather(0x01, location); }
    void requestMETAR(const std::string& airport_code) { requestWeather(0x02, airport_code); }
    void requestTAF(const std::string& airport_code) { requestWeather(0x03, airport_code); }
    void requestMarineWeather(const std::string& zone) { requestWeather(0x04, zone); }
    void requestWeatherAlerts(const std::string& location) { requestWeather(0x05, location); }
    void cancelWeatherRequest();
    bool isStationLinkConnected(bool refresh = false);
    void processWeatherMessage(const std::string& from, const std::vector<uint8_t>& data);
    void handleWeatherRequest(uint8_t request_type, const std::string& query, const std::string& requester);
    bool sendWeatherResponse(uint8_t type, const std::string& location, const std::string& report);
    void fetchWeatherAsync(uint8_t type, const std::string& query, const std::string& requester);
    static std::string fetchWeatherReport(uint8_t type, const std::string& query);
    void processWeatherFetchResult();
    void loadWeatherCache();

    // Weather helpers
    static bool gridSquareToLatLon(const std::string& grid, double& lat, double& lon);
    static std::string httpGet(const std::string& host, const std::string& path, bool use_ssl = true);
    static std::string parseWeatherResponse(uint8_t type, const std::string& raw_response);
    static std::string extractForecastText(const std::string& json);

    // Weather state
    std::vector<WeatherReport> weather_reports_;
    char weather_location_input_[64] = {0};
    char weather_airport_input_[8] = {0};
    char weather_zone_input_[16] = {0};
    bool weather_request_pending_ = false;
    std::chrono::steady_clock::time_point weather_request_time_;
    static constexpr int WEATHER_TIMEOUT_MS = 30000;

    // Remote weather requests: one worker thread drains a bounded queue,
    // one job per requester. The thread holds a shared_ptr to this state,
    // never to App, so shutdown with a fetch in flight is safe.
    struct WeatherJob {
        uint8_t type = 0;
        std::string query;
        std::string requester;
        std::string result;
    };
    struct WeatherWorker {
        static constexpr size_t kMaxQueued = 4;
        std::mutex mutex;
        std::deque<WeatherJob> queue;
        std::vector<WeatherJob> done;
        std::vector<WeatherJob> rejected;
        std::string active_requester;
        bool running = false;
    };
    std::shared_ptr<WeatherWorker> weather_worker_ = std::make_shared<WeatherWorker>();

    // PSK Reporter (R1): spots go to PskReporterClient, which uploads IPFIX
    // on its own cadence from a helper thread. psk_status_ is its state for
    // the Settings status line.
    std::unique_ptr<PskReporterClient> psk_client_;
    PskReporterStatus psk_status_;
    void refreshPskStatus();
    std::string psk_last_error_shown_;
    std::string psk_receiver_key_;
    uint64_t psk_last_upload_shown_ = 0;
    std::chrono::steady_clock::time_point last_psk_poll_{};
    // Rig-confirmed dial frequency (0 = unknown), for display.
    uint32_t current_frequency_hz_ = 0;

    // PSK Reporter functions
    void queuePSKReporterSpot(const std::string& callsign, const std::string& grid,
                              std::optional<float> snr);
    void uploadPSKReporterSpots();
    void updatePSKReporter();
};

} // namespace skylink
