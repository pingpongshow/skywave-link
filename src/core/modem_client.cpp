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
    #include <poll.h>
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

// STATUS `VOICE=off|open|talking|bcastrx` folded into the voice snapshot.
void applyVoiceSummary(const std::string& summary, VoiceStatus* v) {
    auto closeVoice = [v]() {
        v->state = VoiceState::Idle;
        v->open = v->ready = v->active = v->local = false;
        v->ptt = v->granted = false;
    };
    if (summary == "off") {
        closeVoice();
        v->broadcast_rx = false;
        v->broadcast_tag.clear();
    } else if (summary == "bcastrx") {
        closeVoice();
        v->broadcast_rx = true;
    } else if (summary == "open") {
        if (!v->open || v->state == VoiceState::TxActive || v->state == VoiceState::Idle) {
            if (v->state == VoiceState::Idle || v->state == VoiceState::TxActive) {
                v->state = VoiceState::Ready;
            }
            v->open = true;
            v->ready = (v->state == VoiceState::Ready);
            v->active = (v->state == VoiceState::RxActive);
            v->local = false;
        }
    } else if (summary == "talking") {
        v->state = VoiceState::TxActive;
        v->open = v->active = v->local = true;
        v->ready = false;
    }
}

} // namespace

std::string normalizeVoiceCodec(const std::string& codec) {
    const std::string upper = upperCopy(trimCopy(codec));
    if (upper == "AUTO" || upper == "700C" || upper == "1300" || upper == "2400" ||
        upper == "3200") {
        return upper;
    }
    if (upper == "700") return "700C";
    return {};
}


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
    // Owners' callbacks must not run while the owner is being torn down.
    async_commands_.clear();
    disconnect();
}

// Connects with a deadline: the socket is made non-blocking first, so an
// unreachable host costs at most `timeout_ms` per address instead of the
// OS connect timeout (over a minute).

