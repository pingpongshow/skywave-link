// ModemClient implementation - TCP client for Skywave modem

#include "skywave_link/modem_client.hpp"
#include <iostream>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <chrono>
#include <thread>
#include <limits>
#include <cctype>
#include <string_view>
#include <algorithm>
#include <utility>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
#endif

namespace skylink {

namespace {

constexpr uint8_t kVoiceMediaVersion = 1u;
constexpr uint8_t kVoiceMediaUplinkPcm = 1u;
constexpr uint8_t kVoiceMediaDownlinkPcm = 2u;
constexpr size_t kVoiceMediaHeaderLen = 16u;

// Data-port binary monitor record (INTERFACES.md "Data Port"): tag, frame
// type, family, mode, 24-bit source/destination hashes, BE length, payload.
constexpr uint8_t kMonitorRecordTag = 0xFBu;

// A connected session is a plain byte stream: the modem coalesces and splits
// writes freely, so two app messages can arrive as one read or one message
// as two. Each message written while connected is therefore framed as
// [0xA5 0x5A][len u16 BE][message] and the receiver hands every complete
// message to the data callback on its own.
constexpr uint8_t kSessionMagic0 = 0xA5u;
constexpr uint8_t kSessionMagic1 = 0x5Au;
constexpr size_t kSessionHeaderLen = 4;
constexpr size_t kSessionMaxMessage = 0xFFFFu;
constexpr size_t kMonitorRecordHeaderLen = 12u;

std::string trimCopy(std::string_view value) {
    size_t start = 0;
    while (start < value.size() &&
           std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    size_t end = value.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return std::string(value.substr(start, end - start));
}

std::string upperCopy(std::string_view value) {
    std::string upper(value);
    for (char& ch : upper) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return upper;
}

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.substr(0, prefix.size()) == prefix;
}

uint16_t loadU16Be(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) |
                                 static_cast<uint16_t>(data[1]));
}

void storeU16Be(uint8_t* data, uint16_t value) {
    data[0] = static_cast<uint8_t>(value >> 8);
    data[1] = static_cast<uint8_t>(value & 0xFFu);
}

void storeU32Be(uint8_t* data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value >> 24);
    data[1] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    data[2] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    data[3] = static_cast<uint8_t>(value & 0xFFu);
}

void storeU64Be(uint8_t* data, uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        data[i] = static_cast<uint8_t>(value & 0xFFu);
        value >>= 8;
    }
}

bool parseBoolToken(std::string_view value) {
    const std::string upper = upperCopy(trimCopy(value));
    return upper == "TRUE" || upper == "ON" || upper == "1" || upper == "YES";
}

VoiceMode parseVoiceMode(std::string_view value) {
    const std::string upper = upperCopy(trimCopy(value));
    if (upper == "CONNECTED") return VoiceMode::Connected;
    if (upper == "BROADCAST") return VoiceMode::Broadcast;
    return VoiceMode::Off;
}

// Skywave VoiceFsm state names (worker.cpp voiceStateName).
VoiceState parseVoiceState(std::string_view value) {
    const std::string upper = upperCopy(trimCopy(value));
    if (upper == "OPENING") return VoiceState::Opening;
    if (upper == "OPEN") return VoiceState::Ready;
    if (upper == "REQUESTING") return VoiceState::TxRequested;
    if (upper == "TALKING") return VoiceState::TxActive;
    if (upper == "RECEIVING") return VoiceState::RxActive;
    if (upper == "CLOSING") return VoiceState::Closing;
    return VoiceState::Idle;
}

std::string voiceModeCommand(VoiceMode mode) {
    switch (mode) {
        case VoiceMode::Connected: return "CONNECTED";
        case VoiceMode::Broadcast: return "BROADCAST";
        case VoiceMode::Off:
        default: return "OFF";
    }
}

// Skywave reports LISTENING as its own FSM state; the app treats it as idle
// and reads ModemStatus::listening for the distinction.
LinkState mapStateToLinkState(std::string_view state) {
    if (state == "CALLING" || state == "CONNECTING") {
        return LinkState::Connecting;
    }
    if (state == "CONNECTED") {
        return LinkState::Connected;
    }
    if (state == "DISCONNECTING") {
        return LinkState::Disconnecting;
    }
    return LinkState::Idle;  // IDLE, LISTENING, anything unrecognized
}

// "M8" / "AUTO(M8)" -> ladder index, -1 when absent.
int parseModeIndex(std::string_view mode) {
    const std::string upper = upperCopy(trimCopy(mode));
    const size_t m = upper.find('M');
    if (m == std::string::npos) return -1;
    int value = 0;
    size_t i = m + 1;
    if (i >= upper.size() || std::isdigit(static_cast<unsigned char>(upper[i])) == 0) {
        return -1;
    }
    for (; i < upper.size() && std::isdigit(static_cast<unsigned char>(upper[i])) != 0; ++i) {
        value = value * 10 + (upper[i] - '0');
    }
    return value;
}

// Splits "KEY=value KEY2=value2" into pairs; keys are upper-cased.
std::vector<std::pair<std::string, std::string>> splitFields(
    std::string_view fields) {
    std::vector<std::pair<std::string, std::string>> out;
    std::istringstream tokens{std::string(fields)};
    std::string token;
    while (tokens >> token) {
        const size_t eq = token.find('=');
        if (eq == std::string::npos) continue;
        out.emplace_back(upperCopy(token.substr(0, eq)), token.substr(eq + 1));
    }
    return out;
}

float parseFloatOr(const std::string& value, float fallback) {
    try {
        return std::stof(value);
    } catch (...) {
        return fallback;
    }
}

long long parseIntOr(const std::string& value, long long fallback) {
    try {
        return std::stoll(value);
    } catch (...) {
        return fallback;
    }
}

std::string bytesToHex(const std::vector<uint8_t>& data) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(data.size() * 2);
    for (uint8_t byte : data) {
        out.push_back(kHex[(byte >> 4) & 0x0Fu]);
        out.push_back(kHex[byte & 0x0Fu]);
    }
    return out;
}

bool parseHexBytes(std::string_view hex, std::vector<uint8_t>* out) {
    if (!out) {
        return false;
    }
    out->clear();
    if ((hex.size() % 2u) != 0u) {
        return false;
    }
    out->reserve(hex.size() / 2u);
    for (size_t i = 0; i < hex.size(); i += 2u) {
        const auto hi = static_cast<unsigned char>(hex[i]);
        const auto lo = static_cast<unsigned char>(hex[i + 1u]);
        if (!std::isxdigit(hi) || !std::isxdigit(lo)) {
            out->clear();
            return false;
        }
        const std::string byte_hex(hex.substr(i, 2u));
        out->push_back(static_cast<uint8_t>(std::stoul(byte_hex, nullptr, 16)));
    }
    return true;
}

