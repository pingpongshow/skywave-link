// APRS-IS Client implementation

#include "skywave_link/aprs_client.hpp"
#include <iostream>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <cstdio>

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
    #include <errno.h>
    #include <poll.h>
#endif
#include <mutex>
#include <thread>

namespace skylink {

std::string APRSClient::sanitizeText(const std::string& text) {
    // APRS-IS is line oriented: a CR/LF (or any other control byte) in
    // free text would let the sender inject extra lines on our
    // authenticated session. Drop all control characters.
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7F) continue;
        out += c;
    }
    return out;
}

bool APRSClient::isValidCallsign(const std::string& callsign) {
    // Base call (letters/digits) with optional -SSID; max 9 chars on APRS-IS.
    if (callsign.empty() || callsign.size() > 9) return false;
    for (char c : callsign) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') return false;
    }
    return callsign.front() != '-' && callsign.back() != '-';
}

bool APRSClient::isValidCoordinate(double lat, double lon) {
    return std::isfinite(lat) && std::isfinite(lon) &&
           lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

APRSClient::APRSClient() = default;

APRSClient::~APRSClient() {
    disconnect();
}

void APRSClient::closeSocket(socket_t& sock) {
    if (sock != INVALID_SOCK) {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        sock = INVALID_SOCK;
    }
}

// DNS lookup plus a non-blocking connect with a deadline per address, so an
// unreachable server costs at most `timeout_ms` instead of the OS timeout.
// The returned socket is left non-blocking.
bool APRSClient::openSocket(const std::string& server, uint16_t port, int timeout_ms,
                            socket_t* out, std::string* error) {
    *out = INVALID_SOCK;
    struct addrinfo hints{}, *result = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port);
    if (getaddrinfo(server.c_str(), port_str.c_str(), &hints, &result) != 0) {
        if (error) *error = "Cannot resolve " + server;
        return false;
    }

    socket_t sock = INVALID_SOCK;
    for (auto* ai = result; ai != nullptr; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == INVALID_SOCK) {
            continue;
        }
        bool connected = false;
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(sock, FIONBIO, &mode);
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
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
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

    freeaddrinfo(result);
    if (sock == INVALID_SOCK) {
        if (error) *error = "Cannot connect to " + server + ":" + port_str;
        return false;
    }
    *out = sock;
    return true;
}

bool APRSClient::connect(const std::string& server, uint16_t port) {
    if (isConnected()) {
        disconnect();
    }
    cancelConnect();

    state_ = APRSState::Connecting;
    if (state_callback_) state_callback_(state_);

    if (!openSocket(server, port, kConnectTimeoutMs, &socket_, &last_connect_error_)) {
        state_ = APRSState::Error;
        if (state_callback_) state_callback_(state_);
        return false;
    }

    state_ = APRSState::Authenticating;
    if (state_callback_) state_callback_(state_);

    return true;
}

int APRSClient::loginOnSocket(socket_t sock, const std::string& callsign,
                              const std::string& passcode_in, const std::string& filter_in,
                              std::string* error) {
    std::string passcode = passcode_in;
    while (!passcode.empty() && std::isspace(static_cast<unsigned char>(passcode.back()))) passcode.pop_back();
    while (!passcode.empty() && std::isspace(static_cast<unsigned char>(passcode.front()))) passcode.erase(0, 1);
    if (sock == INVALID_SOCK) return 0;
    // Both go verbatim on the login line: callsign must be a plain callsign
    // and the passcode an integer (e.g. "12345" or "-1" for receive-only).
    bool passcode_ok = !passcode.empty() && passcode.size() <= 6;
    for (size_t i = 0; i < passcode.size() && passcode_ok; ++i) {
        const char c = passcode[i];
        passcode_ok = std::isdigit(static_cast<unsigned char>(c)) || (i == 0 && c == '-' && passcode.size() > 1);
    }
    if (!isValidCallsign(callsign) || !passcode_ok) {
        std::cerr << "[APRS] Invalid callsign/passcode for login\n";
        if (error) *error = "Invalid callsign/passcode for login";
        return 0;
    }

    // Login: user CALLSIGN pass PASSCODE vers SOFTWARE VERSION filter FILTER.
    // The filter is what makes port 14580 send anything back at all.
    const std::string filter = sanitizeText(filter_in);
    std::stringstream ss;
    ss << "user " << callsign << " pass " << passcode << " vers Skywave-Link " SKYWAVE_LINK_VERSION;
    if (!filter.empty()) ss << " filter " << filter;
    ss << "\r\n";
    const std::string login = ss.str();
#ifdef _WIN32
    const bool sent = send(sock, login.c_str(), static_cast<int>(login.size()), 0) ==
                      static_cast<int>(login.size());
#else
    const bool sent = send(sock, login.c_str(), login.size(), 0) ==
                      static_cast<ssize_t>(login.size());
#endif
    if (!sent) {
        std::cerr << "[APRS] send() failed for login\n";
        if (error) *error = "Login send failed";
        return 0;
    }

    // Read server response to verify authentication
    // Wait briefly for the response on non-blocking socket
    char resp_buf[512];
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sock, &readfds);
    struct timeval tv = {2, 0};  // 2 second timeout
    if (select(static_cast<int>(sock) + 1, &readfds, nullptr, nullptr, &tv) > 0) {
        int rn = static_cast<int>(recv(sock, resp_buf, sizeof(resp_buf) - 1, 0));
        if (rn == 0) {
            if (error) *error = "Server closed the connection";
            return -1;  // server closed the connection
        }
        if (rn > 0) {
            resp_buf[rn] = '\0';
            std::string response(resp_buf);
            if (response.find("unverified") != std::string::npos) {
                std::cerr << "[APRS] Authentication failed: unverified\n";
                if (error) *error = "Authentication failed: unverified";
                return 0;
            }
        }
    }
    return 1;
}