bool ModemClient::connectSocket(socket_t& sock, const std::string& host, uint16_t port,
                                int timeout_ms) {
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
        setNonBlocking(sock);
        bool connected = false;
#ifdef _WIN32
        if (::connect(sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            connected = true;
        } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(sock, &wfds);
            timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            if (select(0, nullptr, &wfds, nullptr, &tv) == 1) {
                int err = 0;
                int len = sizeof(err);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
                connected = (err == 0);
            }
        }
#else
        if (::connect(sock, ai->ai_addr, ai->ai_addrlen) == 0) {
            connected = true;
        } else if (errno == EINPROGRESS) {
            pollfd pfd{sock, POLLOUT, 0};
            int rc;
            do {
                rc = ::poll(&pfd, 1, timeout_ms);
            } while (rc < 0 && errno == EINTR);
            if (rc == 1) {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
                connected = (err == 0);
            }
        }
#endif
        if (connected) {
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

    cancelConnect();
    host_ = host;
    cmd_port_ = cmd_port;
    data_port_ = data_port;
    voice_port_ = voice_port;
    resetSessionState();

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

// Shared by the helper thread and the client: the client may be destroyed
// (or the connect cancelled) while the helper still waits on the network.
struct ModemClient::ConnectJob {
    std::mutex mu;
    bool done = false;
    bool cancelled = false;
    bool ok = false;
    socket_t cmd = INVALID_SOCK;
    socket_t data = INVALID_SOCK;
    socket_t voice = INVALID_SOCK;
};

// Background reopen of the voice socket: the TCP connect runs on a helper
// thread (an unreachable host must never stall poll()).
struct ModemClient::VoiceJob {
    std::mutex mu;
    bool done = false;
    bool cancelled = false;
    socket_t sock = INVALID_SOCK;
};

bool ModemClient::beginConnect(const std::string& host,
                               uint16_t cmd_port,
                               uint16_t data_port,
                               uint16_t voice_port) {
    if (isConnected()) {
        disconnect();
    }
    cancelConnect();
    host_ = host;
    cmd_port_ = cmd_port;
    data_port_ = data_port;
    voice_port_ = voice_port;
    resetSessionState();

    auto job = std::make_shared<ConnectJob>();
    connect_job_ = job;
    try {
        std::thread([job, host, cmd_port, data_port, voice_port]() {
            socket_t cmd = INVALID_SOCK;
            socket_t data = INVALID_SOCK;
            socket_t voice = INVALID_SOCK;
            bool ok = connectSocket(cmd, host, cmd_port);
            if (ok) {
                ok = connectSocket(data, host, data_port);
                if (!ok) closeSocket(cmd);
            }
            if (ok && !connectSocket(voice, host, voice_port)) {
                voice = INVALID_SOCK;
            }
            std::lock_guard<std::mutex> lock(job->mu);
            if (job->cancelled) {
                closeSocket(cmd);
                closeSocket(data);
                closeSocket(voice);
            } else {
                job->ok = ok;
                job->cmd = cmd;
                job->data = data;
                job->voice = voice;
            }
            job->done = true;
        }).detach();
    } catch (const std::exception&) {
        connect_job_.reset();
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.state = ModemState::Error;
        return false;
    }
    return true;
}

ConnectProgress ModemClient::pollConnect() {
    if (!connect_job_) return ConnectProgress::None;
    std::shared_ptr<ConnectJob> job = connect_job_;
    std::lock_guard<std::mutex> lock(job->mu);
    if (!job->done) return ConnectProgress::InProgress;
    connect_job_.reset();
    if (!job->ok) {
        std::lock_guard<std::mutex> status_lock(status_mutex_);
        status_.state = ModemState::Error;
        return ConnectProgress::Failed;
    }
    cmd_socket_ = job->cmd;
    data_socket_ = job->data;
    voice_socket_ = job->voice;
    job->cmd = job->data = job->voice = INVALID_SOCK;
    if (voice_socket_ == INVALID_SOCK) {
        std::cerr << "[ModemClient] voice port " << voice_port_
                  << " unavailable; continuing without voice media\n";
    }
    std::lock_guard<std::mutex> status_lock(status_mutex_);
    status_.state = ModemState::Connected;
    return ConnectProgress::Connected;
}

void ModemClient::cancelConnect() {
    if (!connect_job_) return;
    {
        std::lock_guard<std::mutex> lock(connect_job_->mu);
        connect_job_->cancelled = true;
        // Finished but never adopted: the sockets are ours to close.
        closeSocket(connect_job_->cmd);
        closeSocket(connect_job_->data);
        closeSocket(connect_job_->voice);
    }
    connect_job_.reset();
    std::lock_guard<std::mutex> lock(status_mutex_);
    if (status_.state == ModemState::Connecting) {
        status_.state = ModemState::Disconnected;
    }
}

void ModemClient::resetSessionState() {
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.state = ModemState::Connecting;
        status_.link_state = LinkState::Idle;
        status_.fsm_state = "IDLE";
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
    {
        std::lock_guard<std::mutex> lock(voice_tx_mutex_);
        voice_tx_pending_.clear();
    }
    voice_retry_at_ = std::chrono::steady_clock::now() + voice_reconnect_interval_;
}

void ModemClient::disconnect() {
    cancelConnect();
    if (voice_job_) {
        std::lock_guard<std::mutex> lock(voice_job_->mu);
        voice_job_->cancelled = true;
        closeSocket(voice_job_->sock);
    }
    voice_job_.reset();
    closeSocket(cmd_socket_);
    closeSocket(data_socket_);
    closeSocket(voice_socket_);
    {
        std::lock_guard<std::mutex> lock(voice_tx_mutex_);
        voice_tx_pending_.clear();
    }

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.state = ModemState::Disconnected;
        status_.link_state = LinkState::Idle;
        status_.fsm_state = "IDLE";
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
    failAsyncCommands("disconnected");
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

VoiceMediaState ModemClient::voiceMediaState() const {
    if (!isConnected()) return VoiceMediaState::Disconnected;
    return voice_socket_ != INVALID_SOCK ? VoiceMediaState::Connected
                                         : VoiceMediaState::Reconnecting;
}

void ModemClient::dropVoiceSocket(const char* why) {
    if (voice_socket_ == INVALID_SOCK) return;
    std::cerr << "[ModemClient] voice socket " << why << "; reopening in "
              << voice_reconnect_interval_.count() << " ms\n";
    closeSocket(voice_socket_);
    voice_buffer_.clear();
    {
        std::lock_guard<std::mutex> lock(voice_tx_mutex_);
        voice_tx_pending_.clear();
    }
    voice_retry_at_ = std::chrono::steady_clock::now() + voice_reconnect_interval_;
}

void ModemClient::maybeReopenVoice() {
    if (voice_socket_ != INVALID_SOCK || !isConnected()) return;
    if (voice_job_) {
        std::shared_ptr<VoiceJob> job = voice_job_;
        std::lock_guard<std::mutex> lock(job->mu);
        if (!job->done) return;
        voice_job_.reset();
        if (job->sock != INVALID_SOCK) {
            voice_socket_ = job->sock;
            job->sock = INVALID_SOCK;
            voice_buffer_.clear();
            std::cerr << "[ModemClient] voice port " << voice_port_ << " reopened\n";
        } else {
            voice_retry_at_ = std::chrono::steady_clock::now() + voice_reconnect_interval_;
        }
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < voice_retry_at_) return;
    voice_retry_at_ = now + voice_reconnect_interval_;
    auto job = std::make_shared<VoiceJob>();
    const std::string host = host_;
    const uint16_t port = voice_port_;
    try {
        std::thread([job, host, port]() {
            socket_t sock = INVALID_SOCK;
            if (!connectSocket(sock, host, port)) sock = INVALID_SOCK;
            std::lock_guard<std::mutex> lock(job->mu);
            if (job->cancelled) {
                closeSocket(sock);
            } else {
                job->sock = sock;
            }
            job->done = true;
        }).detach();
    } catch (const std::exception&) {
        return;
    }
    voice_job_ = job;
}

void ModemClient::poll() {
    if (!isConnected()) return;

    expireAsyncCommands();
    if (!isConnected()) return;

    // Events first, so a session that just ended is known before any
    // queued session data is flushed toward the modem.
    readCommandSocket();
    if (!isConnected()) return;
    flushPendingData();
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
    if (isConnected() && voice_socket_ != INVALID_SOCK) {
        bool ok;
        {
            std::lock_guard<std::mutex> lock(voice_tx_mutex_);
            ok = flushVoiceLocked();
        }
        if (!ok) dropVoiceSocket("send error");
    }
    if (isConnected() && voice_socket_ != INVALID_SOCK) readVoiceSocket();
    if (isConnected()) maybeReopenVoice();
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
            dropVoiceSocket("error");
        }
        return;
    }
#else
    ssize_t n = recv(voice_socket_, buffer, sizeof(buffer), 0);
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            dropVoiceSocket("error");
        }
        return;
    }