// `NOISEFLOOR <dbfs> BUSY=<0|1>` (worker.cpp noiseFloor()).
std::optional<NoiseFloorReading> parseNoiseFloorResponse(
    std::string_view response) {
    const std::string line = trimCopy(response);
    if (!startsWith(line, "NOISEFLOOR ")) {
        return std::nullopt;
    }

    std::istringstream tokens(line.substr(11));
    std::string first;
    if (!(tokens >> first)) return std::nullopt;

    NoiseFloorReading reading;
    try {
        reading.noise_dbfs = std::stof(first);
    } catch (...) {
        return std::nullopt;
    }

    std::string token;
    while (tokens >> token) {
        const size_t eq = token.find('=');
        if (eq == std::string::npos) continue;
        if (upperCopy(token.substr(0, eq)) == "BUSY") {
            reading.busy = parseBoolToken(token.substr(eq + 1));
        }
    }
    return reading;
}

} // namespace


bool ModemClient::sockets_initialized_ = false;

bool ModemClient::initSockets() {
    if (sockets_initialized_) return true;

#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        return false;
    }
#endif
    sockets_initialized_ = true;
    return true;
}

ModemClient::ModemClient() {
    initSockets();
}

ModemClient::~ModemClient() {
    disconnect();
}

bool ModemClient::connectSocket(socket_t& sock, const std::string& host, uint16_t port) {
    struct addrinfo hints{}, *result = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0) {
        return false;
    }

    // Try every resolved address: "localhost" typically yields ::1 first,
    // while the modem may listen on 127.0.0.1 only.
    sock = INVALID_SOCK;
    for (struct addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == INVALID_SOCK) {
            continue;
        }
        if (::connect(sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            break;
        }
        closeSocket(sock);
    }
    if (sock == INVALID_SOCK) {
        freeaddrinfo(result);
        return false;
    }

    const int nodelay = 1;
    (void)setsockopt(sock,
                     IPPROTO_TCP,
                     TCP_NODELAY,
                     reinterpret_cast<const char*>(&nodelay),
                     static_cast<int>(sizeof(nodelay)));

    freeaddrinfo(result);
    setNonBlocking(sock);
    return true;
}

void ModemClient::closeSocket(socket_t& sock) {
    if (sock != INVALID_SOCK) {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        sock = INVALID_SOCK;
    }
}

bool ModemClient::setNonBlocking(socket_t sock) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK) != -1;
#endif
}

bool ModemClient::connect(const std::string& host,
                          uint16_t cmd_port,
                          uint16_t data_port,
                          uint16_t voice_port) {
    if (isConnected()) {
        disconnect();
    }

    host_ = host;
    cmd_port_ = cmd_port;
    data_port_ = data_port;
    voice_port_ = voice_port;

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.state = ModemState::Connecting;
        status_.link_state = LinkState::Idle;
        status_.remote_callsign.clear();
        status_.busy_detected = false;
        status_.ptt_active = false;
        status_.tx_buffer_bytes = 0;
        status_.voice = VoiceStatus{};
    }

    clearResponseQueue();
    cmd_buffer_.clear();
    data_buffer_.clear();
    session_rx_.clear();
    {
        std::lock_guard<std::mutex> lock(data_tx_mutex_);
        data_tx_pending_.clear();
    }
    voice_buffer_.clear();
    {
        std::lock_guard<std::mutex> lock(voice_mutex_);
        voice_rx_samples_.clear();
    }
    voice_tx_sequence_ = 0;
    voice_tx_sample_counter_ = 0;

    // Connect command port
    if (!connectSocket(cmd_socket_, host, cmd_port)) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.state = ModemState::Error;
        return false;
    }

    // Connect data port
    if (!connectSocket(data_socket_, host, data_port)) {
        closeSocket(cmd_socket_);
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.state = ModemState::Error;
        return false;
    }

    // The voice port is optional: a modem built without ENABLE_VOICE never
    // binds it (main.cpp only warns), and the link emulator has no voice
    // media path at all. Losing it must not cost us the session.
    if (!connectSocket(voice_socket_, host, voice_port)) {
        std::cerr << "[ModemClient] voice port " << voice_port
                  << " unavailable; continuing without voice media\n";
        voice_socket_ = INVALID_SOCK;
    }

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.state = ModemState::Connected;
    }

    return true;
}

void ModemClient::disconnect() {
    closeSocket(cmd_socket_);
    closeSocket(data_socket_);
    closeSocket(voice_socket_);

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.state = ModemState::Disconnected;
        status_.link_state = LinkState::Idle;
        status_.remote_callsign.clear();
        status_.voice = VoiceStatus{};
    }

    cmd_buffer_.clear();
    data_buffer_.clear();
    session_rx_.clear();
    {
        std::lock_guard<std::mutex> lock(data_tx_mutex_);
        data_tx_pending_.clear();
    }
    voice_buffer_.clear();
    {
        std::lock_guard<std::mutex> lock(voice_mutex_);
        voice_rx_samples_.clear();
    }
    clearResponseQueue();

}

void ModemClient::clearResponseQueue() {
    std::lock_guard<std::mutex> lock(response_mutex_);
    std::queue<std::string> empty;
    response_queue_.swap(empty);
}

bool ModemClient::isConnected() const {
    return cmd_socket_ != INVALID_SOCK && data_socket_ != INVALID_SOCK;
}

bool ModemClient::hasVoiceMedia() const {
    return voice_socket_ != INVALID_SOCK;
}

void ModemClient::poll() {
    if (!isConnected()) return;

    flushPendingData();
    if (!isConnected()) return;
    readCommandSocket();
    if (!isConnected()) return;
    // Callbacks fired from readDataSocket() may re-enter poll() (e.g. via
    // getResponse()); a nested data read is skipped, and the bytes stay in
    // the socket for the next poll.
    if (!in_data_read_) {
        struct Guard {
            bool& flag;
            explicit Guard(bool& f) : flag(f) { flag = true; }
            ~Guard() { flag = false; }
        } guard(in_data_read_);
        readDataSocket();
    }
    if (isConnected() && voice_socket_ != INVALID_SOCK) readVoiceSocket();
}