bool APRSClient::authenticate(const std::string& callsign, const std::string& passcode) {
    if (socket_ == INVALID_SOCK) return false;
    const std::string filter = defaultFilter(callsign, has_position_, my_position_.latitude,
                                             my_position_.longitude, filter_range_km_);
    const std::string effective = filter_.empty() ? filter : filter_;
    const int rc = loginOnSocket(socket_, callsign, passcode, effective, &last_connect_error_);
    if (rc < 0) {
        disconnect();  // server closed the connection
        return false;
    }
    if (rc == 0) {
        return false;
    }
    my_callsign_ = callsign;
    sent_filter_ = effective;
    state_ = APRSState::Connected;
    if (state_callback_) state_callback_(state_);
    return true;
}

// Shared by the helper thread and the client; the client may be destroyed
// (or the connect cancelled) while the helper still waits on the network.
struct APRSClient::ConnectJob {
    std::mutex mu;
    bool done = false;
    bool cancelled = false;
    bool ok = false;
    socket_t sock = INVALID_SOCK;
    std::string callsign;
    std::string filter;
    std::string error;
};

bool APRSClient::beginConnect(const std::string& server, uint16_t port,
                              const std::string& callsign, const std::string& passcode) {
    if (socket_ != INVALID_SOCK) {
        disconnect();
    }
    cancelConnect();
    last_connect_error_.clear();
    state_ = APRSState::Connecting;
    if (state_callback_) state_callback_(state_);

    auto job = std::make_shared<ConnectJob>();
    job->callsign = callsign;
    my_callsign_ = callsign;
    job->filter = effectiveFilter();
    connect_job_ = job;
    try {
        const std::string filter = job->filter;
        std::thread([job, server, port, callsign, passcode, filter]() {
            socket_t sock = INVALID_SOCK;
            std::string error;
            bool ok = openSocket(server, port, kConnectTimeoutMs, &sock, &error);
            if (ok && loginOnSocket(sock, callsign, passcode, filter, &error) != 1) {
                ok = false;
                closeSocket(sock);
            }
            std::lock_guard<std::mutex> lock(job->mu);
            if (job->cancelled) {
                closeSocket(sock);
            } else {
                job->ok = ok;
                job->sock = sock;
                job->error = error;
            }
            job->done = true;
        }).detach();
    } catch (const std::exception&) {
        connect_job_.reset();
        state_ = APRSState::Error;
        if (state_callback_) state_callback_(state_);
        return false;
    }
    return true;
}

