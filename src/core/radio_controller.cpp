// RadioController - local radio control and TCP PTT service

#include "skywave_link/radio_controller.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <sstream>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif
#include <hamlib/rig.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace skylink {

namespace {

using rig_handle_t = RIG*;

#ifdef _WIN32
using socket_handle_t = SOCKET;
constexpr socket_handle_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_handle_t = int;
constexpr socket_handle_t kInvalidSocket = -1;
#endif

constexpr uint16_t kDefaultPttPort = 4533;

void closeSocket(socket_handle_t fd) {
#ifdef _WIN32
    if (fd != INVALID_SOCKET) {
        closesocket(fd);
    }
#else
    if (fd >= 0) {
        close(fd);
    }
#endif
}

bool ensureSocketRuntime() {
#ifdef _WIN32
    static std::atomic<bool> ready{false};
    static std::mutex init_mutex;
    if (ready.load()) {
        return true;
    }
    std::lock_guard<std::mutex> lock(init_mutex);
    if (ready.load()) {
        return true;
    }
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        return false;
    }
    ready.store(true);
#endif
    return true;
}

std::string trimCopy(const std::string& input) {
    const auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = input.find_last_not_of(" \t\r\n");
    return input.substr(first, last - first + 1);
}

std::string upperCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

bool parseEndpoint(const std::string& endpoint, std::string* host_out, uint16_t* port_out) {
    if (!host_out || !port_out) {
        return false;
    }

    std::string trimmed = trimCopy(endpoint);
    if (trimmed.empty()) {
        *host_out = "127.0.0.1";
        *port_out = kDefaultPttPort;
        return true;
    }

    std::string host = trimmed;
    uint16_t port = kDefaultPttPort;
    const std::size_t colon = trimmed.rfind(':');
    if (colon != std::string::npos && colon > 0 && colon + 1 < trimmed.size()) {
        const std::string port_str = trimmed.substr(colon + 1);
        char* end = nullptr;
        const long parsed = std::strtol(port_str.c_str(), &end, 10);
        if (end && *end == '\0' && parsed > 0 && parsed <= 65535) {
            host = trimmed.substr(0, colon);
            port = static_cast<uint16_t>(parsed);
        }
    }

    if (host.empty()) {
        host = "127.0.0.1";
    }

    *host_out = host;
    *port_out = port;
    return true;
}

socket_handle_t connectTcpEndpoint(const std::string& endpoint) {
    if (!ensureSocketRuntime()) {
        return kInvalidSocket;
    }

    std::string host;
    uint16_t port = 0;
    if (!parseEndpoint(endpoint, &host, &port)) {
        return kInvalidSocket;
    }

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    struct addrinfo* result = nullptr;
    const std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0) {
        return kInvalidSocket;
    }

    socket_handle_t fd = kInvalidSocket;
    for (struct addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == kInvalidSocket) {
            continue;
        }
        if (connect(fd, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            break;
        }
        closeSocket(fd);
        fd = kInvalidSocket;
    }

    freeaddrinfo(result);
    return fd;
}

bool sendAll(socket_handle_t fd, const char* data, size_t len) {
    while (len > 0) {
#ifdef _WIN32
        const int sent = send(fd, data, static_cast<int>(len), 0);
#else
        const ssize_t sent = send(fd, data, len, 0);
#endif
        if (sent <= 0) {
            return false;
        }
        data += sent;
        len -= static_cast<size_t>(sent);
    }
    return true;
}

bool recvLine(socket_handle_t fd, std::string* line_out) {
    if (!line_out) {
        return false;
    }

    line_out->clear();
    char ch = '\0';
    while (true) {
#ifdef _WIN32
        const int rc = recv(fd, &ch, 1, 0);
#else
        const ssize_t rc = recv(fd, &ch, 1, 0);
#endif
        if (rc <= 0) {
            return false;
        }
        if (ch == '\n') {
            break;
        }
        if (ch != '\r') {
            line_out->push_back(ch);
        }
        if (line_out->size() > 512) {
            return false;
        }
    }
    return true;
}

bool modeFromString(const std::string& mode_name, rmode_t* mode_out) {
    if (!mode_out) {
        return false;
    }
    const std::string mode = upperCopy(trimCopy(mode_name));
    if (mode == "USB") {
        *mode_out = RIG_MODE_USB;
    } else if (mode == "LSB") {
        *mode_out = RIG_MODE_LSB;
    } else if (mode == "CW") {
        *mode_out = RIG_MODE_CW;
    } else if (mode == "CWR") {
        *mode_out = RIG_MODE_CWR;
    } else if (mode == "AM") {
        *mode_out = RIG_MODE_AM;
    } else if (mode == "FM") {
        *mode_out = RIG_MODE_FM;
    } else if (mode == "DIGU" || mode == "DATA-USB") {
        *mode_out = RIG_MODE_PKTUSB;
    } else if (mode == "DIGL" || mode == "DATA-LSB") {
        *mode_out = RIG_MODE_PKTLSB;
    } else {
        return false;
    }
    return true;
}