void ModemClient::readCommandSocket() {
    char buffer[4096];

#ifdef _WIN32
    int n = recv(cmd_socket_, buffer, sizeof(buffer), 0);
    if (n == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            disconnect();
        }
        return;
    }
#else
    ssize_t n = recv(cmd_socket_, buffer, sizeof(buffer), 0);
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            disconnect();
        }
        return;
    }
#endif

    if (n == 0) {
        // Connection closed
        disconnect();
        return;
    }

    cmd_buffer_.append(buffer, n);

    // Process complete lines.
    // Modem responses are CR-terminated ("\r"), while some clients may emit LF/CRLF.
    size_t pos;
    while ((pos = cmd_buffer_.find_first_of("\r\n")) != std::string::npos) {
        std::string line = cmd_buffer_.substr(0, pos);
        cmd_buffer_.erase(0, pos + 1);

        // Collapse an optional second delimiter (CRLF or LFCR) into the same break.
        if (!cmd_buffer_.empty() &&
            ((cmd_buffer_[0] == '\r') || (cmd_buffer_[0] == '\n'))) {
            cmd_buffer_.erase(0, 1);
        }

        if (!line.empty()) {
            processCommandResponse(line);
        }
    }
}


// Data port carries two things: the session payload byte stream, and -
// only while no session is CONNECTED - 0xFB-tagged binary monitor records
// (INTERFACES.md forbids interleaving them precisely because payload bytes
// can contain 0xFB). We follow the same rule: connected means every byte is
// payload; otherwise a leading 0xFB with a complete header is a record.
void ModemClient::readDataSocket() {
    uint8_t buffer[4096];

#ifdef _WIN32
    int n = recv(data_socket_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
    if (n == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            std::cerr << "[ModemClient] Data socket error\n";
            disconnect();
        }
        return;
    }
#else
    ssize_t n = recv(data_socket_, buffer, sizeof(buffer), 0);
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            std::cerr << "[ModemClient] Data socket error\n";
            disconnect();
        }
        return;
    }
#endif

    if (n == 0) {
        // Data socket closed by remote
        std::cerr << "[ModemClient] Data socket closed by remote\n";
        disconnect();
        return;
    }

    data_buffer_.insert(data_buffer_.end(), buffer, buffer + n);

    bool connected_session;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        connected_session = (status_.link_state == LinkState::Connected);
    }

    // Everything is parsed into locals and the consumed bytes erased BEFORE
    // any callback runs: callbacks may re-enter poll()/disconnect() and must
    // never observe (or mutate) buffers we are still indexing into.
    std::vector<std::vector<uint8_t>> records;
    std::vector<std::vector<uint8_t>> messages;
    std::vector<uint8_t> payload;
    while (!data_buffer_.empty()) {
        if (connected_session || data_buffer_.front() != kMonitorRecordTag) {
            // Longest run that cannot start a record.
            size_t run = connected_session ? data_buffer_.size() : 1;
            if (!connected_session) {
                while (run < data_buffer_.size() &&
                       data_buffer_[run] != kMonitorRecordTag) {
                    ++run;
                }
            }
            payload.insert(payload.end(), data_buffer_.begin(),
                           data_buffer_.begin() + static_cast<std::ptrdiff_t>(run));
            data_buffer_.erase(data_buffer_.begin(),
                               data_buffer_.begin() + static_cast<std::ptrdiff_t>(run));
            continue;
        }

        if (data_buffer_.size() < kMonitorRecordHeaderLen) break;  // need more
        const size_t body_len = loadU16Be(data_buffer_.data() + 10);
        const size_t record_len = kMonitorRecordHeaderLen + body_len;
        if (data_buffer_.size() < record_len) break;  // need more

        std::vector<uint8_t> record(data_buffer_.begin(),
                                    data_buffer_.begin() + static_cast<std::ptrdiff_t>(record_len));
        data_buffer_.erase(data_buffer_.begin(),
                           data_buffer_.begin() + static_cast<std::ptrdiff_t>(record_len));
        records.push_back(std::move(record));
    }

    // A stalled partial record must not pin memory forever if the modem and
    // this client ever disagree about framing.
    if (data_buffer_.size() > 1u << 20) {
        std::cerr << "[ModemClient] Data buffer resync (dropping "
                  << data_buffer_.size() << " bytes)\n";
        data_buffer_.clear();
    }

    // Session bytes can arrive just before the CONNECTED event is processed,
    // so a payload that starts with a frame header is de-framed either way.
    const bool framed_start = payload.size() >= 2 && payload[0] == kSessionMagic0 &&
                              payload[1] == kSessionMagic1;
    if (!connected_session && session_rx_.empty() && !framed_start) {
        if (!payload.empty()) {
            messages.push_back(std::move(payload));
        }
    } else {
        session_rx_.insert(session_rx_.end(), payload.begin(), payload.end());
        size_t pos = 0;
        while (session_rx_.size() - pos >= kSessionHeaderLen) {
            if (session_rx_[pos] != kSessionMagic0 || session_rx_[pos + 1] != kSessionMagic1) {
                ++pos;  // resynchronize on the next header
                continue;
            }
            const size_t len = loadU16Be(session_rx_.data() + pos + 2);
            if (session_rx_.size() - pos < kSessionHeaderLen + len) break;  // need more
            std::vector<uint8_t> message(
                session_rx_.begin() + static_cast<std::ptrdiff_t>(pos + kSessionHeaderLen),
                session_rx_.begin() + static_cast<std::ptrdiff_t>(pos + kSessionHeaderLen + len));
            pos += kSessionHeaderLen + len;
            if (!message.empty()) {
                messages.push_back(std::move(message));
            }
        }
        session_rx_.erase(session_rx_.begin(), session_rx_.begin() + static_cast<std::ptrdiff_t>(pos));
    }

    for (const auto& record : records) {
        handleMonitorRecord(record);
    }
    if (data_callback_) {
        for (const auto& message : messages) {
            data_callback_(message);
        }
    }
}

void ModemClient::handleMonitorRecord(const std::vector<uint8_t>& record) {
    if (record.size() < kMonitorRecordHeaderLen) return;

    MonitorFrame frame;
    frame.frame_type_id = record[1];
    frame.frame_type = skywaveFrameTypeName(record[1]);
    frame.family = skywaveFamilyName(record[2]);
    frame.mode = record[3];

    // The record's body is the whole link frame; hashes identify the
    // stations but are not reversible, so callsigns come from the body.
    const std::vector<uint8_t> raw(record.begin() + kMonitorRecordHeaderLen,
                                   record.end());
    dispatchMonitorFrame(std::move(frame), raw);
}

