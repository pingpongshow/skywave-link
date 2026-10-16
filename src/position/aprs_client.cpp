// APRS-IS Client implementation

#include "skywave_link/aprs_client.hpp"
#include <iostream>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <cctype>

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
#endif

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

bool APRSClient::connect(const std::string& server, uint16_t port) {
    if (isConnected()) {
        disconnect();
    }

    state_ = APRSState::Connecting;
    if (state_callback_) state_callback_(state_);

    struct addrinfo hints{}, *result = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port);
    if (getaddrinfo(server.c_str(), port_str.c_str(), &hints, &result) != 0) {
        state_ = APRSState::Error;
        if (state_callback_) state_callback_(state_);
        return false;
    }

    bool connected = false;
    for (auto* ai = result; ai != nullptr; ai = ai->ai_next) {
        socket_ = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (socket_ == INVALID_SOCK) {
            continue;
        }
        if (::connect(socket_, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            connected = true;
            break;
        }
#ifdef _WIN32
        closesocket(socket_);
#else
        close(socket_);
#endif
        socket_ = INVALID_SOCK;
    }

    freeaddrinfo(result);
    if (!connected || socket_ == INVALID_SOCK) {
        state_ = APRSState::Error;
        if (state_callback_) state_callback_(state_);
        return false;
    }

    // Set non-blocking
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(socket_, FIONBIO, &mode);
#else
    int flags = fcntl(socket_, F_GETFL, 0);
    fcntl(socket_, F_SETFL, flags | O_NONBLOCK);
#endif

    state_ = APRSState::Authenticating;
    if (state_callback_) state_callback_(state_);

    return true;
}

bool APRSClient::authenticate(const std::string& callsign, const std::string& passcode_in) {
    std::string passcode = passcode_in;
    while (!passcode.empty() && std::isspace(static_cast<unsigned char>(passcode.back()))) passcode.pop_back();
    while (!passcode.empty() && std::isspace(static_cast<unsigned char>(passcode.front()))) passcode.erase(0, 1);
    if (socket_ == INVALID_SOCK) return false;
    // Both go verbatim on the login line: callsign must be a plain callsign
    // and the passcode an integer (e.g. "12345" or "-1" for receive-only).
    bool passcode_ok = !passcode.empty() && passcode.size() <= 6;
    for (size_t i = 0; i < passcode.size() && passcode_ok; ++i) {
        const char c = passcode[i];
        passcode_ok = std::isdigit(static_cast<unsigned char>(c)) || (i == 0 && c == '-' && passcode.size() > 1);
    }
    if (!isValidCallsign(callsign) || !passcode_ok) {
        std::cerr << "[APRS] Invalid callsign/passcode for login\n";
        return false;
    }

    my_callsign_ = callsign;

    // Build login string: user CALLSIGN pass PASSCODE vers SOFTWARE VERSION filter FILTER
    std::stringstream ss;
    ss << "user " << callsign << " pass " << passcode
       << " vers Skywave-Link 1.0.0\r\n";

    if (!sendPacket(ss.str())) {
        return false;
    }

    // Read server response to verify authentication
    // Wait briefly for the response on non-blocking socket
    char resp_buf[512];
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(socket_, &readfds);
    struct timeval tv = {2, 0};  // 2 second timeout
    if (select(static_cast<int>(socket_) + 1, &readfds, nullptr, nullptr, &tv) > 0) {
        int rn = recv(socket_, resp_buf, sizeof(resp_buf) - 1, 0);
        if (rn == 0) {
            disconnect();  // server closed the connection
            return false;
        }
        if (rn > 0) {
            resp_buf[rn] = '\0';
            std::string response(resp_buf);
            if (response.find("unverified") != std::string::npos) {
                std::cerr << "[APRS] Authentication failed: unverified\n";
                return false;
            }
        }
    }

    state_ = APRSState::Connected;
    if (state_callback_) state_callback_(state_);
    return true;
}

void APRSClient::disconnect() {
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
}