#endif

    if (n == 0) {
        // Only the media path is gone; the session continues.
        dropVoiceSocket("closed by the modem");
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
            voice_rx_total_samples_ += sample_count;
            // Nobody playing (devices closed): keep only the newest audio
            // rather than replaying minutes of it later.
            if (voice_rx_samples_.size() > kMaxVoiceRxSamples) {
                voice_rx_samples_.erase(
                    voice_rx_samples_.begin(),
                    voice_rx_samples_.begin() +
                        static_cast<std::ptrdiff_t>(voice_rx_samples_.size() - kMaxVoiceRxSamples));
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

    std::string state_name;
    std::string voice_summary;

    for (const auto& [key, value] : splitFields(fields)) {
        if (key == "STATE") {
            state_name = upperCopy(value);
            snapshot.fsm_state = state_name;
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
        } else if (key == "TURN") {
            const std::string turn = upperCopy(value);
            snapshot.turn = (turn == "ISS" || turn == "IRS") ? turn : std::string("-");
        } else if (key == "VOICE") {
            std::string summary = value;
            for (char& ch : summary) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            snapshot.voice_summary = summary;
            voice_summary = summary;
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

    // VOICE= is a summary only; it repairs a missed EVT:VOICE transition.
    if (!voice_summary.empty()) {
        VoiceStatus voice;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            voice = status_.voice;
            const VoiceStatus before = voice;
            applyVoiceSummary(voice_summary, &voice);
            changed = voice.state != before.state || voice.open != before.open ||
                      voice.broadcast_rx != before.broadcast_rx;
            status_.voice = voice;
        }
        if (changed && voice_callback_) voice_callback_(voice);
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
// The hex payload is the whole raw link frame (envelope + body + CRC).
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
            status_.fsm_state = state;
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
            status_.fsm_state = "CONNECTED";
            status_.remote_callsign = call;
            if (!family.empty()) status_.family = upperCopy(family);
            if (!mode.empty()) {
                status_.mode = upperCopy(mode);
                status_.mode_index = std::max(0, parseModeIndex(mode));
            }
        }
        if (connected_callback_) connected_callback_(call);
    } else if (name == "DISCONNECTED") {
        // EVT:DISCONNECTED <reason>; also ends a call that never connected.
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.link_state = LinkState::Idle;
            if (status_.fsm_state != "LISTENING") status_.fsm_state = "IDLE";
            status_.remote_callsign.clear();
        }
        resetSessionStream();
        if (disconnected_callback_) disconnected_callback_(upperCopy(args));
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
        // `EVT:SNR <local_db> <remote_db|->`, sent while connected whenever
        // the link SNR moves by >= 1 dB.
        std::istringstream tokens(args);
        std::string local, remote;
        tokens >> local >> remote;
        const float snr = parseFloatOr(local, 0.0f);
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.snr_db = snr;
            if (!remote.empty()) {
                status_.remote_snr_valid = (remote != "-");
                status_.remote_snr_db =
                    status_.remote_snr_valid ? parseFloatOr(remote, 0.0f) : 0.0f;
            }
        }
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
        const std::string upper_args = upperCopy(args);
        std::istringstream tokens(upper_args);
        std::string first, second;
        tokens >> first >> second;
        snapshot.last_event = upper_args;
        if (first == "CODEC") {
            // `EVT:VOICE CODEC <700C|1300|2400|3200>`: the codec stepped
            // mid-talkspurt. Keep the AUTO(...) form when AUTO chose it.
            if (!second.empty()) {
                snapshot.codec = startsWith(snapshot.codec, "AUTO")
                                     ? "AUTO(" + second + ")"
                                     : second;
            }
        } else if (first == "BCAST") {
            // `EVT:VOICE BCAST <tag>` / `EVT:VOICE BCAST END`.
            if (second == "END" || second.empty()) {
                snapshot.broadcast_rx = false;
                snapshot.broadcast_tag.clear();
            } else {
                snapshot.broadcast_rx = true;
                snapshot.broadcast_tag = second;
            }
        } else if (snapshot.last_event == "OPEN") {
            snapshot.open = true;
            snapshot.ready = true;
            snapshot.state = VoiceState::Ready;
        } else if (snapshot.last_event == "CLOSED") {
            // The voice service closed; the configured mode, the codec and
            // a broadcast still playing out are not part of it.
            VoiceStatus closed;
            closed.mode = snapshot.mode;
            closed.broadcast = snapshot.broadcast;
            closed.codec = snapshot.codec;
            closed.broadcast_rx = snapshot.broadcast_rx;
            closed.broadcast_tag = snapshot.broadcast_tag;
            closed.last_event = "CLOSED";
            snapshot = closed;
        } else if (snapshot.last_event == "TALKING") {
            snapshot.state = VoiceState::TxActive;
            snapshot.active = true;
            snapshot.local = true;
            snapshot.ptt = true;
            snapshot.granted = true;
        } else if (snapshot.last_event == "STOPPED") {
            snapshot.state = snapshot.open ? VoiceState::Ready : VoiceState::Idle;
            snapshot.active = false;
            snapshot.local = false;
            snapshot.ptt = false;
            snapshot.granted = false;
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
        if (!snapshot.broadcast_rx) snapshot.broadcast_tag.clear();
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_.voice = snapshot;
        }
        if (voice_callback_) voice_callback_(snapshot);
    }

    if (startsWith(line, "STATUS ")) {
        applyStatusLine(line.substr(7));
    } else if (startsWith(line, "MAXMODE ")) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.max_mode = upperCopy(trimCopy(line.substr(8)));
    } else if (startsWith(line, "CWID ")) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.cw_id_known = true;
        status_.cw_id = parseBoolToken(line.substr(5));
    } else if (startsWith(line, "BUFFER ")) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_.tx_buffer_bytes = static_cast<size_t>(parseIntOr(trimCopy(line.substr(7)), 0));
    } else if (startsWith(line, "STATE ")) {
        const std::string state = upperCopy(trimCopy(line.substr(6)));
        LinkState previous;
        LinkState next;
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            previous = status_.link_state;
            status_.fsm_state = state;
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

    if (takeAsyncReply(line)) {
        return;
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
    // Fire-and-forget, but always tracked so its reply (or `ERROR:`) is not
    // mistaken for the answer to a later command.
    if (!writeCommandLine(cmd)) return false;
    trackAsyncCommand(cmd, {}, 3000, /*silent=*/true);
    return true;
}

bool ModemClient::writeCommandLine(const std::string& cmd) {
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
// In non-blocking mode the result is only "sent"; `cb` reports the answer.
bool ModemClient::sendCommandOk(const std::string& cmd, int timeout_ms,
                                CommandResultCallback cb) {
    if (nonblocking_commands_) {
        return sendCommandAsync(cmd, std::move(cb), std::max(timeout_ms, 3000));
    }
    const auto response = sendCommandForResponse(cmd, timeout_ms);
    const bool ok = response && !startsWith(*response, "ERROR:");
    if (response && !ok) {
        std::cerr << "[ModemClient] '" << cmd << "' -> " << *response << "\n";
    }
    if (cb) cb(ok, response.value_or(std::string()));
    return ok;
}

// Sends `cmds` one after another, each only once the previous succeeded.
void ModemClient::sendCommandChain(std::vector<std::string> cmds, CommandResultCallback cb) {
    if (cmds.empty()) {
        if (cb) cb(true, "");
        return;
    }
    const std::string first = cmds.front();
    cmds.erase(cmds.begin());
    sendCommandAsync(first, [this, rest = std::move(cmds), cb = std::move(cb)](
                                bool ok, const std::string& response) mutable {
        if (!ok || rest.empty()) {
            if (cb) cb(ok, response);
            return;
        }
        sendCommandChain(std::move(rest), std::move(cb));
    });
}

bool ModemClient::sendCommandAsync(const std::string& cmd,
                                   CommandResultCallback cb,
                                   int timeout_ms) {
    if (!writeCommandLine(cmd)) {
        std::cerr << "[ModemClient] '" << trimCopy(cmd) << "' -> not sent\n";
        if (cb) cb(false, "");
        return false;
    }
    trackAsyncCommand(cmd, std::move(cb), timeout_ms, /*silent=*/false);
    return true;
}

void ModemClient::trackAsyncCommand(const std::string& cmd, CommandResultCallback cb,
                                    int timeout_ms, bool silent) {
    const std::string trimmed = trimCopy(cmd);
    PendingCommand pending;
    pending.cmd = trimmed;
    pending.verb = upperCopy(trimmed.substr(0, trimmed.find(' ')));
    pending.cb = std::move(cb);
    pending.silent = silent;
    pending.deadline = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(std::max(timeout_ms, 1));
    async_commands_.push_back(std::move(pending));
}

// The modem answers control commands in order, so a reply belongs to the
// oldest pending command with that verb; older ones got no reply. `ERROR:`
// carries no verb and is taken as the oldest command's answer. Every command
// is tracked (sendCommand, sendCommandAsync and the blocking queries alike),
// so that ordering holds. A timed-out command stays queued for
// kLateReplyGrace and silently absorbs its late reply.
bool ModemClient::takeAsyncReply(const std::string& line) {
    if (async_commands_.empty()) return false;
    size_t match = async_commands_.size();
    if (startsWith(line, "ERROR:")) {
        match = 0;
    } else {
        for (size_t i = 0; i < async_commands_.size(); ++i) {
            if (responseMatchesVerb(line, async_commands_[i].verb)) {
                match = i;
                break;
            }
        }
    }
    if (match == async_commands_.size()) return false;

    std::vector<PendingCommand> skipped(std::make_move_iterator(async_commands_.begin()),
                                        std::make_move_iterator(async_commands_.begin() + match));
    PendingCommand answered = std::move(async_commands_[match]);
    async_commands_.erase(async_commands_.begin(), async_commands_.begin() + match + 1);

    for (auto& p : skipped) {
        if (p.expired) continue;  // already reported
        if (!p.silent) std::cerr << "[ModemClient] '" << p.cmd << "' -> no reply\n";
        if (p.cb) p.cb(false, "");
    }
    if (answered.expired) return true;  // a late reply, absorbed
    const bool ok = !startsWith(line, "ERROR:");
    if (!ok && !answered.silent) {
        std::cerr << "[ModemClient] '" << answered.cmd << "' -> " << line << "\n";
    }
    if (answered.cb) answered.cb(ok, line);
    return true;
}

void ModemClient::expireAsyncCommands() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<PendingCommand> timed_out;
    for (auto& p : async_commands_) {
        if (p.expired || p.deadline > now) continue;
        p.expired = true;
        p.discard_at = now + kLateReplyGrace;
        PendingCommand report;
        report.cmd = p.cmd;
        report.silent = p.silent;
        report.cb = std::move(p.cb);
        p.cb = {};
        timed_out.push_back(std::move(report));
    }
    async_commands_.erase(
        std::remove_if(async_commands_.begin(), async_commands_.end(),
                       [now](const PendingCommand& p) {
                           return p.expired && p.discard_at <= now;
                       }),
        async_commands_.end());
    // Callbacks last: they may send (and so queue) new commands.
    for (auto& p : timed_out) {
        if (!p.silent) {
            std::cerr << "[ModemClient] '" << p.cmd << "' -> no reply (timeout)\n";
        }
        if (p.cb) p.cb(false, "");
    }
}

size_t ModemClient::pendingAsyncCommands() const {
    return static_cast<size_t>(std::count_if(async_commands_.begin(), async_commands_.end(),
                                             [](const PendingCommand& p) { return !p.expired; }));
}

void ModemClient::failAsyncCommands(const char* reason) {
    std::deque<PendingCommand> failed;
    failed.swap(async_commands_);
    for (auto& p : failed) {
        if (p.expired) continue;
        if (reason && *reason && !p.silent) {
            std::cerr << "[ModemClient] '" << p.cmd << "' -> " << reason << "\n";
        }
        if (p.cb) p.cb(false, "");
    }
}

// A blocking query: tracked like every other command (so a pending async
// command cannot steal its answer, or it theirs), then the socket is pumped
// until the reply arrives or `timeout_ms` passes.
std::optional<std::string> ModemClient::sendCommandForResponse(const std::string& cmd,
                                                               int timeout_ms) {
    if (!writeCommandLine(cmd)) {
        return std::nullopt;
    }
    struct Result {
        bool done = false;
        std::string line;
    };
    auto result = std::make_shared<Result>();
    trackAsyncCommand(cmd, [result](bool, const std::string& response) {
        result->done = true;
        result->line = response;
    }, std::max(timeout_ms, 1), /*silent=*/true);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 1));
    while (!result->done && isConnected()) {
        poll();
        if (result->done) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            expireAsyncCommands();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!result->done || result->line.empty()) {
        return std::nullopt;
    }
    return result->line;
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