void ModemClient::readVoiceSocket() {
    uint8_t buffer[4096];

#ifdef _WIN32
    int n = recv(voice_socket_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
    if (n == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            std::cerr << "[ModemClient] Voice socket error\n";
            closeSocket(voice_socket_);
        }
        return;
    }
#else
    ssize_t n = recv(voice_socket_, buffer, sizeof(buffer), 0);
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            std::cerr << "[ModemClient] Voice socket error\n";
            closeSocket(voice_socket_);
        }
        return;
    }
#endif

    if (n == 0) {
        // Only the media path is gone; the session continues.
        std::cerr << "[ModemClient] Voice socket closed by remote\n";
        closeSocket(voice_socket_);
        voice_buffer_.clear();
        return;
    }

    voice_buffer_.insert(voice_buffer_.end(), buffer, buffer + n);

    while (voice_buffer_.size() >= kVoiceMediaHeaderLen) {
        const uint8_t version = voice_buffer_[0];
        const uint8_t type = voice_buffer_[1];
        const uint16_t payload_len = loadU16Be(voice_buffer_.data() + 2);
        const size_t frame_len = kVoiceMediaHeaderLen + static_cast<size_t>(payload_len);

        if (version != kVoiceMediaVersion || frame_len > (1024u * 1024u)) {
            voice_buffer_.clear();
            break;
        }
        if (voice_buffer_.size() < frame_len) {
            break;
        }

        if (type == kVoiceMediaDownlinkPcm && (payload_len % 2u) == 0u) {
            const uint8_t* payload = voice_buffer_.data() + kVoiceMediaHeaderLen;
            const size_t sample_count = static_cast<size_t>(payload_len / 2u);
            std::lock_guard<std::mutex> lock(voice_mutex_);
            for (size_t i = 0; i < sample_count; ++i) {
                const int16_t sample = static_cast<int16_t>(
                    static_cast<uint16_t>(payload[i * 2]) |
                    (static_cast<uint16_t>(payload[i * 2 + 1]) << 8));
                voice_rx_samples_.push_back(sample);
            }
        }

        voice_buffer_.erase(voice_buffer_.begin(), voice_buffer_.begin() + static_cast<std::ptrdiff_t>(frame_len));
    }
}

// --- notification and response dispatch ------------------------------------

void ModemClient::applyStatusLine(const std::string& fields) {
    ModemStatus snapshot;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        snapshot = status_;
    }

    float snr_before = snapshot.snr_db;
    std::string state_name;

    for (const auto& [key, value] : splitFields(fields)) {
        if (key == "STATE") {
            state_name = upperCopy(value);
            snapshot.link_state = mapStateToLinkState(state_name);
        } else if (key == "CALL") {
            snapshot.my_callsign = value;
        } else if (key == "REMOTE") {
            snapshot.remote_callsign = (value == "-") ? std::string() : value;
        } else if (key == "FAMILY") {
            snapshot.family = upperCopy(value);
        } else if (key == "MODE") {
            snapshot.mode = upperCopy(value);
            snapshot.mode_index = std::max(0, parseModeIndex(snapshot.mode));
            snapshot.mode_pinned = !startsWith(snapshot.mode, "AUTO");
        } else if (key == "NET") {
            snapshot.net_bps = parseFloatOr(value, 0.0f);
        } else if (key == "SNR") {
            snapshot.snr_db = parseFloatOr(value, snapshot.snr_db);
        } else if (key == "RSNR") {
            snapshot.remote_snr_valid = (value != "-");
            snapshot.remote_snr_db = snapshot.remote_snr_valid
                                      ? parseFloatOr(value, 0.0f)
                                      : 0.0f;
        } else if (key == "CFO") {
            snapshot.tx_align_hz = parseFloatOr(value, 0.0f);
        } else if (key == "RETRY") {
            snapshot.retry_ratio = parseFloatOr(value, 0.0f);
        } else if (key == "BUSY") {
            snapshot.busy_detected = parseBoolToken(value);
        } else if (key == "LISTEN") {
            snapshot.listening = parseBoolToken(value);
        } else if (key == "COMPRESS") {
            snapshot.compression = parseBoolToken(value);
        } else if (key == "CRYPTO") {
            snapshot.encryption = parseBoolToken(value);
        } else if (key == "EPOCH") {
            snapshot.crypto_epoch = static_cast<int>(parseIntOr(value, 0));
        } else if (key == "RESUME") {
            snapshot.resume = parseBoolToken(value);
        } else if (key == "MASK") {
            snapshot.masked_carriers = static_cast<int>(parseIntOr(value, 0));
        } else if (key == "TXBUF") {
            snapshot.tx_buffer_bytes = static_cast<size_t>(parseIntOr(value, 0));
        } else if (key == "FOK") {
            snapshot.frames_ok = static_cast<uint64_t>(parseIntOr(value, 0));
        } else if (key == "FFAIL") {
            snapshot.frames_failed = static_cast<uint64_t>(parseIntOr(value, 0));
        }
    }

    if (snapshot.link_state != LinkState::Connected) {
        snapshot.remote_callsign.clear();
    }

    LinkState previous;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        previous = status_.link_state;
        // Preserve the transport-level connection state; STATUS only
        // describes the radio session.
        snapshot.state = status_.state;
        snapshot.voice = status_.voice;
        snapshot.ptt_active = status_.ptt_active;
        status_ = snapshot;
    }
    onLinkStateChanged(previous, snapshot.link_state);

    // The modem emits no periodic SNR notification, so STATUS polling is
    // where link quality changes surface.
    if (snr_callback_ && snapshot.snr_db != snr_before) {
        snr_callback_(snapshot.snr_db);
    }
}

void ModemClient::dispatchMonitorFrame(MonitorFrame frame,
                                       const std::vector<uint8_t>& raw) {
    auto parsed = parseSkywaveFrame(raw, /*allow_truncated=*/true);
    if (!parsed) return;

    frame.truncated = parsed->truncated;
    frame.encrypted = (parsed->flags & kSkywaveFlagEncrypted) != 0;
    frame.payload = parsed->body;
    if (frame.frame_type.empty()) {
        frame.frame_type_id = parsed->type;
        frame.frame_type = parsed->type_name;
    }

    // Callsigns: `MON:` lines carry the modem's cached context, but the
    // bodies of CQ/BEACON name their sender outright, which is both more
    // reliable and the only source the binary records have.
    switch (static_cast<SkywaveFrameType>(parsed->type)) {
        case SkywaveFrameType::Cq:
            if (auto cq = parseSkywaveCq(parsed->body)) {
                frame.src_callsign = cq->callsign;
                frame.dst_callsign = "*";
            }
            break;
        case SkywaveFrameType::Beacon:
            if (auto b = parseSkywaveBeacon(parsed->body)) {
                frame.src_callsign = b->callsign;
                frame.dst_callsign = "*";
                if (frame.family.empty() || frame.family == "?") {
                    frame.family = skywaveFamilyName(b->family);
                }
            }
            break;
        default:
            break;
    }

    // Complete broadcasts are delivered once, by EVT:BROADCAST (it names the
    // sender and keeps flowing while connected); reassembling the same chain
    // here as well showed every broadcast two or three times.
    if (monitor_callback_) {
        monitor_callback_(frame);
    }
}