APRSConnectProgress APRSClient::pollConnect() {
    if (!connect_job_) return APRSConnectProgress::None;
    std::shared_ptr<ConnectJob> job = connect_job_;
    {
        std::lock_guard<std::mutex> lock(job->mu);
        if (!job->done) return APRSConnectProgress::InProgress;
        connect_job_.reset();
        if (!job->ok) {
            last_connect_error_ = job->error;
        } else {
            socket_ = job->sock;
            job->sock = INVALID_SOCK;
            my_callsign_ = job->callsign;
            sent_filter_ = job->filter;
        }
    }
    if (socket_ == INVALID_SOCK) {
        send_position_on_connect_ = false;
        state_ = APRSState::Error;
        if (state_callback_) state_callback_(state_);
        return APRSConnectProgress::Failed;
    }
    state_ = APRSState::Connected;
    if (state_callback_) state_callback_(state_);
    // The position (or filter settings) may have changed during the login.
    refreshServerFilter();
    if (send_position_on_connect_) {
        send_position_on_connect_ = false;
        queued_position_sent_ = sendPositionNow();
    }
    return APRSConnectProgress::Connected;
}

void APRSClient::cancelConnect() {
    if (!connect_job_) return;
    {
        std::lock_guard<std::mutex> lock(connect_job_->mu);
        connect_job_->cancelled = true;
        closeSocket(connect_job_->sock);  // finished but never adopted
    }
    connect_job_.reset();
    send_position_on_connect_ = false;
}

void APRSClient::disconnect() {
    cancelConnect();
    if (socket_ != INVALID_SOCK) {
#ifdef _WIN32
        closesocket(socket_);
#else
        close(socket_);
#endif
        socket_ = INVALID_SOCK;
    }

    state_ = APRSState::Disconnected;
    if (state_callback_) state_callback_(state_);
}

bool APRSClient::isConnected() const {
    return socket_ != INVALID_SOCK && state_ == APRSState::Connected;
}

void APRSClient::poll() {
    if (socket_ == INVALID_SOCK) return;

    // Read available data
    char buffer[1024];
#ifdef _WIN32
    int n = recv(socket_, buffer, sizeof(buffer) - 1, 0);
    if (n == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        disconnect();
        return;
    }
#else
    ssize_t n = recv(socket_, buffer, sizeof(buffer) - 1, 0);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        disconnect();
        return;
    }
#endif
    if (n == 0) {
        // Orderly shutdown by the server: mark disconnected so the caller
        // reconnects instead of polling a dead socket forever.
        disconnect();
        return;
    }

    if (n > 0) {
        buffer[n] = '\0';
        rx_buffer_ += buffer;

        // Guard against unbounded buffer growth (no newlines from server)
        if (rx_buffer_.size() > 65536) {
            rx_buffer_.clear();
        }

        // Process complete lines
        size_t pos;
        while ((pos = rx_buffer_.find('\n')) != std::string::npos) {
            std::string line = rx_buffer_.substr(0, pos);
            rx_buffer_.erase(0, pos + 1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (!line.empty()) {
                processLine(line);
            }
        }
    }

    // Check beacon timer
    checkBeaconTimer();
}

void APRSClient::setMyPosition(double lat, double lon, double alt_m) {
    my_position_.latitude = lat;
    my_position_.longitude = lon;
    my_position_.altitude_m = alt_m;
    has_position_ = isValidCoordinate(lat, lon) && !(lat == 0.0 && lon == 0.0);
    refreshServerFilter();
}

std::string APRSClient::defaultFilter(const std::string& callsign, bool has_position,
                                      double lat, double lon, int range_km) {
    std::string out;
    if (has_position && isValidCoordinate(lat, lon)) {
        // One decimal (~11 km): fine enough for a range filter and stable
        // enough that GPS jitter does not resend it.
        char buf[96];
        std::snprintf(buf, sizeof(buf), "r/%.1f/%.1f/%d", lat, lon,
                      std::clamp(range_km, 1, 5000));
        out = buf;
    }
    std::string call = sanitizeText(callsign);
    for (char& c : call) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (isValidCallsign(call)) {
        if (!out.empty()) out += " ";
        out += "b/" + call;
    }
    return out;
}

std::string APRSClient::effectiveFilter() const {
    if (!filter_.empty()) return filter_;
    return defaultFilter(my_callsign_, has_position_, my_position_.latitude,
                         my_position_.longitude, filter_range_km_);
}

void APRSClient::refreshServerFilter() {
    if (!isConnected()) return;
    const std::string want = effectiveFilter();
    if (want.empty() || want == sent_filter_) return;
    if (sendPacket("#filter " + sanitizeText(want) + "\r\n")) {
        sent_filter_ = want;
    }
}

void APRSClient::setFilterRangeKm(int km) {
    filter_range_km_ = std::clamp(km, 1, 5000);
    refreshServerFilter();
}

void APRSClient::setMyComment(const std::string& comment) {
    my_position_.comment = comment;
}