// --- operator-facing wording -----------------------------------------------

std::string describeDisconnectReason(const std::string& reason) {
    const std::string r = upperCopy(trimCopy(reason));
    if (r == "NORMAL") return "Disconnected normally";
    if (r == "LINK") return "Link lost (the session dropped)";
    if (r == "NO_ANSWER") return "No answer from the called station";
    if (r == "NO_START_ACK") return "The station answered but the link could not be set up";
    if (r == "REFUSED") return "The called station refused the connection";
    if (r == "TIMEOUT") return "Incoming call timed out before the link was set up";
    if (r.empty()) return "Disconnected";
    return "Disconnected (" + r + ")";
}

std::string describeCommandError(const std::string& cmd, const std::string& response) {
    const std::string trimmed = upperCopy(trimCopy(cmd));
    const std::string verb = trimmed.substr(0, trimmed.find(' '));
    if (response.empty()) {
        return "the modem did not answer";
    }
    if (!startsWith(response, "ERROR:")) {
        return response;
    }
    const std::string reason = upperCopy(trimCopy(std::string_view(response).substr(6)));
    if (reason == "STATE") {
        if (verb == "CONNECT") return "the modem is busy (already calling or in a session)";
        if (verb == "FAMILY") {
            return trimmed == "FAMILY ULTRA"
                       ? "ULTRA is not permitted on the modem, or it is not idle"
                       : "the family can only change while the modem is idle";
        }
        if (verb == "ULTRA") return "the modem refused to change the ULTRA setting now";
        if (verb == "CQ" || verb == "BEACON" || verb == "TXTEST") {
            return "the modem can only do that while idle";
        }
        if (verb == "DISCONNECT") return "there is no session to disconnect";
        return "the modem cannot do that in its current state";
    }
    if (reason == "BADARG") {
        if (verb == "MYCALL") return "the modem rejected the callsign";
        if (verb == "BCAST") return "the broadcast payload was rejected";
        return "the modem rejected the value";
    }
    if (reason == "UNSUPPORTED") return "this modem build does not support that";
    if (reason == "UNKNOWN") return "the modem does not know that command";
    return "the modem answered " + response;
}