// `MON:<src>:<dst|*>:<frame_type>:<family>:<Mx>:<hex_payload>`
// The hex payload is the raw link frame, capped at 64 bytes by the modem.
void ModemClient::handleMonitorLine(const std::string& body) {
    auto line = parseSkywaveMonitorLine(body);
    if (!line) return;

    MonitorFrame frame;
    frame.src_callsign = line->src;
    frame.dst_callsign = line->dst;
    frame.frame_type_id = line->frame_type;
    frame.frame_type =
        skywaveFrameTypeName(static_cast<uint8_t>(line->frame_type));
    frame.family = upperCopy(line->family);
    frame.mode = line->mode;

    if (line->raw_frame.empty()) {
        // Encrypted or undecodable payloads surface as metadata only.
        if (monitor_callback_) monitor_callback_(frame);
        return;
    }

    dispatchMonitorFrame(std::move(frame), line->raw_frame);
}

// `EVT:<NAME> [args]` (INTERFACES.md "Notifications").
void ModemClient::handleEventLine(const std::string& body) {
    const size_t space = body.find(' ');
    const std::string name = upperCopy(body.substr(0, space));
    const std::string args =
        space == std::string::npos ? std::string() : trimCopy(body.substr(space + 1));

    if (name == "STATE") {
        const std::string state = upperCopy(args);
        LinkState previous;
        LinkState next;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            previous = status_.link_state;
            status_.link_state = mapStateToLinkState(state);
            next = status_.link_state;
            status_.listening = (state == "LISTENING");
            if (status_.link_state == LinkState::Idle) {
                status_.remote_callsign.clear();
            }
        }
        onLinkStateChanged(previous, next);
        if (state_callback_) state_callback_(state);
    } else if (name == "CONNECTED") {
        // EVT:CONNECTED <remote_call> <family> <initial_mode>
        std::istringstream tokens(args);
        std::string call, family, mode;
        tokens >> call >> family >> mode;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.link_state = LinkState::Connected;
            status_.remote_callsign = call;
            if (!family.empty()) status_.family = upperCopy(family);
            if (!mode.empty()) {
                status_.mode = upperCopy(mode);
                status_.mode_index = std::max(0, parseModeIndex(mode));
            }
        }
        if (connected_callback_) connected_callback_(call);
    } else if (name == "DISCONNECTED") {
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.link_state = LinkState::Idle;
            status_.remote_callsign.clear();
        }
        resetSessionStream();
        if (disconnected_callback_) disconnected_callback_();
    } else if (name == "MODE") {
        // EVT:MODE <Mx> <net_bps>
        std::istringstream tokens(args);
        std::string mode;
        double net = 0.0;
        tokens >> mode >> net;
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (!mode.empty()) {
            status_.mode = upperCopy(mode);
            status_.mode_index = std::max(0, parseModeIndex(mode));
        }
        status_.net_bps = net;
    } else if (name == "SNR") {
        // Documented but not currently emitted by the modem; handled so a
        // future firmware that does emit it needs no client change.
        std::istringstream tokens(args);
        std::string local;
        tokens >> local;
        const float snr = parseFloatOr(local, 0.0f);
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.snr_db = snr;
        }
        if (snr_callback_) snr_callback_(snr);
    } else if (name == "BUSY") {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.busy_detected = parseBoolToken(args);
    } else if (name == "BROADCAST") {
        // `EVT:BROADCAST <src> <hex>`: a complete BCAST_DATA chain the
        // modem's monitor already reassembled. Unlike the data port's
        // binary records this keeps flowing during a session, so it is
        // the preferred source; the client's own reassembler covers
        // modems that predate the event.
        const size_t space = args.find(' ');
        const std::string src =
            space == std::string::npos ? args : args.substr(0, space);
        std::vector<uint8_t> payload;
        if (space != std::string::npos &&
            parseHexBytes(trimCopy(args.substr(space + 1)), &payload) &&
            !payload.empty() && broadcast_callback_) {
            broadcast_callback_(src == "?" ? std::string() : src, payload);
        }
    } else if (name == "TX_WATCHDOG") {
        // Always a modem-side fault; surface it rather than swallowing it.
        std::cerr << "[ModemClient] modem TX watchdog fired after " << args
                  << " s of continuous keying\n";
    } else if (name == "CFO_WARN") {
        std::cerr << "[ModemClient] TX alignment at " << args
                  << " Hz - the dial needs retuning\n";
    } else if (name == "VOICE") {
        VoiceStatus snapshot;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            snapshot = status_.voice;
        }
        snapshot.last_event = upperCopy(args);
        if (snapshot.last_event == "OPEN") {
            snapshot.open = true;
            snapshot.ready = true;
            snapshot.state = VoiceState::Ready;
        } else if (snapshot.last_event == "CLOSED") {
            snapshot = VoiceStatus{};
            snapshot.last_event = "CLOSED";
        } else if (snapshot.last_event == "TALKING") {
            snapshot.state = VoiceState::TxActive;
            snapshot.active = true;
        } else if (snapshot.last_event == "STOPPED") {
            snapshot.state = snapshot.open ? VoiceState::Ready : VoiceState::Idle;
            snapshot.active = false;
        }
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.voice = snapshot;
        }
        if (voice_callback_) voice_callback_(snapshot);
    }

    if (event_callback_) event_callback_(name, args);
}