std::string modeToString(rmode_t mode) {
    switch (mode) {
    case RIG_MODE_USB: return "USB";
    case RIG_MODE_LSB: return "LSB";
    case RIG_MODE_CW: return "CW";
    case RIG_MODE_CWR: return "CWR";
    case RIG_MODE_AM: return "AM";
    case RIG_MODE_FM: return "FM";
    case RIG_MODE_PKTUSB: return "DIGU";
    case RIG_MODE_PKTLSB: return "DIGL";
    default: return "UNKNOWN";
    }
}

std::string rigError(int rc) {
    const char* text = rigerror(rc);
    if (!text || !*text) {
        return "unknown hamlib error";
    }
    return text;
}

} // namespace

RadioController::RadioController() = default;

RadioController::~RadioController() {
    stopPttServer();
    disconnect();
}

void RadioController::applyConfig(const RadioConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = config;
    if (config_.ptt_server_port == 0) {
        config_.ptt_server_port = kDefaultPttPort;
    }
}

bool RadioController::connect() {
    std::lock_guard<std::mutex> lock(mutex_);
    return connectLocked();
}

void RadioController::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    disconnectLocked();
}

bool RadioController::reconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    disconnectLocked();
    return connectLocked();
}

bool RadioController::isConfigured() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.enabled;
}

bool RadioController::isConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

bool RadioController::isDryRun() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dry_run_;
}

bool RadioController::setFrequency(uint64_t hz) {
    std::lock_guard<std::mutex> lock(mutex_);
    return setFrequencyLocked(hz);
}

std::optional<uint64_t> RadioController::getFrequency() {
    std::lock_guard<std::mutex> lock(mutex_);
    return getFrequencyLocked();
}

bool RadioController::setMode(const std::string& mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    return setModeLocked(mode);
}

std::optional<std::string> RadioController::getMode() {
    std::lock_guard<std::mutex> lock(mutex_);
    return getModeLocked();
}

bool RadioController::setPtt(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    return setPttLocked(enabled);
}

bool RadioController::getPtt() {
    std::lock_guard<std::mutex> lock(mutex_);
    return getPttLocked();
}

bool RadioController::startPttServer() {
    std::lock_guard<std::mutex> lock(mutex_);
    return startPttServerLocked();
}

void RadioController::stopPttServer() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopPttServerLocked();
}

RadioController::Status RadioController::getStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);

    Status status;
    status.configured = config_.enabled;
    status.dry_run = dry_run_;
    status.connected = connected_;
    status.ptt = ptt_on_;
    status.frequency_hz = frequency_hz_;
    status.mode = mode_;
    status.device = config_.device;
    status.hamlib_model = config_.hamlib_model;
    status.ptt_server_running = ptt_server_running_;
    status.ptt_bind = config_.ptt_server_bind;
    status.ptt_port = config_.ptt_server_port;
    status.last_error = last_error_;
    return status;
}

void RadioController::setErrorLocked(const std::string& message) {
    last_error_ = message;
}

bool RadioController::ensureConnectedLocked() {
    if (connected_) {
        return true;
    }
    return connectLocked();
}

bool RadioController::connectLocked() {
    if (!config_.enabled) {
        setErrorLocked("radio control disabled");
        return false;
    }
    if (connected_) {
        return true;
    }

    if (config_.hamlib_model <= 0) {
        dry_run_ = true;
        connected_ = true;
        ptt_on_ = false;
        last_error_.clear();
        return true;
    }

    rig_load_all_backends();
    rig_handle_t rig = rig_init(config_.hamlib_model);
    if (!rig) {
        setErrorLocked("hamlib rig_init failed");
        return false;
    }

    if (!config_.device.empty()) {
        const struct confparams* path_param = rig_confparam_lookup(rig, "rig_pathname");
        if (path_param) {
            const int rc = rig_set_conf(rig, path_param->token, config_.device.c_str());
            if (rc != RIG_OK) {
                setErrorLocked("rig_pathname: " + rigError(rc));
                rig_cleanup(rig);
                return false;
            }
        }
    }

    if (config_.baud > 0) {
        const struct confparams* baud_param = rig_confparam_lookup(rig, "serial_speed");
        if (baud_param) {
            const std::string baud = std::to_string(config_.baud);
            const int rc = rig_set_conf(rig, baud_param->token, baud.c_str());
            if (rc != RIG_OK) {
                setErrorLocked("serial_speed: " + rigError(rc));
                rig_cleanup(rig);
                return false;
            }
        }
    }

    const int rc = rig_open(rig);
    if (rc != RIG_OK) {
        setErrorLocked("rig_open: " + rigError(rc));
        rig_cleanup(rig);
        return false;
    }

    rig_ = rig;
    dry_run_ = false;
    connected_ = true;
    ptt_on_ = false;
    last_error_.clear();
    (void)getFrequencyLocked();
    (void)getModeLocked();
    return true;
}