// --- high-level commands ---------------------------------------------------

bool ModemClient::setCallsign(const std::string& callsign, CommandResultCallback cb) {
    return sendCommandOk("MYCALL " + upperCopy(trimCopy(callsign)), 500, std::move(cb));
}

bool ModemClient::connectToStation(const std::string& callsign, CommandResultCallback cb) {
    return sendCommandOk("CONNECT " + upperCopy(trimCopy(callsign)), 500, std::move(cb));
}

bool ModemClient::disconnectStation() {
    return sendCommandOk("DISCONNECT");
}

bool ModemClient::abort() {
    return sendCommandOk("ABORT");
}

bool ModemClient::setListenMode(bool enabled) {
    return sendCommandOk(std::string("LISTEN ") + (enabled ? "ON" : "OFF"));
}

bool ModemClient::setMonitorMode(bool enabled) {
    return sendCommandOk(std::string("MONITOR ") + (enabled ? "ON" : "OFF"));
}

bool ModemClient::sendBeacon(CommandResultCallback cb) {
    return sendCommandOk("BEACON", 500, std::move(cb));
}

// Application-level broadcast: Skywave's BCAST carries arbitrary bytes as a
// fragment chain (<= 16 fragments x 256 B). BEACON itself is telemetry-only.
bool ModemClient::sendBroadcastData(const std::vector<uint8_t>& data, CommandResultCallback cb) {
    if (data.empty() || data.size() > 16u * 256u) {
        if (cb) cb(false, "ERROR:BADARG");
        return false;
    }
    return sendCommandOk("BCAST " + bytesToHex(data), 1000, std::move(cb));
}