void ModemClient::processCommandResponse(const std::string& line) {
    if (startsWith(line, "EVT:")) {
        handleEventLine(line.substr(4));
        return;
    }
    if (startsWith(line, "MON:")) {
        handleMonitorLine(line.substr(4));
        return;
    }

    // `VOICESTATUS ...` may arrive as a command response; fold it into the
    // status snapshot before it reaches the response queue so a caller that
    // ignores the text still sees the update.
    if (startsWith(line, "VOICESTATUS ")) {
        VoiceStatus snapshot;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            snapshot = status_.voice;
        }
        // Optional fields: absent means off / not applicable.
        snapshot.broadcast_rx = false;
        snapshot.real_time = true;
        snapshot.unit_sec = 0.0;
        for (const auto& [key, value] : splitFields(line.substr(12))) {
            if (key == "MODE") {
                snapshot.mode = parseVoiceMode(value);
                snapshot.broadcast = (snapshot.mode == VoiceMode::Broadcast);
            } else if (key == "CODEC") {
                snapshot.codec = upperCopy(value);
            } else if (key == "STATE") {
                snapshot.state = parseVoiceState(value);
                snapshot.open = (snapshot.state != VoiceState::Idle);
                snapshot.ready = (snapshot.state == VoiceState::Ready);
                snapshot.active = (snapshot.state == VoiceState::TxActive ||
                                   snapshot.state == VoiceState::RxActive);
                snapshot.local = (snapshot.state == VoiceState::TxActive ||
                                  snapshot.state == VoiceState::TxRequested);
            } else if (key == "PTT") {
                snapshot.ptt = parseBoolToken(value);
            } else if (key == "GRANTED") {
                snapshot.granted = parseBoolToken(value);
            } else if (key == "TXQ") {
                snapshot.tx_queued = static_cast<int>(parseIntOr(value, 0));
            } else if (key == "RXQ") {
                snapshot.rx_queued = static_cast<int>(parseIntOr(value, 0));
            } else if (key == "TRANSPORT") {
                snapshot.transport_mode = parseModeIndex(value);
            } else if (key == "BCASTRX") {
                snapshot.broadcast_rx = parseBoolToken(value);
            } else if (key == "RT") {
                snapshot.real_time = parseBoolToken(value);
            } else if (key == "UNIT") {
                snapshot.unit_sec = std::atof(value.c_str());
            }
        }
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.voice = snapshot;
        }
        if (voice_callback_) voice_callback_(snapshot);
    }

    if (startsWith(line, "STATUS ")) {
        applyStatusLine(line.substr(7));
    } else if (startsWith(line, "STATE ")) {
        const std::string state = upperCopy(trimCopy(line.substr(6)));
        LinkState previous;
        LinkState next;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            previous = status_.link_state;
            status_.link_state = mapStateToLinkState(state);
            next = status_.link_state;
            status_.listening = (state == "LISTENING");
            if (status_.link_state == LinkState::Idle) {
                status_.remote_callsign.clear();
            }
        }
        onLinkStateChanged(previous, next);
        if (state_callback_) state_callback_(state);
    }

    std::lock_guard<std::mutex> lock(response_mutex_);
    response_queue_.push(line);
    // Fire-and-forget commands (sendCommand without getResponse) leave
    // their answers here; bound the queue so it cannot grow forever.
    while (response_queue_.size() > kMaxQueuedResponses) {
        response_queue_.pop();
    }
}

// A radio session ended: its partial de-framing state must not leak into
// the next session (or swallow unconnected payloads), and bytes still
// waiting to be written for it are no longer deliverable.
void ModemClient::resetSessionStream() {
    session_rx_.clear();
    std::lock_guard<std::mutex> lock(data_tx_mutex_);
    data_tx_pending_.clear();
}

void ModemClient::onLinkStateChanged(LinkState previous, LinkState next) {
    if (previous == next) return;
    if ((previous == LinkState::Connected || previous == LinkState::Disconnecting) &&
        next != LinkState::Connected && next != LinkState::Disconnecting) {
        resetSessionStream();
    }
}

bool ModemClient::sendCommand(const std::string& cmd) {
    if (!isConnected()) return false;

    std::string full_cmd = cmd + "\n";

#ifdef _WIN32
    int sent = send(cmd_socket_, full_cmd.c_str(), static_cast<int>(full_cmd.size()), 0);
    return sent == static_cast<int>(full_cmd.size());
#else
    ssize_t sent = send(cmd_socket_, full_cmd.c_str(), full_cmd.size(), 0);
    return sent == static_cast<ssize_t>(full_cmd.size());
#endif
}

// Skywave answers a command by echoing its verb (`LISTEN ON`, `CONNECT W1AW`)
// or with `ERROR:<reason>`. There is no `OK`.
bool ModemClient::sendCommandOk(const std::string& cmd, int timeout_ms) {
    const auto response = sendCommandForResponse(cmd, timeout_ms);
    if (!response) return false;
    if (startsWith(*response, "ERROR:")) {
        std::cerr << "[ModemClient] '" << cmd << "' -> " << *response << "\n";
        return false;
    }
    return true;
}

// Responses carry no id, so correlation is by position plus verb: stale
// answers (to fire-and-forget commands or an earlier command that timed out)
// are dropped before sending, and any line that arrives while waiting is
// accepted only if it echoes this command's verb or is an ERROR:.
std::optional<std::string> ModemClient::sendCommandForResponse(const std::string& cmd,
                                                               int timeout_ms) {
    const std::string trimmed = trimCopy(cmd);
    const std::string verb = upperCopy(trimmed.substr(0, trimmed.find(' ')));

    clearResponseQueue();
    if (!sendCommand(cmd)) {
        return std::nullopt;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const int remaining = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (remaining <= 0) {
            return std::nullopt;
        }
        auto response = getResponse(remaining);
        if (!response) {
            return std::nullopt;
        }
        if (responseMatchesVerb(*response, verb)) {
            return response;
        }
        // Late answer to some other command: discard and keep waiting.
    }
}

bool ModemClient::responseMatchesVerb(const std::string& response, const std::string& verb) {
    if (startsWith(response, "ERROR:")) return true;
    const std::string first = upperCopy(response.substr(0, response.find(' ')));
    return first == verb;
}

