// APRSClient - APRS-IS position reporting client
// Connects to APRS-IS network for position beaconing and queries

#pragma once

#include <memory>
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

// Outcome of a background login started with APRSClient::beginConnect().
enum class APRSConnectProgress { None, InProgress, Connected, Failed };

// Callback types
using PositionCallback = std::function<void(const APRSPosition& pos)>;
// An APRS message addressed to this station (acks/rejects are consumed;
// a numbered message has already been acknowledged when this runs).
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

    // Background connect + login for UI threads: DNS, the TCP connect
    // (with kConnectTimeoutMs) and the login exchange run on a helper
    // thread. Call pollConnect() from the main loop; it adopts the session
    // and reports Connected/Failed exactly once. A position queued with
    // sendPositionWhenConnected() goes out as soon as the login succeeds.
    bool beginConnect(const std::string& server, uint16_t port,
                      const std::string& callsign, const std::string& passcode);
    bool isConnecting() const { return connect_job_ != nullptr; }
    APRSConnectProgress pollConnect();
    void cancelConnect();
    void sendPositionWhenConnected() { send_position_on_connect_ = true; }
    // True once after pollConnect() sent a queued position.
    bool takeQueuedPositionSent() {
        const bool sent = queued_position_sent_;
        queued_position_sent_ = false;
        return sent;
    }
    const std::string& lastConnectError() const { return last_connect_error_; }
    static constexpr int kConnectTimeoutMs = 5000;
    void disconnect();
    bool isConnected() const;
    APRSState getState() const { return state_; }

    // Polling (call from main loop)
    void poll();

    // Position reporting. 0,0 (or an invalid coordinate) means "no fix":
    // nothing is beaconed and the default filter has no range part. While
    // connected with the default filter, a position change that alters the
    // filter text sends `#filter`.
    void setMyPosition(double lat, double lon, double alt_m = 0.0);
    bool hasMyPosition() const { return has_position_; }
    void setMyComment(const std::string& comment);
    void setMySymbol(APRSSymbol symbol);
    void setMySymbol(char table, char symbol);

    // Beaconing
    void setBeaconInterval(int seconds);
    bool sendPositionNow();
    void enableBeaconing(bool enabled);
    bool isBeaconingEnabled() const { return beacon_enabled_; }

    // Relay position for another station (HF gateway mode)
    bool sendPositionFor(const std::string& callsign, double lat, double lon,
                         const std::string& grid = "", const std::string& comment = "");

    // Queries: a directed `?APRSP` query (APRS 1.01 ch. 15), sent as a
    // message to `callsign`; its position comes back as a normal report.
    bool requestPosition(const std::string& callsign);
    // A numbered APRS message ("{n" suffix, so the recipient acks it).
    bool sendMessage(const std::string& to_callsign, const std::string& message);

    // Server-side filter. Without one, port 14580 sends nothing at all.
    // `filter` empty: defaultFilter() for this station. Applies to the next
    // login; while connected it is also sent at once as `#filter`.
    void setFilter(const std::string& filter);
    void setFilterRangeKm(int km);
    // The filter in effect for the current position / settings.
    std::string effectiveFilter() const;
    // `r/<lat>/<lon>/<km> b/<CALL>`, or `b/<CALL>` without a position.
    static std::string defaultFilter(const std::string& callsign, bool has_position,
                                     double lat, double lon, int range_km);

    // Callbacks
    void onPositionReceived(PositionCallback cb) { position_callback_ = std::move(cb); }
    void onMessageReceived(MessageCallback cb) { message_callback_ = std::move(cb); }
    void onStateChange(APRSStateCallback cb) { state_callback_ = std::move(cb); }

    // APRS passcode calculation (for registration)
    static int32_t calculatePasscode(const std::string& callsign);
    // The configured passcode when set, else the one computed for the
    // callsign (the single place both settings and login derive it);
    // "-1" (receive-only) without a callsign.
    static std::string effectivePasscode(const std::string& configured,
                                         const std::string& callsign);
    // The text of an APRS message line addressed to `my_callsign`
    // (`FROM>PATH::ADDRESSEE:text{id`): from, text and message id ("" when
    // unnumbered). Nullopt for anything else, including acks/rejects.
    struct ParsedMessage {
        std::string from;
        std::string text;
        std::string id;
    };
    static std::optional<ParsedMessage> parseMessageFor(const std::string& line,
                                                        const std::string& my_callsign);
    // Position report parser (uncompressed `!`, `=`, `/`, `@` formats);
    // nullopt for anything that is not one.
    static std::optional<APRSPosition> parsePositionReport(const std::string& line);

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

    bool has_position_ = false;
    std::string filter_;          // explicit filter; empty = default
    int filter_range_km_ = 100;
    std::string sent_filter_;     // filter the server has (login or #filter)
    int message_seq_ = 0;
    void refreshServerFilter();

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

    // Background connect state shared with the helper thread.
    struct ConnectJob;
    std::shared_ptr<ConnectJob> connect_job_;
    bool send_position_on_connect_ = false;
    bool queued_position_sent_ = false;
    std::string last_connect_error_;

    // Internal methods
    static bool openSocket(const std::string& server, uint16_t port, int timeout_ms,
                           socket_t* out, std::string* error);
    static void closeSocket(socket_t& sock);
    // Sends the login line and checks the server's answer (2 s). Returns
    // 1 when logged in, 0 when refused/invalid, -1 when the server closed.
    static int loginOnSocket(socket_t sock, const std::string& callsign,
                             const std::string& passcode, const std::string& filter,
                             std::string* error);
    bool sendPacket(const std::string& packet);
    void processLine(const std::string& line);
    void checkBeaconTimer();
};

} // namespace skylink