bool ModemClient::sendCq(CommandResultCallback cb) {
    return sendCommandOk("CQ", 500, std::move(cb));
}

bool ModemClient::setCwId(bool enabled, CommandResultCallback cb) {
    return sendCommandOk(std::string("CWID ") + (enabled ? "ON" : "OFF"), 500, std::move(cb));
}

bool ModemClient::usePttServer(const std::string& host, uint16_t port, CommandResultCallback cb) {
    const std::string h = trimCopy(host);
    if (!isConnected() || port == 0 || h.empty() ||
        h.find_first_of(" \t\r\n") != std::string::npos) {
        if (cb) cb(false, isConnected() ? "ERROR:BADARG" : "");
        return false;
    }
    // An IPv6 literal needs brackets to carry a port.
    const std::string endpoint =
        (h.find(':') != std::string::npos && h.front() != '[')
            ? "[" + h + "]:" + std::to_string(port)
            : h + ":" + std::to_string(port);
    sendCommandChain({"PTTMETHOD TCP", "PTTPORT " + endpoint}, std::move(cb));
    return true;
}

void ModemClient::setPttActive(bool active) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_.ptt_active = active;
}

// --- waveform / ladder control ---------------------------------------------

std::vector<std::string> familyChangeCommands(const std::string& family) {
    const std::string upper = upperCopy(trimCopy(family));
    if (upper == "ULTRA") {
        return {"ULTRA ON", "FAMILY ULTRA"};
    }
    if (upper == "WIDE" || upper == "MID" || upper == "NARROW") {
        return {"FAMILY " + upper};
    }
    return {};
}

