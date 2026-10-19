// Configuration management for Skywave Link

#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace skylink {

// Station configuration
struct StationConfig {
    std::string callsign;
    std::string grid_square;        // Maidenhead locator
    std::string name;               // Operator name
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude_m = 0.0;
};

// Modem connection configuration
struct ModemConfig {
    std::string host = "127.0.0.1";
    // Skywave defaults (INTERFACES.md).
    uint16_t command_port = 9101;
    uint16_t data_port = 9100;
    uint16_t voice_port = 9102;
    // Bandwidth family: WIDE | MID | NARROW | ULTRA (WAVEFORM.md).
    std::string family = "WIDE";
    bool auto_connect = true;
    int reconnect_interval_sec = 5;
};

struct VoiceConfig {
    bool auto_start = false;
    // Codec2 rate: AUTO | 700C | 1300 | 2400 | 3200 (VOICE.md).
    std::string codec = "AUTO";
    std::string mode = "CONNECTED";
    std::string ptt_hotkey;
    std::string capture_device;
    std::string playback_device;
    int capture_gain_percent = 100;
    int playback_gain_percent = 100;
};

// Local radio control configuration
struct RadioConfig {
    bool enabled = false;
    std::string backend = "none";   // none | hamlib | kenwood_tcp
    std::string device;
    int baud = 38400;
    int hamlib_model = 0;          // 0 = dry-run/manual dev mode
    std::string hamlib_manufacturer;
    std::string kenwood_host;
    uint16_t kenwood_port = 5002;
    bool auto_connect = true;
    bool ptt_server_enabled = true;
    std::string ptt_server_bind = "127.0.0.1";
    uint16_t ptt_server_port = 4533;
    std::string default_band = "40m";
    bool auto_qsy = true;           // Auto QSY after CQ response
    int qsy_ack_timeout_ms = 8000;      // Wait for QSY ACK before rollback
    int qsy_reconnect_delay_ms = 2500;  // Delay before retry connect from calling freq
    int qsy_reconnect_window_ms = 15000;// Treat link drop in this window as QSY failure
    int qsy_connect_timeout_ms = 12000; // Max wait for each reconnect attempt
    int qsy_max_reconnect_attempts = 3; // Max initiator reconnect attempts after QSY fail
};

// APRS configuration
struct APRSConfig {
    bool enabled = false;
    std::string server = "rotate.aprs.net";
    uint16_t port = 14580;
    std::string passcode;           // APRS-IS passcode
    int beacon_interval_sec = 900;  // 15 minutes
    char symbol_table = '/';
    char symbol = '-';              // House by default
    std::string comment = "Skywave Link HF Digital";
    bool gps_enabled = false;
    std::string gps_port;           // Serial port for NMEA GPS
    int gps_baud = 9600;            // Serial baud for NMEA GPS
};

// Email gateway configuration
struct EmailConfig {
    // SMTP outbound
    std::string smtp_host;
    uint16_t smtp_port = 587;
    std::string smtp_username;
    std::string smtp_password;
    bool smtp_use_tls = true;

    // POP3/IMAP inbound
    std::string pop3_host;
    uint16_t pop3_port = 995;
    std::string pop3_username;
    std::string pop3_password;
    bool pop3_use_tls = true;
    int pop3_poll_interval_sec = 300;
    int pop3_max_messages = 50;

    // Gateway mode
    bool gateway_enabled = false;   // Act as email gateway for other stations
    std::vector<std::string> allowed_relays;  // Callsigns allowed to relay

    // Winlink CMS Telnet (internet) relay
    bool winlink_telnet_enabled = false;
    std::string winlink_telnet_host = "server.winlink.org";
    uint16_t winlink_telnet_port = 8772;
    bool winlink_telnet_use_tls = false;
    std::string winlink_telnet_callsign;
    std::string winlink_telnet_password;
    int winlink_fetch_interval_sec = 300;
    int winlink_fetch_max_messages = 20;