void APRSClient::setMySymbol(APRSSymbol symbol) {
    my_position_.symbol = symbol;
}

void APRSClient::setMySymbol(char table, char symbol) {
    my_position_.symbol.table = table;
    my_position_.symbol.symbol = symbol;
}

void APRSClient::setBeaconInterval(int seconds) {
    beacon_interval_ = seconds;
}

bool APRSClient::sendPositionNow() {
    if (!isConnected()) return false;
    // No fix (0,0 or invalid): never beacon a made-up position.
    if (!has_position_) return false;

    std::string packet = my_callsign_ + ">APRS,TCPIP*:" +
                         formatPosition(my_position_) + "\r\n";
    bool ok = sendPacket(packet);
    last_beacon_ = std::chrono::steady_clock::now();
    return ok;
}

bool APRSClient::sendPositionFor(const std::string& callsign, double lat, double lon,
                                  const std::string& grid, const std::string& comment) {
    if (!isConnected()) return false;
    // Callsign comes from the radio packet: it goes into the APRS header, so
    // it must be a plain callsign. Position must be finite and in range.
    if (!isValidCallsign(callsign) || !isValidCoordinate(lat, lon)) return false;

    // Build position for another station (HF gateway relay)
    // Third-party format: GATEWAY>APRS,TCPIP*:}ORIGCALL>APRS,TCPIP,GATEWAY*:=position
    APRSPosition pos;
    pos.callsign = callsign;
    pos.latitude = lat;
    pos.longitude = lon;
    pos.symbol = Symbols::HFGateway;  // HF gateway symbol
    std::string relayed_comment = sanitizeText(comment);
    const std::string safe_grid = sanitizeText(grid);
    if (!safe_grid.empty()) {
        if (!relayed_comment.empty()) {
            relayed_comment += " ";
        }
        relayed_comment += "QTH " + safe_grid;
    }
    pos.comment = relayed_comment.empty()
        ? ("via " + my_callsign_ + " HF")
        : (relayed_comment + " via " + my_callsign_);

    // Format as third-party traffic (allows originating station to be identified)
    std::stringstream ss;
    ss << my_callsign_ << ">APRS,TCPIP*:}" << callsign << ">APRS,TCPIP," << my_callsign_ << "*:"
       << formatPosition(pos) << "\r\n";

    std::string packet = ss.str();
    return sendPacket(packet);
}

void APRSClient::enableBeaconing(bool enabled) {
    beacon_enabled_ = enabled;
    if (enabled) {
        last_beacon_ = std::chrono::steady_clock::now();
    }
}

bool APRSClient::requestPosition(const std::string& callsign) {
    if (!isConnected() || !isValidCallsign(callsign)) return false;
    // Directed station query: a message whose text is "?APRSP".
    std::stringstream ss;
    ss << my_callsign_ << ">APRS,TCPIP*::" << std::setw(9) << std::left << callsign
       << ":?APRSP\r\n";
    return sendPacket(ss.str());
}

bool APRSClient::sendMessage(const std::string& to_callsign, const std::string& message) {
    if (!isConnected() || !isValidCallsign(to_callsign)) return false;
    // Format: FROMCALL>APRS,TCPIP*::TOCALL   :text{n   (text max 67 chars,
    // no '|', '~' or '{').
    std::string text = sanitizeText(message);
    text.erase(std::remove_if(text.begin(), text.end(),
                              [](char c) { return c == '|' || c == '~' || c == '{'; }),
               text.end());
    if (text.size() > 67) text.resize(67);
    message_seq_ = message_seq_ % 99999 + 1;
    std::stringstream ss;
    ss << my_callsign_ << ">APRS,TCPIP*::" << std::setw(9) << std::left << to_callsign
       << ":" << text << "{" << message_seq_ << "\r\n";
    return sendPacket(ss.str());
}

void APRSClient::setFilter(const std::string& filter) {
    filter_ = sanitizeText(filter);
    refreshServerFilter();
}

std::string APRSClient::effectivePasscode(const std::string& configured,
                                         const std::string& callsign) {
    std::string trimmed = configured;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) trimmed.pop_back();
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) trimmed.erase(0, 1);
    if (!trimmed.empty()) return trimmed;
    // No callsign: "-1" logs in receive-only.
    if (callsign.find_first_not_of(" \t\r\n") == std::string::npos) return "-1";
    return std::to_string(calculatePasscode(callsign));
}