void RadioController::disconnectLocked() {
    if (rig_) {
        rig_handle_t rig = static_cast<rig_handle_t>(rig_);
        (void)rig_set_ptt(rig, RIG_VFO_CURR, RIG_PTT_OFF);
        (void)rig_close(rig);
        rig_cleanup(rig);
        rig_ = nullptr;
    }
    connected_ = false;
    dry_run_ = false;
    ptt_on_ = false;
}

bool RadioController::setFrequencyLocked(uint64_t hz) {
    if (hz == 0 || !ensureConnectedLocked()) {
        return false;
    }

    if (dry_run_) {
        frequency_hz_ = hz;
        return true;
    }

    rig_handle_t rig = static_cast<rig_handle_t>(rig_);
    const int rc = rig_set_freq(rig, RIG_VFO_CURR, static_cast<freq_t>(hz));
    if (rc != RIG_OK) {
        setErrorLocked("rig_set_freq: " + rigError(rc));
        return false;
    }

    frequency_hz_ = hz;
    last_error_.clear();
    return true;
}

std::optional<uint64_t> RadioController::getFrequencyLocked() {
    if (!ensureConnectedLocked()) {
        return std::nullopt;
    }

    if (dry_run_) {
        return frequency_hz_;
    }

    rig_handle_t rig = static_cast<rig_handle_t>(rig_);
    freq_t freq = 0;
    const int rc = rig_get_freq(rig, RIG_VFO_CURR, &freq);
    if (rc != RIG_OK) {
        setErrorLocked("rig_get_freq: " + rigError(rc));
        return std::nullopt;
    }

    frequency_hz_ = static_cast<uint64_t>(freq);
    last_error_.clear();
    return frequency_hz_;
}

bool RadioController::setModeLocked(const std::string& mode_name) {
    if (!ensureConnectedLocked()) {
        return false;
    }

    const std::string normalized = upperCopy(trimCopy(mode_name));
    if (normalized.empty()) {
        setErrorLocked("empty mode");
        return false;
    }

    if (dry_run_) {
        mode_ = normalized;
        return true;
    }

    rmode_t mode = RIG_MODE_NONE;
    if (!modeFromString(normalized, &mode)) {
        setErrorLocked("unsupported mode: " + normalized);
        return false;
    }

    rig_handle_t rig = static_cast<rig_handle_t>(rig_);
    const int rc = rig_set_mode(rig, RIG_VFO_CURR, mode, static_cast<pbwidth_t>(2800));
    if (rc != RIG_OK) {
        setErrorLocked("rig_set_mode: " + rigError(rc));
        return false;
    }

    mode_ = normalized;
    last_error_.clear();
    return true;
}

std::optional<std::string> RadioController::getModeLocked() {
    if (!ensureConnectedLocked()) {
        return std::nullopt;
    }

    if (dry_run_) {
        return mode_;
    }

    rig_handle_t rig = static_cast<rig_handle_t>(rig_);
    rmode_t mode = RIG_MODE_NONE;
    pbwidth_t width = 0;
    const int rc = rig_get_mode(rig, RIG_VFO_CURR, &mode, &width);
    if (rc != RIG_OK) {
        setErrorLocked("rig_get_mode: " + rigError(rc));
        return std::nullopt;
    }

    (void)width;
    mode_ = modeToString(mode);
    last_error_.clear();
    return mode_;
}

bool RadioController::setPttLocked(bool enabled) {
    if (!ensureConnectedLocked()) {
        return false;
    }

    if (dry_run_) {
        ptt_on_ = enabled;
        return true;
    }

    rig_handle_t rig = static_cast<rig_handle_t>(rig_);
    const int rc = rig_set_ptt(rig, RIG_VFO_CURR, enabled ? RIG_PTT_ON : RIG_PTT_OFF);
    if (rc != RIG_OK) {
        setErrorLocked("rig_set_ptt: " + rigError(rc));
        return false;
    }

    ptt_on_ = enabled;
    last_error_.clear();
    return true;
}