    // Interop/reliability enhancements
    bool winlink_compat_mode = false;          // Wrap relay traffic in B2F envelope
    bool weak_signal_profile_enabled = true;   // Enable robust HF behavior on weak paths
    int weak_signal_snr_threshold_db = -8;     // Trigger threshold for weak profile
    int weak_signal_ack_timeout_ms = 45000;    // Longer ACK timeout on weak links
    int weak_signal_max_retries = 5;           // More retries on weak links
    bool weak_signal_enable_compression = true;// LZH-compress payload when beneficial

    // HF store-and-forward relay safety policy
    bool hf_store_forward_enabled = true;       // Relay non-local mail over mesh
    int hf_store_forward_ttl = 3;               // Initial TTL for forwarded packets
    int hf_store_forward_hop_limit = 6;         // Absolute hop-count ceiling
    int hf_store_forward_dedup_window_sec = 300;// Duplicate suppression window
};

// Message store configuration
struct StoreConfig {
    std::string db_path = "messages.db";
    int message_expiry_days = 7;
    int max_pending_messages = 100;
    bool auto_cleanup = true;
};

// File transfer configuration
struct FileConfig {
    size_t max_file_size = 50 * 1024;  // 50 KB
    std::string download_path = "downloads";
    bool auto_accept = false;       // Accept every offer without asking
    // Offers from these stations are accepted without the Incoming File
    // prompt. Stored uppercase; `trusted_callsigns = "W1AW, K6XYZ-2"`.
    std::vector<std::string> trusted_callsigns;
};

// Callsign lists in config and settings fields: split on commas and
// whitespace, trimmed, uppercased, duplicates and empties dropped.
std::vector<std::string> parseCallsignList(const std::string& text);
std::string joinCallsignList(const std::vector<std::string>& calls);

// BBS configuration
struct BBSConfig {
    bool enabled = false;           // Enable BBS mode
    std::string bbs_path = "bbs";   // Directory for BBS files
    std::string welcome_message = "Welcome to Skywave Link BBS";
    bool allow_uploads = false;     // Allow remote stations to upload
    size_t max_upload_size = 50 * 1024;  // 50 KB max upload
    std::vector<std::string> allowed_uploaders;  // Callsigns allowed to upload
};

// Beacon configuration
struct BeaconConfig {
    bool enabled = false;           // Enable periodic beaconing
    int interval_sec = 600;         // Beacon interval (default 10 min)
    std::string beacon_text = "Skywave Link";  // Beacon message
    bool notify_pending = true;     // Notify stations with pending messages
};

// Away mode configuration
struct AwayConfig {
    bool enabled = false;           // Enable away mode
    std::string away_message = "Station is unattended. Leave a message.";
    bool allow_message_query = true;   // Allow remote query of messages
    bool allow_email_gateway = true;   // Allow email gateway access
    bool allow_bbs_access = true;      // Allow BBS file downloads
    bool auto_reply = true;            // Auto-reply with away message
};

// PSK Reporter configuration
struct PSKReporterConfig {
    bool enabled = false;           // Enable automatic PSK Reporter uploads
    std::string antenna = "Dipole"; // Antenna description
    int min_snr = -20;              // Minimum SNR to report (filter weak/false decodes)
    int upload_interval_sec = 300;  // Batch upload interval (5 min default)
};

// UI preferences
struct UIConfig {
    bool dark_mode = true;
    int font_size = 15;
    bool show_timestamps = true;
    bool sound_notifications = true;
    std::string notification_sound;
    int window_width = 0;   // 0 = size to the display on first run
    int window_height = 0;
};

// Complete application configuration
struct AppConfig {
    StationConfig station;
    ModemConfig modem;
    VoiceConfig voice;
    RadioConfig radio;
    APRSConfig aprs;
    EmailConfig email;
    StoreConfig store;
    FileConfig files;
    BBSConfig bbs;
    BeaconConfig beacon;
    AwayConfig away;
    PSKReporterConfig psk_reporter;
    UIConfig ui;

    // Persistence
    bool save(const std::string& path) const;
    bool load(const std::string& path);
    void setDefaults();

    // Config file path
    static std::string getDefaultConfigPath();
    static std::string getLegacyConfigPath();
};

} // namespace skylink
