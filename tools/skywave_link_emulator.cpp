// Skywave Link Emulator
// Emulates a minimal modem bridge so two Skywave Link GUI instances can test
// messaging/link workflows without RF/audio/modem hardware.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <unistd.h>
    #include <errno.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
#endif

namespace {

std::atomic<bool> g_stop_requested{false};

void onSignal(int) {
    g_stop_requested.store(true, std::memory_order_relaxed);
}

std::string trimSpaces(const std::string& input) {
    size_t start = 0;
    while (start < input.size() &&
           std::isspace(static_cast<unsigned char>(input[start])) != 0) {
        ++start;
    }
    size_t end = input.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        --end;
    }
    return input.substr(start, end - start);
}

std::string upperAscii(std::string input) {
    std::transform(input.begin(), input.end(), input.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return input;
}

std::string normalizeCallsign(const std::string& input) {
    return upperAscii(trimSpaces(input));
}

bool parsePort(const std::string& text, uint16_t* out_port) {
    if (out_port == nullptr) {
        return false;
    }
    char* end_ptr = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end_ptr, 10);
    if (end_ptr == nullptr || *end_ptr != '\0' || value == 0 || value > 65535UL) {
        return false;
    }
    *out_port = static_cast<uint16_t>(value);
    return true;
}

void closeSocket(socket_t& sock) {
    if (sock == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
    sock = kInvalidSocket;
}

void enableNoDelay(socket_t sock) {
    if (sock == kInvalidSocket) {
        return;
    }
    const int nodelay = 1;
    (void)setsockopt(sock,
                     IPPROTO_TCP,
                     TCP_NODELAY,
                     reinterpret_cast<const char*>(&nodelay),
                     static_cast<int>(sizeof(nodelay)));
}

bool sendAll(socket_t sock, const uint8_t* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        int send_flags = 0;
#ifdef MSG_NOSIGNAL
        send_flags = MSG_NOSIGNAL;
#endif
        const int n = static_cast<int>(
            ::send(sock,
                   reinterpret_cast<const char*>(data + offset),
                   static_cast<int>(size - offset),
                   send_flags));
        if (n <= 0) {
            return false;
        }
        offset += static_cast<size_t>(n);
    }
    return true;
}

bool waitForReadable(socket_t sock, int timeout_ms) {
    if (sock == kInvalidSocket) {
        return false;
    }

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);

    struct timeval tv {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

#ifdef _WIN32
    const int ready = select(0, &rfds, nullptr, nullptr, &tv);
#else
    const int ready = select(sock + 1, &rfds, nullptr, nullptr, &tv);
#endif
    return ready > 0;
}

bool isPotentialCommandRoutedMarker(uint8_t marker) {
    // These payload markers are commonly sent immediately after BEACON/CQ/PING.
    switch (marker) {
        case 0xFA:  // Beacon payload
        case 0xFB:  // Broadcast
        case 0xFC:  // CQ
        case 0xF4:  // Position beacon
        case 0xF9:  // Pending message indicator
        case 0xE0:  // Ping request
        case 0xD0:  // BBS announce
        case 0xD1:  // BBS remove announce
            return true;
        default:
            return false;
    }
}

bool isBroadcastRoutedMarker(uint8_t marker) {
    switch (marker) {
        case 0xFA:  // Beacon payload
        case 0xFB:  // Broadcast text
        case 0xFC:  // CQ
        case 0xF4:  // Position beacon
        case 0xF9:  // Pending-message beacon
        case 0xD0:  // BBS announce
        case 0xD1:  // BBS remove announce
            return true;
        default:
            return false;
    }
}

std::vector<uint8_t> buildWrappedFrame(const std::string& from, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> wrapped;
    const size_t from_len = std::min<size_t>(255, from.size());
    wrapped.reserve(payload.size() + from_len + 2);
    wrapped.push_back(0xFA);
    wrapped.push_back(static_cast<uint8_t>(from_len));
    wrapped.insert(wrapped.end(), from.begin(), from.begin() + static_cast<ptrdiff_t>(from_len));
    wrapped.insert(wrapped.end(), payload.begin(), payload.end());
    return wrapped;
}

enum class NextTxRoute {
    None,
    Broadcast,
    Directed
};

struct Station {
    int id = 0;
    int endpoint_slot = -1;  // -1 in shared-port mode, 0/1 in split-endpoint mode
    socket_t cmd_socket = kInvalidSocket;
    socket_t data_socket = kInvalidSocket;
    std::string callsign;

    int peer_id = 0;
    NextTxRoute next_tx_route = NextTxRoute::None;
    std::string directed_target;
    std::chrono::steady_clock::time_point route_deadline{};
    std::string directed_reply_target;
    std::chrono::steady_clock::time_point directed_reply_deadline{};

    uint64_t cat_frequency_hz = 7100000;
    std::string cat_mode = "USB";

    std::mutex cmd_write_mutex;
    std::mutex data_write_mutex;
};

struct OutboundLine {
    std::shared_ptr<Station> station;
    std::string line;
};

struct TargetDataFrame {
    std::shared_ptr<Station> station;
    std::vector<uint8_t> payload;
};

class LinkEmulator {
public:
    LinkEmulator(std::string bind_host, uint16_t cmd_port, uint16_t data_port, bool verbose)
        : bind_host_(std::move(bind_host)),
          cmd_port_(cmd_port),
          data_port_(data_port),
          verbose_(verbose) {}