int32_t APRSClient::calculatePasscode(const std::string& callsign) {
    // APRS passcode algorithm
    std::string call = callsign;

    // Remove SSID
    size_t dash = call.find('-');
    if (dash != std::string::npos) {
        call = call.substr(0, dash);
    }

    // Convert to uppercase
    for (char& c : call) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    int32_t hash = 0x73e2;  // Initial hash value

    for (size_t i = 0; i < call.length(); i += 2) {
        hash ^= static_cast<int32_t>(call[i]) << 8;
        if (i + 1 < call.length()) {
            hash ^= static_cast<int32_t>(call[i + 1]);
        }
    }

    return hash & 0x7FFF;  // Only positive values
}

std::string APRSClient::formatLatitude(double lat) {
    // Format: DDMM.hhN/S
    if (!std::isfinite(lat)) lat = 0.0;
    lat = std::clamp(lat, -90.0, 90.0);
    char ns = lat >= 0 ? 'N' : 'S';
    // Round to hundredths of a minute first so 59.999' never prints as 60.00
    long hundredths = std::lround(std::abs(lat) * 6000.0);
    int degrees = static_cast<int>(hundredths / 6000);
    double minutes = static_cast<double>(hundredths % 6000) / 100.0;

    std::stringstream ss;
    ss << std::setfill('0') << std::setw(2) << degrees
       << std::fixed << std::setprecision(2) << std::setw(5) << minutes
       << ns;
    return ss.str();
}

std::string APRSClient::formatLongitude(double lon) {
    // Format: DDDMM.hhE/W
    if (!std::isfinite(lon)) lon = 0.0;
    lon = std::clamp(lon, -180.0, 180.0);
    char ew = lon >= 0 ? 'E' : 'W';
    long hundredths = std::lround(std::abs(lon) * 6000.0);
    int degrees = static_cast<int>(hundredths / 6000);
    double minutes = static_cast<double>(hundredths % 6000) / 100.0;

    std::stringstream ss;
    ss << std::setfill('0') << std::setw(3) << degrees
       << std::fixed << std::setprecision(2) << std::setw(5) << minutes
       << ew;
    return ss.str();
}

std::string APRSClient::formatPosition(const APRSPosition& pos) {
    // Format: =DDMM.hhN/DDDMM.hhWSymbol Comment
    auto printable = [](char c, char fallback) {
        const unsigned char uc = static_cast<unsigned char>(c);
        return (uc < 0x21 || uc > 0x7E) ? fallback : c;
    };
    std::stringstream ss;
    ss << "=" << formatLatitude(pos.latitude)
       << printable(pos.symbol.table, '/') << formatLongitude(pos.longitude)
       << printable(pos.symbol.symbol, '-');

    const std::string comment = sanitizeText(pos.comment);
    if (!comment.empty()) {
        ss << " " << comment;
    }

    return ss.str();
}

bool APRSClient::sendPacket(const std::string& packet) {
    if (socket_ == INVALID_SOCK) return false;

#ifdef _WIN32
    int sent = send(socket_, packet.c_str(), static_cast<int>(packet.size()), 0);
    if (sent != static_cast<int>(packet.size())) {
        std::cerr << "[APRS] send() failed: sent=" << sent
                  << " expected=" << static_cast<int>(packet.size())
                  << " wsa=" << WSAGetLastError() << "\n";
        return false;
    }
    return true;
#else
    ssize_t sent = send(socket_, packet.c_str(), packet.size(), 0);
    if (sent != static_cast<ssize_t>(packet.size())) {
        std::cerr << "[APRS] send() failed: sent=" << sent
                  << " expected=" << static_cast<ssize_t>(packet.size())
                  << " errno=" << errno << "\n";
        return false;
    }
    return true;
#endif
}