void APRSClient::setMyCourse(double speed_kmh, double course_deg) {
    my_position_.speed_kmh = speed_kmh;
    my_position_.course_deg = course_deg;
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
    if (!isValidCoordinate(my_position_.latitude, my_position_.longitude)) return false;

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

void APRSClient::requestPosition(const std::string& callsign) {
    if (!isConnected()) return;
    if (!isValidCallsign(callsign)) return;

    std::string packet = my_callsign_ + ">APRS,TCPIP*:?" + callsign + "\r\n";
    sendPacket(packet);
}

void APRSClient::sendMessage(const std::string& to_callsign, const std::string& message) {
    if (!isConnected()) return;
    if (!isValidCallsign(to_callsign)) return;

    // Format: FROMCALL>APRS::TOCALL   :message{seq
    std::stringstream ss;
    ss << my_callsign_ << ">APRS,TCPIP*::" << std::setw(9) << std::left << to_callsign
       << ":" << sanitizeText(message) << "\r\n";
    sendPacket(ss.str());
}

void APRSClient::setFilter(const std::string& filter) {
    if (!isConnected()) return;

    std::string packet = "#filter " + sanitizeText(filter) + "\r\n";
    sendPacket(packet);
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

void APRSClient::processLine(const std::string& line) {
    // Skip server messages (start with #)
    if (!line.empty() && line[0] == '#') {
        return;
    }

    // Try to parse as position report
    APRSPosition pos = parsePosition(line);
    if (!pos.callsign.empty()) {
        if (position_callback_) {
            position_callback_(pos);
        }
    }

    // Check for messages
    // Format: FROMCALL>...:MSG:message{seq
    size_t msg_start = line.find("::");
    if (msg_start != std::string::npos) {
        // Extract sender
        size_t arrow = line.find('>');
        if (arrow != std::string::npos) {
            std::string from = line.substr(0, arrow);
            std::string msg_part = line.substr(msg_start + 2);

            // Skip addressee (9 chars)
            if (msg_part.length() > 10) {
                std::string message = msg_part.substr(10);

                // Remove message ID
                size_t brace = message.find('{');
                if (brace != std::string::npos) {
                    message = message.substr(0, brace);
                }

                if (message_callback_) {
                    message_callback_(from, message);
                }
            }
        }
    }
}

APRSPosition APRSClient::parsePosition(const std::string& packet) {
    APRSPosition pos;

    // Extract callsign
    size_t arrow = packet.find('>');
    if (arrow == std::string::npos) return pos;
    pos.callsign = packet.substr(0, arrow);

    // Find position data (after last :)
    size_t colon = packet.rfind(':');
    if (colon == std::string::npos || colon + 1 >= packet.length()) return pos;

    std::string data = packet.substr(colon + 1);

    // Check for position format markers
    char marker = data[0];
    if (marker != '!' && marker != '=' && marker != '/' && marker != '@') {
        return pos;  // Not a position packet
    }

    // Simple parsing - just extract lat/lon
    // Full implementation would handle all APRS formats
    if (data.length() < 20) return pos;

    // Parse latitude: DDMM.hhN
    try {
        int lat_deg = std::stoi(data.substr(1, 2));
        double lat_min = std::stod(data.substr(3, 5));
        char lat_ns = data[8];
        pos.latitude = lat_deg + lat_min / 60.0;
        if (lat_ns == 'S') pos.latitude = -pos.latitude;

        // Parse longitude: DDDMM.hhW (after symbol table char)
        int lon_deg = std::stoi(data.substr(10, 3));
        double lon_min = std::stod(data.substr(13, 5));
        char lon_ew = data[18];
        pos.longitude = lon_deg + lon_min / 60.0;
        if (lon_ew == 'W') pos.longitude = -pos.longitude;

        // Symbol
        pos.symbol.table = data[9];
        pos.symbol.symbol = data[19];

        // Comment (rest of packet)
        if (data.length() > 20) {
            pos.comment = data.substr(20);
        }

        pos.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    } catch (...) {
        // Parsing failed
        pos.callsign.clear();
    }

    return pos;
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