    LinkEmulator(std::string bind_host,
                 uint16_t endpoint_a_cmd_port,
                 uint16_t endpoint_a_data_port,
                 uint16_t endpoint_b_cmd_port,
                 uint16_t endpoint_b_data_port,
                 bool verbose)
        : bind_host_(std::move(bind_host)),
          verbose_(verbose),
          split_endpoint_mode_(true) {
        endpoint_cmd_ports_[0] = endpoint_a_cmd_port;
        endpoint_data_ports_[0] = endpoint_a_data_port;
        endpoint_cmd_ports_[1] = endpoint_b_cmd_port;
        endpoint_data_ports_[1] = endpoint_b_data_port;
    }

    ~LinkEmulator() {
        stop();
    }

    bool start() {
#ifdef _WIN32
        WSADATA wsa_data {};
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            std::cerr << "[LinkEmu] WSAStartup failed\n";
            return false;
        }
#endif
        if (split_endpoint_mode_) {
            if (!startSplitEndpointMode()) {
                stop();
                return false;
            }
            return true;
        }

        if (!openListener(cmd_port_, &cmd_listener_)) {
            std::cerr << "[LinkEmu] Failed to open command listener on " << bind_host_
                      << ":" << cmd_port_ << "\n";
            stop();
            return false;
        }
        if (!openListener(data_port_, &data_listener_)) {
            std::cerr << "[LinkEmu] Failed to open data listener on " << bind_host_
                      << ":" << data_port_ << "\n";
            stop();
            return false;
        }

        running_.store(true, std::memory_order_relaxed);
        cmd_accept_thread_ = std::thread(&LinkEmulator::acceptLoop, this, true);
        data_accept_thread_ = std::thread(&LinkEmulator::acceptLoop, this, false);

        std::cout << "[LinkEmu] Listening on " << bind_host_
                  << " (cmd " << cmd_port_
                  << ", data " << data_port_ << ")\n";
        return true;
    }

    void stop() {
        const bool was_running = running_.exchange(false, std::memory_order_relaxed);
        (void)was_running;

        if (split_endpoint_mode_) {
            for (int i = 0; i < 2; ++i) {
                closeSocket(endpoint_cmd_listeners_[i]);
                closeSocket(endpoint_data_listeners_[i]);
            }
            for (int i = 0; i < 2; ++i) {
                if (endpoint_cmd_accept_threads_[i].joinable()) {
                    endpoint_cmd_accept_threads_[i].join();
                }
                if (endpoint_data_accept_threads_[i].joinable()) {
                    endpoint_data_accept_threads_[i].join();
                }
            }
        } else {
            closeSocket(cmd_listener_);
            closeSocket(data_listener_);

            if (cmd_accept_thread_.joinable()) {
                cmd_accept_thread_.join();
            }
            if (data_accept_thread_.joinable()) {
                data_accept_thread_.join();
            }
        }

        std::vector<std::shared_ptr<Station>> stations;
        {
            std::lock_guard<std::mutex> lock(stations_mutex_);
            for (auto& [_, station] : stations_) {
                stations.push_back(station);
            }
            stations_.clear();
            pending_data_attach_.clear();
            endpoint_station_ids_[0] = 0;
            endpoint_station_ids_[1] = 0;
        }
        for (auto& station : stations) {
            closeSocket(station->cmd_socket);
            closeSocket(station->data_socket);
        }

#ifdef _WIN32
        WSACleanup();
#endif
    }