namespace {

// "FROM>TO,PATH:info" -> FROM and info; false for anything else.
bool splitPacket(const std::string& line, std::string* from, std::string* info) {
    const size_t arrow = line.find('>');
    if (arrow == std::string::npos || arrow == 0) return false;
    const size_t colon = line.find(':', arrow);
    if (colon == std::string::npos) return false;
    *from = line.substr(0, arrow);
    *info = line.substr(colon + 1);
    return true;
}

std::string upperTrim(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// "DDMM.hh" + N/S (or "DDDMM.hh" + E/W) with all-digit fields.
bool parseCoord(const std::string& text, int deg_digits, char pos_hemi, char neg_hemi,
                double max_deg, double* out) {
    const size_t len = static_cast<size_t>(deg_digits) + 6;  // DD MM . hh H
    if (text.size() != len) return false;
    for (size_t i = 0; i + 1 < len; ++i) {
        if (i == static_cast<size_t>(deg_digits) + 2) {
            if (text[i] != '.') return false;
        } else if (!std::isdigit(static_cast<unsigned char>(text[i]))) {
            return false;
        }
    }
    const char hemi = text[len - 1];
    if (hemi != pos_hemi && hemi != neg_hemi) return false;
    const double deg = std::stod(text.substr(0, static_cast<size_t>(deg_digits)));
    const double min = std::stod(text.substr(static_cast<size_t>(deg_digits), 5));
    if (min >= 60.0) return false;
    double v = deg + min / 60.0;
    if (v > max_deg) return false;
    *out = (hemi == neg_hemi) ? -v : v;
    return true;
}

} // namespace

std::optional<APRSPosition> APRSClient::parsePositionReport(const std::string& line) {
    std::string from, data;
    if (!splitPacket(line, &from, &data) || data.empty()) return std::nullopt;
    size_t off = 0;
    switch (data[0]) {
        case '!': case '=': off = 1; break;
        case '/': case '@': off = 8; break;  // 7-character timestamp
        default: return std::nullopt;        // not a position report
    }
    if (data.size() < off + 19) return std::nullopt;  // lat(8) table lon(9) symbol
    APRSPosition pos;
    try {
        if (!parseCoord(data.substr(off, 8), 2, 'N', 'S', 90.0, &pos.latitude) ||
            !parseCoord(data.substr(off + 9, 9), 3, 'E', 'W', 180.0, &pos.longitude)) {
            return std::nullopt;  // compressed or malformed
        }
    } catch (...) {
        return std::nullopt;
    }
    pos.callsign = from;
    pos.symbol.table = data[off + 8];
    pos.symbol.symbol = data[off + 18];
    pos.comment = sanitizeText(data.substr(off + 19));
    pos.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return pos;
}

std::optional<APRSClient::ParsedMessage> APRSClient::parseMessageFor(
    const std::string& line, const std::string& my_callsign) {
    std::string from, info;
    if (!splitPacket(line, &from, &info)) return std::nullopt;
    // ":ADDRESSEE:text" with a 9-character, space-padded addressee.
    if (info.size() < 11 || info[0] != ':' || info[10] != ':') return std::nullopt;
    if (upperTrim(info.substr(1, 9)) != upperTrim(my_callsign) || my_callsign.empty()) {
        return std::nullopt;
    }
    std::string text = info.substr(11);
    // Acks and rejects ("ack12", "rej12") are protocol, not messages.
    if ((text.rfind("ack", 0) == 0 || text.rfind("rej", 0) == 0) && text.size() > 3 &&
        text.find(' ') == std::string::npos) {
        return std::nullopt;
    }
    ParsedMessage msg;
    msg.from = from;
    const size_t brace = text.find('{');
    if (brace != std::string::npos) {
        std::string id = text.substr(brace + 1);
        const size_t close = id.find('}');  // reply-ack form "{id}ack"
        if (close != std::string::npos) id.resize(close);
        if (id.size() > 5) id.resize(5);
        msg.id = sanitizeText(id);
        text.resize(brace);
    }
    msg.text = sanitizeText(text);
    return msg;
}

void APRSClient::processLine(const std::string& line) {
    // Server comments (logresp, keepalives) start with '#'.
    if (line.empty() || line[0] == '#') {
        return;
    }

    if (auto pos = parsePositionReport(line)) {
        if (position_callback_) position_callback_(*pos);
        return;
    }

    if (auto msg = parseMessageFor(line, my_callsign_)) {
        if (!msg->id.empty() && isValidCallsign(msg->from)) {
            // Numbered messages are acknowledged, or the sender retries.
            std::stringstream ss;
            ss << my_callsign_ << ">APRS,TCPIP*::" << std::setw(9) << std::left << msg->from
               << ":ack" << msg->id << "\r\n";
            sendPacket(ss.str());
        }
        if (message_callback_) message_callback_(msg->from, msg->text);
    }
}

void APRSClient::checkBeaconTimer() {
    if (!beacon_enabled_ || !isConnected()) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_beacon_);

    if (elapsed.count() >= beacon_interval_) {
        sendPositionNow();
    }
}

} // namespace skylink