bool ModemClient::setFamily(const std::string& family, CommandResultCallback cb) {
    std::vector<std::string> cmds = familyChangeCommands(family);
    if (cmds.empty() || !isConnected()) {
        if (cb) cb(false, cmds.empty() ? "ERROR:BADARG" : "");
        return false;
    }
    // Accepted only while IDLE/LISTENING; the modem answers ERROR:STATE
    // during a session.
    sendCommandChain(std::move(cmds), std::move(cb));
    return true;
}

bool ModemClient::setMode(const std::string& mode, CommandResultCallback cb) {
    return sendCommandOk("MODE " + upperCopy(trimCopy(mode)), 500, std::move(cb));
}

bool ModemClient::setMaxMode(const std::string& mode, CommandResultCallback cb) {
    return sendCommandOk("MAXMODE " + upperCopy(trimCopy(mode)), 500, std::move(cb));
}

bool ModemClient::setCpMode(const std::string& cp) {
    return sendCommandOk("CPMODE " + upperCopy(trimCopy(cp)));
}

void ModemClient::requestModeSettings() {
    // Answers are folded into status_ by processCommandResponse().
    sendCommand("MAXMODE");
    sendCommand("CWID");
}

bool ModemClient::sendData(const std::vector<uint8_t>& message) {
    if (!isConnected()) return false;
    if (message.empty()) return true;

    // Outside a session the modem would hold these bytes in its TX queue
    // and send them, unframed, to whoever connects next. Broadcasts go
    // through BCAST (sendBroadcastData) instead.
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (status_.link_state != LinkState::Connected) return false;
    }
    if (message.size() > kSessionMaxMessage) return false;
    std::vector<uint8_t> data;
    data.reserve(kSessionHeaderLen + message.size());
    data.push_back(kSessionMagic0);
    data.push_back(kSessionMagic1);
    data.push_back(static_cast<uint8_t>(message.size() >> 8));
    data.push_back(static_cast<uint8_t>(message.size() & 0xFFu));
    data.insert(data.end(), message.begin(), message.end());

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
    bool linked;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        linked = (status_.link_state == LinkState::Connected);
    }
    bool ok;
    {
        std::lock_guard<std::mutex> lock(data_tx_mutex_);
        if (!linked) {
            data_tx_pending_.clear();  // session data for a session that is gone
            return;
        }
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

bool ModemClient::reportDialFrequency(double hz, CommandResultCallback cb) {
    if (hz < 0.0) return false;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "FREQINFO %.0f", hz);
    return sendCommandOk(buf, 500, std::move(cb));
}

// --- voice -----------------------------------------------------------------

bool ModemClient::setVoiceCodec(const std::string& codec, CommandResultCallback cb) {
    const std::string normalized = normalizeVoiceCodec(codec);
    if (normalized.empty()) {
        if (cb) cb(false, "ERROR:BADARG");
        return false;
    }
    return sendCommandOk("VOICECODEC " + normalized, 500, std::move(cb));
}

bool ModemClient::setVoiceMode(VoiceMode mode, CommandResultCallback cb) {
    return sendCommandOk("VOICE " + voiceModeCommand(mode), 500, std::move(cb));
}