private:
    bool startSplitEndpointMode() {
        for (int i = 0; i < 2; ++i) {
            if (!openListener(endpoint_cmd_ports_[i], &endpoint_cmd_listeners_[i])) {
                std::cerr << "[LinkEmu] Failed to open endpoint " << (i + 1)
                          << " command listener on " << bind_host_ << ":"
                          << endpoint_cmd_ports_[i] << "\n";
                return false;
            }
            if (!openListener(endpoint_data_ports_[i], &endpoint_data_listeners_[i])) {
                std::cerr << "[LinkEmu] Failed to open endpoint " << (i + 1)
                          << " data listener on " << bind_host_ << ":"
                          << endpoint_data_ports_[i] << "\n";
                return false;
            }
        }

        {
            std::lock_guard<std::mutex> lock(stations_mutex_);
            for (int i = 0; i < 2; ++i) {
                auto station = std::make_shared<Station>();
                station->id = next_station_id_++;
                station->endpoint_slot = i;
                station->route_deadline = std::chrono::steady_clock::now();
                stations_[station->id] = station;
                endpoint_station_ids_[i] = station->id;
            }
        }

        running_.store(true, std::memory_order_relaxed);
        for (int i = 0; i < 2; ++i) {
            endpoint_cmd_accept_threads_[i] =
                std::thread(&LinkEmulator::acceptLoopEndpoint, this, i, true);
            endpoint_data_accept_threads_[i] =
                std::thread(&LinkEmulator::acceptLoopEndpoint, this, i, false);
        }

        std::cout << "[LinkEmu] Split-endpoint bridge listening on " << bind_host_ << "\n";
        std::cout << "[LinkEmu]   Endpoint 1: cmd " << endpoint_cmd_ports_[0]
                  << ", data " << endpoint_data_ports_[0] << "\n";
        std::cout << "[LinkEmu]   Endpoint 2: cmd " << endpoint_cmd_ports_[1]
                  << ", data " << endpoint_data_ports_[1] << "\n";
        return true;
    }

    bool openListener(uint16_t port, socket_t* out_listener) {
        if (out_listener == nullptr) {
            return false;
        }

        struct addrinfo hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_PASSIVE;

        const std::string port_text = std::to_string(port);
        struct addrinfo* result = nullptr;
        const char* host_ptr = bind_host_.empty() ? nullptr : bind_host_.c_str();
        if (getaddrinfo(host_ptr, port_text.c_str(), &hints, &result) != 0) {
            return false;
        }

        socket_t listener = kInvalidSocket;
        for (auto* ai = result; ai != nullptr; ai = ai->ai_next) {
            listener = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (listener == kInvalidSocket) {
                continue;
            }

            const int opt = 1;
            (void)setsockopt(listener,
                             SOL_SOCKET,
                             SO_REUSEADDR,
                             reinterpret_cast<const char*>(&opt),
                             static_cast<int>(sizeof(opt)));

            if (bind(listener, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) != 0) {
                closeSocket(listener);
                continue;
            }
            if (listen(listener, 16) != 0) {
                closeSocket(listener);
                continue;
            }
            break;
        }
        freeaddrinfo(result);

        if (listener == kInvalidSocket) {
            return false;
        }
        *out_listener = listener;
        return true;
    }

    std::shared_ptr<Station> endpointStationBySlotLocked(int endpoint_slot) {
        if (endpoint_slot < 0 || endpoint_slot >= 2) {
            return nullptr;
        }
        const int station_id = endpoint_station_ids_[endpoint_slot];
        if (station_id <= 0) {
            return nullptr;
        }
        auto it = stations_.find(station_id);
        if (it == stations_.end()) {
            return nullptr;
        }
        return it->second;
    }

    void acceptLoop(bool command_listener) {
        socket_t listener = command_listener ? cmd_listener_ : data_listener_;
        while (running_.load(std::memory_order_relaxed)) {
            if (!waitForReadable(listener, 200)) {
                continue;
            }

            struct sockaddr_storage addr {};
            socklen_t addr_len = static_cast<socklen_t>(sizeof(addr));
            socket_t client = accept(listener,
                                     reinterpret_cast<struct sockaddr*>(&addr),
                                     &addr_len);
            if (client == kInvalidSocket) {
                continue;
            }
            enableNoDelay(client);

            if (command_listener) {
                auto station = registerCommandStation(client);
                if (!station) {
                    closeSocket(client);
                    continue;
                }
                std::thread(&LinkEmulator::commandSocketLoop, this, station).detach();
            } else {
                auto station = attachDataSocket(client);
                if (!station) {
                    if (verbose_) {
                        std::cerr << "[LinkEmu] Rejecting data socket (no pending command client)\n";
                    }
                    closeSocket(client);
                    continue;
                }
                std::thread(&LinkEmulator::dataSocketLoop, this, station).detach();
            }
        }
    }

    void acceptLoopEndpoint(int endpoint_slot, bool command_listener) {
        socket_t listener = command_listener
            ? endpoint_cmd_listeners_[endpoint_slot]
            : endpoint_data_listeners_[endpoint_slot];

        while (running_.load(std::memory_order_relaxed)) {
            if (!waitForReadable(listener, 200)) {
                continue;
            }

            struct sockaddr_storage addr {};
            socklen_t addr_len = static_cast<socklen_t>(sizeof(addr));
            socket_t client = accept(listener,
                                     reinterpret_cast<struct sockaddr*>(&addr),
                                     &addr_len);
            if (client == kInvalidSocket) {
                continue;
            }
            enableNoDelay(client);

            std::shared_ptr<Station> station;
            if (command_listener) {
                station = registerEndpointCommandSocket(endpoint_slot, client);
            } else {
                station = attachEndpointDataSocket(endpoint_slot, client);
            }

            if (!station) {
                closeSocket(client);
                continue;
            }

            if (command_listener) {
                std::thread(&LinkEmulator::commandSocketLoop, this, station).detach();
            } else {
                std::thread(&LinkEmulator::dataSocketLoop, this, station).detach();
            }
        }
    }

    std::shared_ptr<Station> registerEndpointCommandSocket(int endpoint_slot, socket_t cmd_socket) {
        std::shared_ptr<Station> station;
        {
            std::lock_guard<std::mutex> lock(stations_mutex_);
            station = endpointStationBySlotLocked(endpoint_slot);
            if (!station) {
                return nullptr;
            }
            if (station->cmd_socket != kInvalidSocket) {
                if (verbose_) {
                    std::cerr << "[LinkEmu] Endpoint " << (endpoint_slot + 1)
                              << " already has command socket; rejecting extra client\n";
                }
                return nullptr;
            }
            station->cmd_socket = cmd_socket;
        }
        if (verbose_) {
            std::cout << "[LinkEmu] Endpoint " << (endpoint_slot + 1)
                      << " command client attached as station #" << station->id << "\n";
        }
        return station;
    }

    std::shared_ptr<Station> attachEndpointDataSocket(int endpoint_slot, socket_t data_socket) {
        std::shared_ptr<Station> station;
        {
            std::lock_guard<std::mutex> lock(stations_mutex_);
            station = endpointStationBySlotLocked(endpoint_slot);
            if (!station) {
                return nullptr;
            }
            if (station->cmd_socket == kInvalidSocket || station->data_socket != kInvalidSocket) {
                if (verbose_) {
                    std::cerr << "[LinkEmu] Endpoint " << (endpoint_slot + 1)
                              << " cannot pair data socket yet; command socket missing or data already attached\n";
                }
                return nullptr;
            }
            station->data_socket = data_socket;
        }
        if (verbose_) {
            std::cout << "[LinkEmu] Endpoint " << (endpoint_slot + 1)
                      << " data client paired with station #" << station->id << "\n";
        }
        return station;
    }

    std::shared_ptr<Station> registerCommandStation(socket_t cmd_socket) {
        auto station = std::make_shared<Station>();
        {
            std::lock_guard<std::mutex> lock(stations_mutex_);
            station->id = next_station_id_++;
            station->cmd_socket = cmd_socket;
            station->route_deadline = std::chrono::steady_clock::now();
            stations_[station->id] = station;
            pending_data_attach_.push_back(station->id);
        }
        if (verbose_) {
            std::cout << "[LinkEmu] Command client attached as station #" << station->id << "\n";
        }
        return station;
    }

    std::shared_ptr<Station> attachDataSocket(socket_t data_socket) {
        std::shared_ptr<Station> chosen;
        {
            std::lock_guard<std::mutex> lock(stations_mutex_);
            while (!pending_data_attach_.empty()) {
                const int id = pending_data_attach_.front();
                pending_data_attach_.pop_front();
                auto it = stations_.find(id);
                if (it == stations_.end()) {
                    continue;
                }
                if (it->second->cmd_socket == kInvalidSocket ||
                    it->second->data_socket != kInvalidSocket) {
                    continue;
                }
                chosen = it->second;
                break;
            }

            if (!chosen) {
                int best_id = 0;
                for (auto& [id, station] : stations_) {
                    if (station->cmd_socket == kInvalidSocket ||
                        station->data_socket != kInvalidSocket) {
                        continue;
                    }
                    if (best_id == 0 || id < best_id) {
                        best_id = id;
                        chosen = station;
                    }
                }
            }

            if (chosen) {
                chosen->data_socket = data_socket;
            }
        }

        if (chosen && verbose_) {
            std::cout << "[LinkEmu] Data client paired with station #" << chosen->id << "\n";
        }
        return chosen;
    }

    void commandSocketLoop(const std::shared_ptr<Station>& station) {
        std::string buffer;
        buffer.reserve(2048);
        char chunk[1024];

        while (running_.load(std::memory_order_relaxed)) {
            const int n = recv(station->cmd_socket, chunk, sizeof(chunk), 0);
            if (n <= 0) {
                break;
            }
            buffer.append(chunk, static_cast<size_t>(n));

            size_t pos = 0;
            while ((pos = buffer.find_first_of("\r\n")) != std::string::npos) {
                std::string line = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);
                while (!buffer.empty() &&
                       (buffer.front() == '\r' || buffer.front() == '\n')) {
                    buffer.erase(0, 1);
                }
                if (!line.empty()) {
                    handleCommandLine(station, line);
                }
            }
        }
        onSocketClosed(station->id, true);
    }

    void dataSocketLoop(const std::shared_ptr<Station>& station) {
        std::vector<uint8_t> chunk(4096);
        while (running_.load(std::memory_order_relaxed)) {
            const int n = recv(station->data_socket,
                               reinterpret_cast<char*>(chunk.data()),
                               static_cast<int>(chunk.size()),
                               0);
            if (n <= 0) {
                break;
            }
            const size_t size = static_cast<size_t>(n);
            if (size == 0) {
                continue;
            }

            // The transport is a raw TCP stream. Under bursty sends, multiple
            // broadcast-style packets can coalesce into one recv(). Split on
            // the next likely broadcast marker so BEACON/CQ/BBS bursts are not
            // treated as a single payload blob.
            if (isBroadcastRoutedMarker(chunk[0])) {
                size_t start = 0;
                for (size_t i = 1; i < size; ++i) {
                    if (!isBroadcastRoutedMarker(chunk[i])) {
                        continue;
                    }
                    if (i > start) {
                        processDataChunk(
                            station,
                            std::vector<uint8_t>(chunk.begin() + start, chunk.begin() + i));
                    }
                    start = i;
                }
                if (start < size) {
                    processDataChunk(
                        station,
                        std::vector<uint8_t>(chunk.begin() + start, chunk.begin() + size));
                }
                continue;
            }

            processDataChunk(station, std::vector<uint8_t>(chunk.begin(), chunk.begin() + n));
        }
        onSocketClosed(station->id, false);
    }

    void onSocketClosed(int station_id, bool command_socket) {
        std::vector<OutboundLine> outbound;
        {
            std::lock_guard<std::mutex> lock(stations_mutex_);
            auto it = stations_.find(station_id);
            if (it == stations_.end()) {
                return;
            }
            auto station = it->second;

            if (command_socket) {
                closeSocket(station->cmd_socket);
                if (split_endpoint_mode_) {
                    // In split-endpoint mode, treat command drop as endpoint session loss.
                    closeSocket(station->data_socket);
                } else {
                    pending_data_attach_.erase(
                        std::remove(pending_data_attach_.begin(), pending_data_attach_.end(), station_id),
                        pending_data_attach_.end());
                }
            } else {
                closeSocket(station->data_socket);
            }

            if (station->peer_id != 0) {
                auto peer_it = stations_.find(station->peer_id);
                if (peer_it != stations_.end()) {
                    auto peer = peer_it->second;
                    peer->peer_id = 0;
                    outbound.push_back({peer, "EVT:DISCONNECTED normal"});
                    outbound.push_back({peer, "EVT:STATE LISTENING"});
                }
                station->peer_id = 0;
                outbound.push_back({station, "EVT:DISCONNECTED normal"});
                outbound.push_back({station, "EVT:STATE LISTENING"});
            }

            if (!split_endpoint_mode_ &&
                station->cmd_socket == kInvalidSocket &&
                station->data_socket == kInvalidSocket) {
                stations_.erase(it);
            }
        }
        flushOutboundLines(outbound);
    }

    void handleCommandLine(const std::shared_ptr<Station>& station, const std::string& raw_line) {
        const std::string line = trimSpaces(raw_line);
        if (line.empty()) {
            return;
        }
        if (verbose_) {
            std::cout << "[LinkEmu] #" << station->id << " CMD: " << line << "\n";
        }

        const size_t first_space = line.find(' ');
        const std::string cmd = upperAscii(line.substr(0, first_space));
        const std::string arg = (first_space == std::string::npos)
            ? std::string()
            : trimSpaces(line.substr(first_space + 1));

        std::vector<OutboundLine> outbound;

        if (cmd == "MYCALL") {
            std::lock_guard<std::mutex> lock(stations_mutex_);
            auto it = stations_.find(station->id);
            if (it != stations_.end()) {
                it->second->callsign = normalizeCallsign(arg);
                outbound.push_back({station, "MYCALL " + it->second->callsign});
            }
        } else if (cmd == "STATUS") {
            emitStateTelemetry(station, &outbound);
        } else if (cmd == "CONNECT") {
            outbound.push_back({station, "CONNECT " + normalizeCallsign(arg)});
            processConnectCommand(station, arg, &outbound);
        } else if (cmd == "DISCONNECT" || cmd == "ABORT") {
            outbound.push_back({station, cmd});
            processDisconnectCommand(station, &outbound);
        } else if (cmd == "STATE") {
            std::lock_guard<std::mutex> lock(stations_mutex_);
            auto it = stations_.find(station->id);
            const bool linked = it != stations_.end() && it->second->peer_id != 0;
            outbound.push_back({station,
                                std::string("STATE ") +
                                    (linked ? "CONNECTED" : "LISTENING")});
        } else if (cmd == "BEACON" || cmd == "CQ" || cmd == "BCAST") {
            {
                std::lock_guard<std::mutex> lock(stations_mutex_);
                auto it = stations_.find(station->id);
                if (it != stations_.end()) {
                    it->second->next_tx_route = NextTxRoute::Broadcast;
                    it->second->directed_target.clear();
                    it->second->route_deadline =
                        std::chrono::steady_clock::now() + std::chrono::seconds(2);
                }
            }
            outbound.push_back({station, cmd});
        } else if (cmd == "LISTEN" || cmd == "MONITOR" || cmd == "CWID") {
            outbound.push_back({station, cmd + " " + (arg.empty() ? "ON" : upperAscii(arg))});
        } else if (cmd == "FAMILY" || cmd == "MODE" || cmd == "MAXMODE" ||
                   cmd == "CPMODE" || cmd == "HARQ" || cmd == "CHANMEM" ||
                   cmd == "PROBE" || cmd == "MASKING" || cmd == "RESUME") {
            outbound.push_back({station, arg.empty() ? cmd : cmd + " " + upperAscii(arg)});
        } else if (cmd == "AUDIOLEVEL") {
            outbound.push_back({station, "AUDIOLEVEL RX=-45.0 TX=-120.0"});
        } else if (cmd == "NOISEFLOOR") {
            outbound.push_back({station, "NOISEFLOOR -95.0 BUSY=0"});
        } else if (cmd == "FREQINFO") {
            outbound.push_back({station, "FREQINFO CENTER=1500.0 SHIFT=0.0 CFO=0.0"});
        } else if (cmd == "VOICE" || cmd == "VOICECODEC" || cmd == "VOICESTART" ||
                   cmd == "VOICESTOP" || cmd == "VOICEPTT" || cmd == "VOICESTATUS") {
            // The emulator has no voice media path (documented in README).
            outbound.push_back({station, "ERROR:UNSUPPORTED"});
        } else if (cmd == "PING") {
            std::lock_guard<std::mutex> lock(stations_mutex_);
            auto it = stations_.find(station->id);
            if (it != stations_.end()) {
                it->second->next_tx_route = NextTxRoute::Directed;
                it->second->directed_target = normalizeCallsign(arg);
                it->second->route_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            }
        } else if (cmd == "CATFREQ") {
            char* end_ptr = nullptr;
            const unsigned long long hz = std::strtoull(arg.c_str(), &end_ptr, 10);
            if (end_ptr != nullptr && *end_ptr == '\0') {
                std::lock_guard<std::mutex> lock(stations_mutex_);
                auto it = stations_.find(station->id);
                if (it != stations_.end()) {
                    it->second->cat_frequency_hz = static_cast<uint64_t>(hz);
                }
            }
        } else if (cmd == "CATGETFREQ") {
            uint64_t hz = 7100000;
            {
                std::lock_guard<std::mutex> lock(stations_mutex_);
                auto it = stations_.find(station->id);
                if (it != stations_.end()) {
                    hz = it->second->cat_frequency_hz;
                }
            }
            outbound.push_back({station, std::to_string(hz)});
        } else if (cmd == "CATMODE") {
            std::lock_guard<std::mutex> lock(stations_mutex_);
            auto it = stations_.find(station->id);
            if (it != stations_.end() && !arg.empty()) {
                it->second->cat_mode = upperAscii(arg);
            }
        } else if (cmd == "CATGETMODE") {
            std::string mode = "USB";
            {
                std::lock_guard<std::mutex> lock(stations_mutex_);
                auto it = stations_.find(station->id);
                if (it != stations_.end()) {
                    mode = it->second->cat_mode;
                }
            }
            outbound.push_back({station, mode});
        } else if (cmd == "VERSION") {
            outbound.push_back({station, "VERSION SKYWAVE-LINK-EMU 1.0"});
        } else if (cmd == "BUFFER") {
            outbound.push_back({station, "BUFFER 0"});
        } else {
            // The modem distinguishes an unknown verb from a gated feature;
            // clients rely on that split (INTERFACES.md).
            outbound.push_back({station, "ERROR:UNKNOWN"});
        }

        flushOutboundLines(outbound);
    }

    void processConnectCommand(const std::shared_ptr<Station>& caller,
                               const std::string& target_raw,
                               std::vector<OutboundLine>* outbound) {
        if (outbound == nullptr) {
            return;
        }
        const std::string target_call = normalizeCallsign(target_raw);
        if (target_call.empty()) {
            return;
        }

        std::shared_ptr<Station> target;
        {
            std::lock_guard<std::mutex> lock(stations_mutex_);
            auto caller_it = stations_.find(caller->id);
            if (caller_it == stations_.end()) {
                return;
            }
            auto caller_station = caller_it->second;

            for (auto& [_, station] : stations_) {
                if (station->id == caller_station->id) {
                    continue;
                }
                if (station->cmd_socket == kInvalidSocket || station->data_socket == kInvalidSocket) {
                    continue;
                }
                if (normalizeCallsign(station->callsign) == target_call) {
                    target = station;
                    break;
                }
            }
            if (!target) {
                outbound->push_back({caller_station, "EVT:STATE LISTENING"});
                return;
            }

            disconnectLinkedPairLocked(caller_station, outbound);
            disconnectLinkedPairLocked(target, outbound);

            caller_station->peer_id = target->id;
            target->peer_id = caller_station->id;

            caller_station->next_tx_route = NextTxRoute::None;
            caller_station->directed_target.clear();
            target->next_tx_route = NextTxRoute::None;
            target->directed_target.clear();

            const std::string caller_call = caller_station->callsign.empty()
                ? ("STATION-" + std::to_string(caller_station->id))
                : caller_station->callsign;
            const std::string target_name = target->callsign.empty()
                ? ("STATION-" + std::to_string(target->id))
                : target->callsign;

            outbound->push_back({caller_station, "EVT:STATE CONNECTING"});
            outbound->push_back({target, "EVT:STATE CONNECTING"});
            outbound->push_back({caller_station, "EVT:CONNECTED " + target_name + " WIDE M8"});
            outbound->push_back({target, "EVT:CONNECTED " + caller_call + " WIDE M8"});
            outbound->push_back({caller_station, "EVT:STATE CONNECTED"});
            outbound->push_back({target, "EVT:STATE CONNECTED"});
        }
    }

    void processDisconnectCommand(const std::shared_ptr<Station>& station,
                                  std::vector<OutboundLine>* outbound) {
        if (outbound == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(stations_mutex_);
        auto it = stations_.find(station->id);
        if (it == stations_.end()) {
            return;
        }
        disconnectLinkedPairLocked(it->second, outbound);
        outbound->push_back({it->second, "EVT:STATE LISTENING"});
    }

    void emitStateTelemetry(const std::shared_ptr<Station>& station,
                            std::vector<OutboundLine>* outbound) {
        if (outbound == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(stations_mutex_);
        auto it = stations_.find(station->id);
        if (it == stations_.end()) {
            return;
        }
        auto st = it->second;
        const bool linked = st->peer_id != 0;
        std::string remote = "-";
        if (linked) {
            auto peer_it = stations_.find(st->peer_id);
            if (peer_it != stations_.end() && !peer_it->second->callsign.empty()) {
                remote = peer_it->second->callsign;
            }
        }
        // Same field set and order as the modem's STATUS line
        // (Modem/skywave/docs/INTERFACES.md).
        std::string status = "STATUS STATE=";
        status += linked ? "CONNECTED" : "LISTENING";
        status += " CALL=" + (st->callsign.empty() ? std::string("-") : st->callsign);
        status += " REMOTE=" + remote;
        status += " FAMILY=WIDE MODE=AUTO(M8) NET=1200.0";
        status += linked ? " SNR=-8.0 RSNR=-8.0" : " SNR=-20.0 RSNR=-";
        status += " CFO=0.0 RETRY=0.0 BUSY=0 LISTEN=1 COMPRESS=1 CRYPTO=0";
        status += " EPOCH=0 RESUME=1 MASK=0 VOICE=off TXBUF=0 FOK=0 FFAIL=0";
        outbound->push_back({st, status});
    }

    void disconnectLinkedPairLocked(const std::shared_ptr<Station>& station,
                                    std::vector<OutboundLine>* outbound) {
        if (outbound == nullptr || !station || station->peer_id == 0) {
            return;
        }
        auto peer_it = stations_.find(station->peer_id);
        std::shared_ptr<Station> peer = (peer_it != stations_.end()) ? peer_it->second : nullptr;

        station->peer_id = 0;
        if (peer) {
            peer->peer_id = 0;
            outbound->push_back({peer, "EVT:DISCONNECTED normal"});
            outbound->push_back({peer, "EVT:STATE LISTENING"});
        }
        outbound->push_back({station, "EVT:DISCONNECTED normal"});
        outbound->push_back({station, "EVT:STATE LISTENING"});
    }

    void processDataChunk(const std::shared_ptr<Station>& sender, const std::vector<uint8_t>& payload) {
        if (payload.empty()) {
            return;
        }

        const bool route_sensitive = isPotentialCommandRoutedMarker(payload.front());
        std::vector<TargetDataFrame> frames;
        constexpr int kRouteSyncAttempts = 3;
        constexpr auto kRouteSyncSleep = std::chrono::milliseconds(8);
        for (int attempt = 0; attempt < kRouteSyncAttempts; ++attempt) {
            bool retry_for_route_sync = false;
            {
                std::lock_guard<std::mutex> lock(stations_mutex_);
                auto it = stations_.find(sender->id);
                if (it == stations_.end()) {
                    return;
                }
                auto sender_station = it->second;
                const auto now = std::chrono::steady_clock::now();
                if (sender_station->next_tx_route != NextTxRoute::None &&
                    now > sender_station->route_deadline) {
                    sender_station->next_tx_route = NextTxRoute::None;
                    sender_station->directed_target.clear();
                }
                if (!sender_station->directed_reply_target.empty() &&
                    now > sender_station->directed_reply_deadline) {
                    sender_station->directed_reply_target.clear();
                }

                const std::string from = sender_station->callsign.empty()
                    ? ("STATION-" + std::to_string(sender_station->id))
                    : sender_station->callsign;

                if (attempt < (kRouteSyncAttempts - 1) &&
                    route_sensitive &&
                    sender_station->next_tx_route == NextTxRoute::None &&
                    sender_station->directed_reply_target.empty()) {
                    // BEACON/CQ/PING command and data can arrive on different sockets a few ms apart.
                    // Briefly wait so command-routed payloads are not dropped or misrouted.
                    retry_for_route_sync = true;
                } else if (sender_station->next_tx_route == NextTxRoute::Broadcast) {
                    sender_station->next_tx_route = NextTxRoute::None;
                    sender_station->directed_target.clear();
                    const std::vector<uint8_t> wrapped = buildWrappedFrame(from, payload);
                    for (auto& [id, station] : stations_) {
                        if (id == sender_station->id || station->data_socket == kInvalidSocket) {
                            continue;
                        }
                        frames.push_back({station, wrapped});
                    }
                } else {
                    std::string target_call;
                    if (sender_station->next_tx_route == NextTxRoute::Directed &&
                        !sender_station->directed_target.empty()) {
                        target_call = sender_station->directed_target;
                        sender_station->next_tx_route = NextTxRoute::None;
                        sender_station->directed_target.clear();
                    } else if (!sender_station->directed_reply_target.empty()) {
                        target_call = sender_station->directed_reply_target;
                        sender_station->directed_reply_target.clear();
                    }

                    if (!target_call.empty()) {
                        std::shared_ptr<Station> target;
                        for (auto& [_, station] : stations_) {
                            if (station->data_socket == kInvalidSocket) {
                                continue;
                            }
                            if (normalizeCallsign(station->callsign) == target_call) {
                                target = station;
                                break;
                            }
                        }
                        if (target) {
                            frames.push_back({target, buildWrappedFrame(from, payload)});
                            if (target->next_tx_route == NextTxRoute::None) {
                                target->directed_reply_target = normalizeCallsign(from);
                                target->directed_reply_deadline =
                                    now + std::chrono::seconds(3);
                            }
                        }
                    } else if (route_sensitive && isBroadcastRoutedMarker(payload.front())) {
                        // If the command arrived late and no explicit route was set, infer
                        // beacon/CQ class payloads as broadcast instead of dropping them.
                        const std::vector<uint8_t> wrapped = buildWrappedFrame(from, payload);
                        for (auto& [id, station] : stations_) {
                            if (id == sender_station->id || station->data_socket == kInvalidSocket) {
                                continue;
                            }
                            frames.push_back({station, wrapped});
                        }
                    } else if (sender_station->peer_id != 0) {
                        auto peer_it = stations_.find(sender_station->peer_id);
                        if (peer_it != stations_.end() && peer_it->second->data_socket != kInvalidSocket) {
                            frames.push_back({peer_it->second, payload});
                        }
                    }
                }
            }
            if (retry_for_route_sync) {
                std::this_thread::sleep_for(kRouteSyncSleep);
                continue;
            }
            break;
        }

        for (const auto& frame : frames) {
            sendDataFrame(frame.station, frame.payload);
        }
    }

    void flushOutboundLines(const std::vector<OutboundLine>& outbound) {
        for (const auto& line : outbound) {
            sendCommandLine(line.station, line.line);
        }
    }

    bool sendCommandLine(const std::shared_ptr<Station>& station, const std::string& line) {
        if (!station) {
            return false;
        }
        const std::string wire = line + "\r\n";
        std::lock_guard<std::mutex> write_lock(station->cmd_write_mutex);
        if (station->cmd_socket == kInvalidSocket) {
            return false;
        }
        return sendAll(station->cmd_socket,
                       reinterpret_cast<const uint8_t*>(wire.data()),
                       wire.size());
    }

    bool sendDataFrame(const std::shared_ptr<Station>& station, const std::vector<uint8_t>& payload) {
        if (!station || payload.empty()) {
            return false;
        }
        std::lock_guard<std::mutex> write_lock(station->data_write_mutex);
        if (station->data_socket == kInvalidSocket) {
            return false;
        }
        return sendAll(station->data_socket, payload.data(), payload.size());
    }

    std::string bind_host_;
    uint16_t cmd_port_ = 9101;
    uint16_t data_port_ = 9100;
    bool verbose_ = false;
    bool split_endpoint_mode_ = false;

    uint16_t endpoint_cmd_ports_[2] = {0, 0};
    uint16_t endpoint_data_ports_[2] = {0, 0};

    std::atomic<bool> running_{false};
    socket_t cmd_listener_ = kInvalidSocket;
    socket_t data_listener_ = kInvalidSocket;
    socket_t endpoint_cmd_listeners_[2] = {kInvalidSocket, kInvalidSocket};
    socket_t endpoint_data_listeners_[2] = {kInvalidSocket, kInvalidSocket};

    std::thread cmd_accept_thread_;
    std::thread data_accept_thread_;
    std::thread endpoint_cmd_accept_threads_[2];
    std::thread endpoint_data_accept_threads_[2];

    std::mutex stations_mutex_;
    std::unordered_map<int, std::shared_ptr<Station>> stations_;
    std::deque<int> pending_data_attach_;
    int endpoint_station_ids_[2] = {0, 0};
    int next_station_id_ = 1;
};

void printUsage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [--host <bind-host>] [--cmd-port <port>] [--data-port <port>] [--verbose]\n"
        << "   or: " << argv0
        << " [--host <bind-host>] --a-cmd-port <port> --a-data-port <port>"
        << " --b-cmd-port <port> --b-data-port <port> [--verbose]\n"
        << "Defaults:\n"
        << "  --host      127.0.0.1\n"
        << "  --cmd-port  9101\n"
        << "  --data-port 9100\n"
        << "Split-endpoint mode:\n"
        << "  Endpoint A/B use separate command+data ports and are bridged by the emulator.\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    uint16_t cmd_port = 9101;
    uint16_t data_port = 9100;
    bool cmd_port_set = false;
    bool data_port_set = false;

    uint16_t a_cmd_port = 0;
    uint16_t a_data_port = 0;
    uint16_t b_cmd_port = 0;
    uint16_t b_data_port = 0;
    bool a_cmd_set = false;
    bool a_data_set = false;
    bool b_cmd_set = false;
    bool b_data_set = false;

    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        }
        if (arg == "--verbose") {
            verbose = true;
            continue;
        }
        if ((arg == "--host" || arg == "--cmd-port" || arg == "--data-port" ||
             arg == "--a-cmd-port" || arg == "--a-data-port" ||
             arg == "--b-cmd-port" || arg == "--b-data-port") && i + 1 >= argc) {
            std::cerr << "Missing value for " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
        if (arg == "--host") {
            host = argv[++i];
            continue;
        }
        if (arg == "--cmd-port") {
            if (!parsePort(argv[++i], &cmd_port)) {
                std::cerr << "Invalid cmd port\n";
                return 1;
            }
            cmd_port_set = true;
            continue;
        }
        if (arg == "--data-port") {
            if (!parsePort(argv[++i], &data_port)) {
                std::cerr << "Invalid data port\n";
                return 1;
            }
            data_port_set = true;
            continue;
        }
        if (arg == "--a-cmd-port") {
            if (!parsePort(argv[++i], &a_cmd_port)) {
                std::cerr << "Invalid endpoint A cmd port\n";
                return 1;
            }
            a_cmd_set = true;
            continue;
        }
        if (arg == "--a-data-port") {
            if (!parsePort(argv[++i], &a_data_port)) {
                std::cerr << "Invalid endpoint A data port\n";
                return 1;
            }
            a_data_set = true;
            continue;
        }
        if (arg == "--b-cmd-port") {
            if (!parsePort(argv[++i], &b_cmd_port)) {
                std::cerr << "Invalid endpoint B cmd port\n";
                return 1;
            }
            b_cmd_set = true;
            continue;
        }
        if (arg == "--b-data-port") {
            if (!parsePort(argv[++i], &b_data_port)) {
                std::cerr << "Invalid endpoint B data port\n";
                return 1;
            }
            b_data_set = true;
            continue;
        }

        std::cerr << "Unknown argument: " << arg << "\n";
        printUsage(argv[0]);
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    const bool split_endpoint_mode =
        a_cmd_set || a_data_set || b_cmd_set || b_data_set;
    if (split_endpoint_mode) {
        if (!(a_cmd_set && a_data_set && b_cmd_set && b_data_set)) {
            std::cerr << "Split-endpoint mode requires all of: --a-cmd-port, --a-data-port, "
                         "--b-cmd-port, --b-data-port\n";
            return 1;
        }
        if (cmd_port_set || data_port_set) {
            std::cerr << "Use either shared-port mode (--cmd-port/--data-port) or split-endpoint mode, not both\n";
            return 1;
        }
        if (a_cmd_port == a_data_port || a_cmd_port == b_cmd_port || a_cmd_port == b_data_port ||
            a_data_port == b_cmd_port || a_data_port == b_data_port || b_cmd_port == b_data_port) {
            std::cerr << "Split-endpoint mode requires four distinct ports\n";
            return 1;
        }
    }

    std::unique_ptr<LinkEmulator> emulator;
    if (split_endpoint_mode) {
        emulator = std::make_unique<LinkEmulator>(
            host, a_cmd_port, a_data_port, b_cmd_port, b_data_port, verbose);
    } else {
        emulator = std::make_unique<LinkEmulator>(host, cmd_port, data_port, verbose);
    }

    if (!emulator->start()) {
        return 1;
    }

    std::cout << "[LinkEmu] Press Ctrl+C to stop\n";
    while (!g_stop_requested.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    emulator->stop();
    std::cout << "[LinkEmu] Stopped\n";
    return 0;
}