bool RadioController::getPttLocked() {
    if (!ensureConnectedLocked()) {
        return false;
    }

    if (dry_run_) {
        return ptt_on_;
    }

    rig_handle_t rig = static_cast<rig_handle_t>(rig_);
    ptt_t state = RIG_PTT_OFF;
    const int rc = rig_get_ptt(rig, RIG_VFO_CURR, &state);
    if (rc == RIG_OK) {
        ptt_on_ = (state != RIG_PTT_OFF);
        last_error_.clear();
    } else {
        setErrorLocked("rig_get_ptt: " + rigError(rc));
    }
    return ptt_on_;
}

bool RadioController::startPttServerLocked() {
    if (!config_.ptt_server_enabled) {
        return false;
    }
    if (ptt_server_running_) {
        return true;
    }
    if (!ensureSocketRuntime()) {
        setErrorLocked("socket runtime unavailable");
        return false;
    }

    std::string host;
    uint16_t port = 0;
    if (!parseEndpoint(config_.ptt_server_bind + ":" + std::to_string(config_.ptt_server_port),
                       &host, &port)) {
        setErrorLocked("invalid PTT bind address");
        return false;
    }

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_PASSIVE;

    struct addrinfo* result = nullptr;
    const std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0) {
        setErrorLocked("getaddrinfo failed for PTT server");
        return false;
    }

    socket_handle_t listener = kInvalidSocket;
    for (struct addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        listener = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (listener == kInvalidSocket) {
            continue;
        }
        int opt = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));
        if (bind(listener, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0 &&
            listen(listener, 4) == 0) {
            break;
        }
        closeSocket(listener);
        listener = kInvalidSocket;
    }
    freeaddrinfo(result);

    if (listener == kInvalidSocket) {
        setErrorLocked("failed to bind PTT server");
        return false;
    }

    ptt_listener_fd_ = static_cast<int>(listener);
    ptt_server_stop_.store(false);
    ptt_server_running_ = true;
    ptt_server_thread_ = std::thread(&RadioController::pttServerLoop, this);
    last_error_.clear();
    return true;
}

void RadioController::stopPttServerLocked() {
    ptt_server_stop_.store(true);

    if (ptt_listener_fd_ >= 0) {
        closeSocket(static_cast<socket_handle_t>(ptt_listener_fd_));
        ptt_listener_fd_ = -1;
    }

    if (ptt_server_thread_.joinable()) {
        ptt_server_thread_.join();
    }

    for (std::thread& thread : ptt_client_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    ptt_client_threads_.clear();
    ptt_server_running_ = false;
}

void RadioController::pttServerLoop() {
    while (!ptt_server_stop_.load()) {
        const socket_handle_t listener = static_cast<socket_handle_t>(ptt_listener_fd_);
        if (listener == kInvalidSocket) {
            break;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listener, &readfds);
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;
        const int ready = select(static_cast<int>(listener + 1), &readfds, nullptr, nullptr, &timeout);
        if (ready <= 0) {
            continue;
        }
        if (!FD_ISSET(listener, &readfds)) {
            continue;
        }

        socket_handle_t client = accept(listener, nullptr, nullptr);
        if (client == kInvalidSocket) {
            continue;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        ptt_client_threads_.emplace_back(&RadioController::pttClientLoop,
                                         this,
                                         static_cast<int>(client));
    }
}

void RadioController::pttClientLoop(int client_fd) {
    const socket_handle_t client = static_cast<socket_handle_t>(client_fd);
    std::string line;
    while (!ptt_server_stop_.load() && recvLine(client, &line)) {
        const std::string command = upperCopy(trimCopy(line));
        std::string response;
        if (command == "PING") {
            response = "OK\n";
        } else if (command == "STATUS") {
            response = getPtt() ? "PTT:ON\n" : "PTT:OFF\n";
        } else if (command == "KEY" || command == "SETPTT ON") {
            response = setPtt(true) ? "OK\n" : ("ERROR " + getStatus().last_error + "\n");
        } else if (command == "UNKEY" || command == "SETPTT OFF") {
            response = setPtt(false) ? "OK\n" : ("ERROR " + getStatus().last_error + "\n");
        } else {
            response = "ERROR unsupported command\n";
        }

        if (!sendAll(client, response.c_str(), response.size())) {
            break;
        }
    }
    closeSocket(client);
}

} // namespace skylink
