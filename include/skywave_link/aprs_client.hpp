// APRSClient - APRS-IS position reporting client
// Connects to APRS-IS network for position beaconing and queries

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <cstdint>
#include <chrono>

namespace skylink {

// APRS symbol table and symbol
struct APRSSymbol {
    char table = '/';    // Primary table '/' or alternate '\\'
    char symbol = '-';   // Symbol character (e.g., '>' for car, '-' for house)
};

// Common APRS symbols
namespace Symbols {
    constexpr APRSSymbol House = {'/', '-'};
    constexpr APRSSymbol Car = {'/', '>'};
    constexpr APRSSymbol Boat = {'/', 's'};
    constexpr APRSSymbol Aircraft = {'/', '\''};
    constexpr APRSSymbol Bicycle = {'/', 'b'};
    constexpr APRSSymbol Jogger = {'/', '['};
    constexpr APRSSymbol Motorcycle = {'/', '<'};
    constexpr APRSSymbol EmergencyVehicle = {'/', '!'};
    constexpr APRSSymbol HFGateway = {'/', '&'};
    constexpr APRSSymbol Portable = {'/', '#'};
}

// Position data
struct APRSPosition {
    std::string callsign;
    double latitude = 0.0;      // Decimal degrees, positive = North
    double longitude = 0.0;     // Decimal degrees, positive = East
    double altitude_m = 0.0;    // Meters above sea level
    double speed_kmh = 0.0;     // Speed in km/h
    double course_deg = 0.0;    // Course in degrees (0-360)
    std::string comment;
    APRSSymbol symbol;
    int64_t timestamp = 0;      // Unix timestamp
};

// Connection state
enum class APRSState {
    Disconnected,
    Connecting,
    Authenticating,
    Connected,
    Error
};

// Callback types
using PositionCallback = std::function<void(const APRSPosition& pos)>;
using MessageCallback = std::function<void(const std::string& from, const std::string& message)>;
using APRSStateCallback = std::function<void(APRSState state)>;

class APRSClient {
public:
    APRSClient();
    ~APRSClient();

    // Non-copyable
    APRSClient(const APRSClient&) = delete;
    APRSClient& operator=(const APRSClient&) = delete;

    // Connection management
    bool connect(const std::string& server = "rotate.aprs.net", uint16_t port = 14580);
    bool authenticate(const std::string& callsign, const std::string& passcode);
    void disconnect();
    bool isConnected() const;
    APRSState getState() const { return state_; }

    // Polling (call from main loop)
    void poll();

    // Position reporting
    void setMyPosition(double lat, double lon, double alt_m = 0.0);
    void setMyCourse(double speed_kmh, double course_deg);
    void setMyComment(const std::string& comment);
    void setMySymbol(APRSSymbol symbol);
    void setMySymbol(char table, char symbol);

    // Beaconing
    void setBeaconInterval(int seconds);
    int getBeaconInterval() const { return beacon_interval_; }
    bool sendPositionNow();
    void enableBeaconing(bool enabled);
    bool isBeaconingEnabled() const { return beacon_enabled_; }

    // Relay position for another station (HF gateway mode)
    bool sendPositionFor(const std::string& callsign, double lat, double lon,
                         const std::string& grid = "", const std::string& comment = "");

    // Queries
    void requestPosition(const std::string& callsign);
    void sendMessage(const std::string& to_callsign, const std::string& message);

    // Filter configuration
    void setFilter(const std::string& filter);  // e.g., "r/lat/lon/range"

    // Callbacks
    void onPositionReceived(PositionCallback cb) { position_callback_ = std::move(cb); }
    void onMessageReceived(MessageCallback cb) { message_callback_ = std::move(cb); }
    void onStateChange(APRSStateCallback cb) { state_callback_ = std::move(cb); }

    // APRS passcode calculation (for registration)
    static int32_t calculatePasscode(const std::string& callsign);

    // Format helpers
    static std::string formatLatitude(double lat);   // DDMM.hhN
    static std::string formatLongitude(double lon);  // DDDMM.hhW
    static std::string formatPosition(const APRSPosition& pos);

    // Input hardening (exposed for testing). Text from radio packets must go
    // through sanitizeText before reaching the APRS-IS line protocol.
    static std::string sanitizeText(const std::string& text);  // strips CR/LF/control chars
    static bool isValidCallsign(const std::string& callsign);   // [A-Za-z0-9-]{1,9}
    static bool isValidCoordinate(double lat, double lon);      // finite, in range

private:
    // Socket handle
#ifdef _WIN32
    using socket_t = unsigned long long;
    static constexpr socket_t INVALID_SOCK = ~0ULL;
#else
    using socket_t = int;
    static constexpr socket_t INVALID_SOCK = -1;
#endif

    socket_t socket_ = INVALID_SOCK;
    APRSState state_ = APRSState::Disconnected;

    // My station info
    std::string my_callsign_;
    APRSPosition my_position_;

    // Beacon configuration
    int beacon_interval_ = 900;  // 15 minutes default
    bool beacon_enabled_ = false;
    std::chrono::steady_clock::time_point last_beacon_;

    // Receive buffer
    std::string rx_buffer_;

    // Callbacks
    PositionCallback position_callback_;
    MessageCallback message_callback_;
    APRSStateCallback state_callback_;

    // Internal methods
    bool sendPacket(const std::string& packet);
    void processLine(const std::string& line);
    APRSPosition parsePosition(const std::string& packet);
    void checkBeaconTimer();
};

} // namespace skylink