std::optional<std::string> ModemClient::getResponse(int timeout_ms) {
    auto start = std::chrono::steady_clock::now();

    while (true) {
        {
            std::lock_guard<std::mutex> lock(response_mutex_);
            if (!response_queue_.empty()) {
                std::string response = response_queue_.front();
                response_queue_.pop();
                return response;
            }
        }

        auto elapsed = std::chrono::steady_clock::now() - start;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >= timeout_ms) {
            return std::nullopt;
        }

        poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// --- high-level commands ---------------------------------------------------

bool ModemClient::setCallsign(const std::string& callsign) {
    return sendCommandOk("MYCALL " + upperCopy(trimCopy(callsign)));
}

bool ModemClient::connectToStation(const std::string& callsign) {
    return sendCommandOk("CONNECT " + upperCopy(trimCopy(callsign)));
}

bool ModemClient::disconnectStation() {
    return sendCommandOk("DISCONNECT");
}

bool ModemClient::abort() {
    return sendCommandOk("ABORT");
}

bool ModemClient::setCompression(bool enabled) {
    // Skywave negotiates zstd through the capability bitmap at connect time
    // (PROTOCOL.md); there is no runtime toggle on the control port. The
    // negotiated result is reported by STATUS as COMPRESS.
    static bool warned = false;
    if (!warned) {
        std::cerr << "[ModemClient] compression is negotiated per session by "
                     "the modem; STATUS reports the result\n";
        warned = true;
    }
    (void)enabled;
    return true;
}

bool ModemClient::setListenMode(bool enabled) {
    return sendCommandOk(std::string("LISTEN ") + (enabled ? "ON" : "OFF"));
}

bool ModemClient::setMonitorMode(bool enabled) {
    return sendCommandOk(std::string("MONITOR ") + (enabled ? "ON" : "OFF"));
}

bool ModemClient::sendBeacon() {
    return sendCommandOk("BEACON");
}

// Application-level broadcast: Skywave's BCAST carries arbitrary bytes as a
// fragment chain (<= 16 fragments x 256 B). BEACON itself is telemetry-only.
bool ModemClient::sendBroadcastData(const std::vector<uint8_t>& data) {
    if (data.empty() || data.size() > 16u * 256u) {
        return false;
    }
    return sendCommandOk("BCAST " + bytesToHex(data), 1000);
}

bool ModemClient::sendCq() {
    return sendCommandOk("CQ");
}

bool ModemClient::setCwId(bool enabled) {
    return sendCommandOk(std::string("CWID ") + (enabled ? "ON" : "OFF"));
}

// --- waveform / ladder control ---------------------------------------------

bool ModemClient::setFamily(const std::string& family) {
    const std::string upper = upperCopy(trimCopy(family));
    if (upper != "WIDE" && upper != "MID" && upper != "NARROW" &&
        upper != "ULTRA") {
        return false;
    }
    // Accepted only while IDLE/LISTENING; the modem answers ERROR:STATE
    // during a session.
    return sendCommandOk("FAMILY " + upper);
}

bool ModemClient::setMode(const std::string& mode) {
    return sendCommandOk("MODE " + upperCopy(trimCopy(mode)));
}

bool ModemClient::setMaxMode(const std::string& mode) {
    return sendCommandOk("MAXMODE " + upperCopy(trimCopy(mode)));
}

bool ModemClient::setCpMode(const std::string& cp) {
    return sendCommandOk("CPMODE " + upperCopy(trimCopy(cp)));
}

bool ModemClient::setHarq(const std::string& harq) {
    return sendCommandOk("HARQ " + upperCopy(trimCopy(harq)));
}

bool ModemClient::setAutoMode(bool enabled) {
    if (enabled) {
        return setMode("AUTO");
    }
    // Pinning without a target means "hold what the ladder reached".
    ModemStatus snapshot = getStatus();
    const int index = snapshot.mode_index > 0 ? snapshot.mode_index : 4;
    return setMode("M" + std::to_string(index));
}

bool ModemClient::sendData(const std::vector<uint8_t>& message) {
    if (!isConnected()) return false;
    if (message.empty()) return true;

    bool connected_session;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        connected_session = (status_.link_state == LinkState::Connected);
    }
    std::vector<uint8_t> framed;
    if (connected_session) {
        if (message.size() > kSessionMaxMessage) return false;
        framed.reserve(kSessionHeaderLen + message.size());
        framed.push_back(kSessionMagic0);
        framed.push_back(kSessionMagic1);
        framed.push_back(static_cast<uint8_t>(message.size() >> 8));
        framed.push_back(static_cast<uint8_t>(message.size() & 0xFFu));
        framed.insert(framed.end(), message.begin(), message.end());
    }
    const std::vector<uint8_t>& data = connected_session ? framed : message;

    // A message is either written whole or queued whole: whatever the
    // socket does not take now stays in data_tx_pending_ and is flushed by
    // poll(), so a frame is never cut short on the wire (which would desync
    // the peer's de-framer).
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(data_tx_mutex_);
        if (data_tx_pending_.size() + data.size() > kMaxPendingDataBytes) {
            std::cerr << "[ModemClient] data send backlog full ("
                      << data_tx_pending_.size() << " bytes); dropping message\n";
            return false;
        }
        data_tx_pending_.insert(data_tx_pending_.end(), data.begin(), data.end());
        ok = flushPendingDataLocked();
    }
    if (!ok) {
        disconnect();
        return false;
    }

    {
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        last_data_send_ms_.store(static_cast<int64_t>(now_ms), std::memory_order_relaxed);
    }

    return ok;
}

void ModemClient::flushPendingData() {
    bool ok;
    {
        std::lock_guard<std::mutex> lock(data_tx_mutex_);
        ok = flushPendingDataLocked();
    }
    if (!ok) disconnect();
}

size_t ModemClient::pendingDataBytes() const {
    std::lock_guard<std::mutex> lock(data_tx_mutex_);
    return data_tx_pending_.size();
}

// Writes as much of data_tx_pending_ as the socket accepts without blocking.
// Returns false only on a hard socket error.
bool ModemClient::flushPendingDataLocked() {
    size_t total_sent = 0;
    bool ok = true;
    while (total_sent < data_tx_pending_.size()) {
#ifdef _WIN32
        int sent = send(data_socket_,
                        reinterpret_cast<const char*>(data_tx_pending_.data() + total_sent),
                        static_cast<int>(data_tx_pending_.size() - total_sent), 0);
        if (sent > 0) {
            total_sent += static_cast<size_t>(sent);
            continue;
        }
        if (sent < 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
            break;  // socket full: keep the rest for the next poll()
        }
        ok = false;
        break;
#else
        ssize_t sent = send(data_socket_, data_tx_pending_.data() + total_sent,
                            data_tx_pending_.size() - total_sent, 0);
        if (sent > 0) {
            total_sent += static_cast<size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;  // socket full: keep the rest for the next poll()
        }
        ok = false;
        break;
#endif
    }
    data_tx_pending_.erase(data_tx_pending_.begin(),
                           data_tx_pending_.begin() + static_cast<std::ptrdiff_t>(total_sent));
    return ok;
}