bool ModemClient::voiceStart(CommandResultCallback cb) {
    return sendCommandOk("VOICESTART", 500, std::move(cb));
}

bool ModemClient::voiceStop(CommandResultCallback cb) {
    return sendCommandOk("VOICESTOP", 500, std::move(cb));
}

bool ModemClient::setVoicePtt(bool enabled, CommandResultCallback cb) {
    return sendCommandOk(std::string("VOICEPTT ") + (enabled ? "ON" : "OFF"), 500, std::move(cb));
}

std::optional<VoiceStatus> ModemClient::queryVoiceStatus() {
    const auto response = sendCommandForResponse("VOICESTATUS", 500);
    if (!response || startsWith(*response, "ERROR:")) {
        return std::nullopt;
    }
    return getVoiceStatus();
}

bool ModemClient::sendVoicePcm(const int16_t* samples, size_t sample_count) {
    if (!isConnected() || voice_socket_ == INVALID_SOCK || !samples || sample_count == 0 ||
        sample_count * sizeof(int16_t) > 0xFFFFu) {
        return false;
    }

    const size_t payload_len = sample_count * sizeof(int16_t);
    bool ok = true;
    {
        std::lock_guard<std::mutex> lock(voice_tx_mutex_);
        if (voice_tx_pending_.size() + kVoiceMediaHeaderLen + payload_len > kMaxVoiceBacklogBytes) {
            return false;  // dropped whole: the stream stays record-aligned
        }
        const size_t at = voice_tx_pending_.size();
        voice_tx_pending_.resize(at + kVoiceMediaHeaderLen + payload_len);
        uint8_t* rec = voice_tx_pending_.data() + at;
        rec[0] = kVoiceMediaVersion;
        rec[1] = kVoiceMediaUplinkPcm;
        storeU16Be(rec + 2, static_cast<uint16_t>(payload_len));
        storeU32Be(rec + 4, voice_tx_sequence_++);
        storeU64Be(rec + 8, voice_tx_sample_counter_);
        // PCM16LE on the wire.
        for (size_t i = 0; i < sample_count; ++i) {
            const uint16_t v = static_cast<uint16_t>(samples[i]);
            rec[kVoiceMediaHeaderLen + i * 2] = static_cast<uint8_t>(v & 0xFFu);
            rec[kVoiceMediaHeaderLen + i * 2 + 1] = static_cast<uint8_t>(v >> 8);
        }
        voice_tx_sample_counter_ += sample_count;
        ok = flushVoiceLocked();
    }
    if (!ok) {
        dropVoiceSocket("send error");
        return false;
    }
    return true;
}

size_t ModemClient::pendingVoiceBytes() const {
    std::lock_guard<std::mutex> lock(voice_tx_mutex_);
    return voice_tx_pending_.size();
}

// Writes what the voice socket takes without blocking; whatever is left
// (a record's tail included) goes first next time, so records stay whole
// on the wire. False only on a hard socket error.
bool ModemClient::flushVoiceLocked() {
    size_t total_sent = 0;
    bool ok = true;
    while (total_sent < voice_tx_pending_.size() && voice_socket_ != INVALID_SOCK) {
#ifdef _WIN32
        int sent = send(voice_socket_,
                        reinterpret_cast<const char*>(voice_tx_pending_.data() + total_sent),
                        static_cast<int>(voice_tx_pending_.size() - total_sent), 0);
        if (sent > 0) {
            total_sent += static_cast<size_t>(sent);
            continue;
        }
        if (sent < 0 && WSAGetLastError() == WSAEWOULDBLOCK) break;
        ok = false;
        break;
#else
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        ssize_t sent = send(voice_socket_, voice_tx_pending_.data() + total_sent,
                            voice_tx_pending_.size() - total_sent, flags);
        if (sent > 0) {
            total_sent += static_cast<size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        ok = false;
        break;
#endif
    }
    voice_tx_pending_.erase(voice_tx_pending_.begin(),
                            voice_tx_pending_.begin() + static_cast<std::ptrdiff_t>(total_sent));
    return ok;
}

uint64_t ModemClient::voiceSamplesReceived() const {
    std::lock_guard<std::mutex> lock(voice_mutex_);
    return voice_rx_total_samples_;
}

size_t ModemClient::voiceSamplesBuffered() const {
    std::lock_guard<std::mutex> lock(voice_mutex_);
    return voice_rx_samples_.size();
}

void ModemClient::discardVoicePcm() {
    std::lock_guard<std::mutex> lock(voice_mutex_);
    voice_rx_samples_.clear();
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