bool ModemClient::sendText(const std::string& text) {
    std::vector<uint8_t> data(text.begin(), text.end());
    return sendData(data);
}

std::optional<std::vector<uint8_t>> ModemClient::receiveData() {
    // Data is delivered via callback, not polling
    return std::nullopt;
}

int64_t ModemClient::millisSinceLastDataSend() const {
    int64_t last_ms = last_data_send_ms_.load(std::memory_order_relaxed);
    if (last_ms <= 0) {
        return std::numeric_limits<int64_t>::max();
    }
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return static_cast<int64_t>(now_ms) - last_ms;
}

// --- query commands --------------------------------------------------------

std::string ModemClient::getVersion() {
    auto response = sendCommandForResponse("VERSION", 500);
    if (!response) {
        return "";
    }
    if (startsWith(*response, "VERSION ")) {
        return trimCopy(std::string_view(*response).substr(8));
    }
    if (startsWith(*response, "ERROR:")) {
        return "";
    }
    return *response;
}

size_t ModemClient::getBufferSize() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_.tx_buffer_bytes;
}

bool ModemClient::refreshStatus(int timeout_ms) {
    const auto response = sendCommandForResponse("STATUS", timeout_ms);
    // processCommandResponse() already folded the fields into status_; this
    // only reports whether the exchange happened.
    return response && startsWith(*response, "STATUS ");
}

std::optional<NoiseFloorReading> ModemClient::getNoiseFloor() {
    const auto response = sendCommandForResponse("NOISEFLOOR", 500);
    if (!response) {
        return std::nullopt;
    }
    return parseNoiseFloorResponse(*response);
}

// `AUDIOLEVEL RX=<dbfs> TX=<dbfs>`
std::optional<AudioLevels> ModemClient::getAudioLevels() {
    const auto response = sendCommandForResponse("AUDIOLEVEL", 500);
    if (!response || !startsWith(*response, "AUDIOLEVEL ")) {
        return std::nullopt;
    }
    AudioLevels levels;
    for (const auto& [key, value] : splitFields(response->substr(11))) {
        if (key == "RX") levels.rx_dbfs = parseFloatOr(value, -120.0f);
        else if (key == "TX") levels.tx_dbfs = parseFloatOr(value, -120.0f);
    }
    return levels;
}

// `FREQINFO CENTER=<hz> SHIFT=<hz> CFO=<hz>`
std::optional<FrequencyInfo> ModemClient::getFrequencyInfo() {
    const auto response = sendCommandForResponse("FREQINFO", 500);
    if (!response || !startsWith(*response, "FREQINFO ")) {
        return std::nullopt;
    }
    FrequencyInfo info;
    for (const auto& [key, value] : splitFields(response->substr(9))) {
        if (key == "CENTER") info.center_hz = parseFloatOr(value, 0.0f);
        else if (key == "SHIFT") info.shift_hz = parseFloatOr(value, 0.0f);
        else if (key == "CFO") info.cfo_hz = parseFloatOr(value, 0.0f);
    }
    return info;
}

bool ModemClient::reportDialFrequency(double hz) {
    if (hz < 0.0) return false;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "FREQINFO %.0f", hz);
    return sendCommandOk(buf);
}

// --- voice -----------------------------------------------------------------

bool ModemClient::setVoiceCodec(const std::string& codec) {
    return sendCommandOk("VOICECODEC " + upperCopy(trimCopy(codec)));
}

bool ModemClient::setVoiceMode(VoiceMode mode) {
    return sendCommandOk("VOICE " + voiceModeCommand(mode));
}

bool ModemClient::voiceStart() {
    return sendCommandOk("VOICESTART");
}

bool ModemClient::voiceStop() {
    return sendCommandOk("VOICESTOP");
}

bool ModemClient::setVoicePtt(bool enabled) {
    return sendCommandOk(std::string("VOICEPTT ") + (enabled ? "ON" : "OFF"));
}

std::optional<VoiceStatus> ModemClient::queryVoiceStatus() {
    const auto response = sendCommandForResponse("VOICESTATUS", 500);
    if (!response || startsWith(*response, "ERROR:")) {
        return std::nullopt;
    }
    return getVoiceStatus();
}

bool ModemClient::sendVoicePcm(const int16_t* samples, size_t sample_count) {
    if (!isConnected() || voice_socket_ == INVALID_SOCK || !samples || sample_count == 0) {
        return false;
    }

    const uint16_t payload_len =
        static_cast<uint16_t>(std::min(sample_count * sizeof(int16_t),
                                       static_cast<size_t>(0xFFFF)));
    std::vector<uint8_t> frame(kVoiceMediaHeaderLen + payload_len);
    frame[0] = kVoiceMediaVersion;
    frame[1] = kVoiceMediaUplinkPcm;
    storeU16Be(frame.data() + 2, payload_len);
    storeU32Be(frame.data() + 4, voice_tx_sequence_++);
    storeU64Be(frame.data() + 8, voice_tx_sample_counter_);
    std::memcpy(frame.data() + kVoiceMediaHeaderLen, samples, payload_len);
    voice_tx_sample_counter_ += static_cast<uint64_t>(payload_len / sizeof(int16_t));

    size_t total_sent = 0;
    int would_block_retries = 0;
    constexpr int kMaxWouldBlockRetries = 200;
    while (total_sent < frame.size()) {
#ifdef _WIN32
        int sent = send(voice_socket_,
                        reinterpret_cast<const char*>(frame.data() + total_sent),
                        static_cast<int>(frame.size() - total_sent),
                        0);
        if (sent > 0) {
            total_sent += static_cast<size_t>(sent);
            would_block_retries = 0;
            continue;
        }
        const int err = WSAGetLastError();
        if (sent < 0 && err == WSAEWOULDBLOCK) {
            if (++would_block_retries > kMaxWouldBlockRetries) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        break;
#else
        ssize_t sent = send(voice_socket_, frame.data() + total_sent, frame.size() - total_sent, 0);
        if (sent > 0) {
            total_sent += static_cast<size_t>(sent);
            would_block_retries = 0;
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (++would_block_retries > kMaxWouldBlockRetries) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        break;
#endif
    }

    return total_sent == frame.size();
}

std::vector<int16_t> ModemClient::receiveVoicePcm(size_t max_samples) {
    std::vector<int16_t> out;
    std::lock_guard<std::mutex> lock(voice_mutex_);
    const size_t count = std::min(max_samples, voice_rx_samples_.size());
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        out.push_back(voice_rx_samples_.front());
        voice_rx_samples_.pop_front();
    }
    return out;
}

} // namespace skylink
