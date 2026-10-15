// Configuration management implementation

#include "skywave_link/config.hpp"
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <algorithm>

namespace skylink {

static std::string escapeToml(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        if (c == '\\') result += "\\\\";
        else if (c == '"') result += "\\\"";
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else if (c == '\t') result += "\\t";
        else result += c;
    }
    return result;
}

static std::string unescapeToml(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
                case '\\': result += '\\'; i++; break;
                case '"':  result += '"';  i++; break;
                case 'n':  result += '\n'; i++; break;
                case 'r':  result += '\r'; i++; break;
                case 't':  result += '\t'; i++; break;
                default:   result += s[i]; break;
            }
        } else {
            result += s[i];
        }
    }
    return result;
}

// Safe numeric parsers that return a default on malformed input
static int safeStoi(const std::string& value, int def = 0) {
    try { return std::stoi(value); }
    catch (...) { return def; }
}

static double safeStod(const std::string& value, double def = 0.0) {
    try { return std::stod(value); }
    catch (...) { return def; }
}

static size_t safeStoull(const std::string& value, size_t def = 0) {
    try { return static_cast<size_t>(std::stoull(value)); }
    catch (...) { return def; }
}

static uint16_t safeUint16(const std::string& value) {
    int v = safeStoi(value, 0);
    v = std::clamp(v, 0, static_cast<int>(std::numeric_limits<uint16_t>::max()));
    return static_cast<uint16_t>(v);
}

void AppConfig::setDefaults() {
    // Station defaults
    station.callsign = "";
    station.grid_square = "";
    station.name = "";
    station.latitude = 0.0;
    station.longitude = 0.0;
    station.altitude_m = 0.0;

    // Modem defaults
    modem.host = "127.0.0.1";
    modem.command_port = 9101;
    modem.data_port = 9100;
    modem.voice_port = 9102;
    modem.family = "WIDE";
    modem.auto_connect = true;
    modem.reconnect_interval_sec = 5;

    // Voice defaults
    voice.auto_start = false;
    voice.codec = "AUTO";
    voice.mode = "CONNECTED";
    voice.ptt_hotkey.clear();
    voice.capture_device.clear();
    voice.playback_device.clear();
    voice.capture_gain_percent = 100;
    voice.playback_gain_percent = 100;

    // Radio defaults
    radio.enabled = false;
    radio.backend = "none";
    radio.device.clear();
    radio.baud = 38400;
    radio.hamlib_model = 0;
    radio.hamlib_manufacturer.clear();
    radio.kenwood_host.clear();
    radio.kenwood_port = 5002;
    radio.auto_connect = true;
    radio.ptt_server_enabled = true;
    radio.ptt_server_bind = "127.0.0.1";
    radio.ptt_server_port = 4533;
    radio.default_band = "40m";
    radio.auto_qsy = true;
    radio.qsy_ack_timeout_ms = 8000;
    radio.qsy_reconnect_delay_ms = 2500;
    radio.qsy_reconnect_window_ms = 15000;
    radio.qsy_connect_timeout_ms = 12000;
    radio.qsy_max_reconnect_attempts = 3;

    // APRS defaults
    aprs.enabled = false;
    aprs.server = "rotate.aprs.net";
    aprs.port = 14580;
    aprs.passcode = "";
    aprs.beacon_interval_sec = 900;
    aprs.symbol_table = '/';
    aprs.symbol = '-';
    aprs.comment = "Skywave Link HF Digital";
    aprs.gps_enabled = false;
    aprs.gps_port = "";
    aprs.gps_baud = 9600;

    // Email defaults
    email.smtp_host = "";
    email.smtp_port = 587;
    email.smtp_username = "";
    email.smtp_password = "";
    email.smtp_use_tls = true;
    email.pop3_host = "";
    email.pop3_port = 995;
    email.pop3_username = "";
    email.pop3_password = "";
    email.pop3_use_tls = true;
    email.pop3_poll_interval_sec = 300;
    email.pop3_max_messages = 50;
    email.gateway_enabled = false;
    email.allowed_relays.clear();
    email.winlink_telnet_enabled = false;
    email.winlink_telnet_host = "server.winlink.org";
    email.winlink_telnet_port = 8772;
    email.winlink_telnet_use_tls = false;
    email.winlink_telnet_callsign = "";
    email.winlink_telnet_password = "";
    email.winlink_fetch_interval_sec = 300;
    email.winlink_fetch_max_messages = 20;
    email.winlink_compat_mode = false;
    email.weak_signal_profile_enabled = true;
    email.weak_signal_snr_threshold_db = -8;
    email.weak_signal_ack_timeout_ms = 45000;
    email.weak_signal_max_retries = 5;
    email.weak_signal_enable_compression = true;
    email.hf_store_forward_enabled = true;
    email.hf_store_forward_ttl = 3;
    email.hf_store_forward_hop_limit = 6;
    email.hf_store_forward_dedup_window_sec = 300;

    // Store defaults
    store.db_path = "messages.db";
    store.message_expiry_days = 7;
    store.max_pending_messages = 100;
    store.auto_cleanup = true;

    // File defaults
    files.max_file_size = 50 * 1024;  // 50 KB
    files.download_path = "downloads";
    files.auto_accept = false;
    files.trusted_callsigns.clear();

    // PSK Reporter defaults
    psk_reporter.enabled = false;
    psk_reporter.antenna = "Dipole";
    psk_reporter.min_snr = -20;
    psk_reporter.upload_interval_sec = 300;

    // UI defaults
    ui.dark_mode = true;
    ui.font_size = 14;
    ui.show_timestamps = true;
    ui.sound_notifications = true;
    ui.notification_sound = "";
    ui.window_width = 1024;
    ui.window_height = 768;
}

std::string AppConfig::getDefaultConfigPath() {
    std::string home;

#ifdef _WIN32
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile) {
        home = userprofile;
    } else {
        home = ".";
    }
    return home + "\\AppData\\Roaming\\Skywave Link\\config.toml";
#else
    const char* home_env = std::getenv("HOME");
    if (home_env) {
        home = home_env;
    } else {
        home = ".";
    }
    return home + "/.config/skywave-link/config.toml";
#endif
}

std::string AppConfig::getLegacyConfigPath() {
    std::string home;

#ifdef _WIN32
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile) {
        home = userprofile;
    } else {
        home = ".";
    }
    return home + "\\AppData\\Roaming\\Skywave\\config.toml";
#else
    const char* home_env = std::getenv("HOME");
    if (home_env) {
        home = home_env;
    } else {
        home = ".";
    }
    return home + "/.config/skywave_link/config.toml";
#endif
}

bool AppConfig::save(const std::string& path) const {
    // Create directory if it doesn't exist
    std::filesystem::path config_path(path);
    std::filesystem::path dir = config_path.parent_path();
    if (!dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }

    std::ofstream file(path);
    if (!file) {
        std::cerr << "[Config] Failed to open file for writing: " << path << "\n";
        return false;
    }

    file << "# Skywave Link Configuration\n";
    file << "# Generated automatically - edit with care\n\n";

    // Station section
    file << "[station]\n";
    file << "callsign = \"" << escapeToml(station.callsign) << "\"\n";
    file << "grid_square = \"" << escapeToml(station.grid_square) << "\"\n";
    file << "name = \"" << escapeToml(station.name) << "\"\n";
    file << "latitude = " << station.latitude << "\n";
    file << "longitude = " << station.longitude << "\n";
    file << "altitude_m = " << station.altitude_m << "\n";
    file << "\n";

    // Modem section
    file << "[modem]\n";
    file << "host = \"" << escapeToml(modem.host) << "\"\n";
    file << "command_port = " << modem.command_port << "\n";
    file << "data_port = " << modem.data_port << "\n";
    file << "voice_port = " << modem.voice_port << "\n";
    file << "family = \"" << escapeToml(modem.family) << "\"\n";
    file << "auto_connect = " << (modem.auto_connect ? "true" : "false") << "\n";
    file << "reconnect_interval_sec = " << modem.reconnect_interval_sec << "\n";
    file << "\n";

    file << "[voice]\n";
    file << "auto_start = " << (voice.auto_start ? "true" : "false") << "\n";
    file << "codec = \"" << escapeToml(voice.codec) << "\"\n";
    file << "mode = \"" << escapeToml(voice.mode) << "\"\n";
    file << "ptt_hotkey = \"" << escapeToml(voice.ptt_hotkey) << "\"\n";
    file << "capture_device = \"" << escapeToml(voice.capture_device) << "\"\n";
    file << "playback_device = \"" << escapeToml(voice.playback_device) << "\"\n";
    file << "capture_gain_percent = " << voice.capture_gain_percent << "\n";
    file << "playback_gain_percent = " << voice.playback_gain_percent << "\n";
    file << "\n";

    // Radio section
    file << "[radio]\n";
    file << "enabled = " << (radio.enabled ? "true" : "false") << "\n";
    file << "backend = \"" << escapeToml(radio.backend) << "\"\n";
    file << "device = \"" << escapeToml(radio.device) << "\"\n";
    file << "baud = " << radio.baud << "\n";
    file << "hamlib_model = " << radio.hamlib_model << "\n";
    file << "hamlib_manufacturer = \"" << escapeToml(radio.hamlib_manufacturer) << "\"\n";
    file << "kenwood_host = \"" << escapeToml(radio.kenwood_host) << "\"\n";
    file << "kenwood_port = " << radio.kenwood_port << "\n";
    file << "auto_connect = " << (radio.auto_connect ? "true" : "false") << "\n";
    file << "ptt_server_enabled = " << (radio.ptt_server_enabled ? "true" : "false") << "\n";
    file << "ptt_server_bind = \"" << escapeToml(radio.ptt_server_bind) << "\"\n";
    file << "ptt_server_port = " << radio.ptt_server_port << "\n";
    file << "default_band = \"" << escapeToml(radio.default_band) << "\"\n";
    file << "auto_qsy = " << (radio.auto_qsy ? "true" : "false") << "\n";
    file << "qsy_ack_timeout_ms = " << radio.qsy_ack_timeout_ms << "\n";
    file << "qsy_reconnect_delay_ms = " << radio.qsy_reconnect_delay_ms << "\n";
    file << "qsy_reconnect_window_ms = " << radio.qsy_reconnect_window_ms << "\n";
    file << "qsy_connect_timeout_ms = " << radio.qsy_connect_timeout_ms << "\n";
    file << "qsy_max_reconnect_attempts = " << radio.qsy_max_reconnect_attempts << "\n";
    file << "\n";

    // APRS section
    file << "[aprs]\n";
    file << "enabled = " << (aprs.enabled ? "true" : "false") << "\n";
    file << "server = \"" << escapeToml(aprs.server) << "\"\n";
    file << "port = " << aprs.port << "\n";
    file << "passcode = \"" << escapeToml(aprs.passcode) << "\"\n";
    file << "beacon_interval_sec = " << aprs.beacon_interval_sec << "\n";
    file << "symbol_table = \"" << aprs.symbol_table << "\"\n";
    file << "symbol = \"" << aprs.symbol << "\"\n";
    file << "comment = \"" << escapeToml(aprs.comment) << "\"\n";
    file << "gps_enabled = " << (aprs.gps_enabled ? "true" : "false") << "\n";
    file << "gps_port = \"" << escapeToml(aprs.gps_port) << "\"\n";
    file << "gps_baud = " << aprs.gps_baud << "\n";
    file << "\n";

    // Email section
    file << "[email]\n";
    file << "smtp_host = \"" << escapeToml(email.smtp_host) << "\"\n";
    file << "smtp_port = " << email.smtp_port << "\n";
    file << "smtp_username = \"" << escapeToml(email.smtp_username) << "\"\n";
    file << "# smtp_password stored securely\n";
    file << "smtp_use_tls = " << (email.smtp_use_tls ? "true" : "false") << "\n";
    file << "pop3_host = \"" << escapeToml(email.pop3_host) << "\"\n";
    file << "pop3_port = " << email.pop3_port << "\n";
    file << "pop3_username = \"" << escapeToml(email.pop3_username) << "\"\n";
    file << "pop3_use_tls = " << (email.pop3_use_tls ? "true" : "false") << "\n";
    file << "pop3_poll_interval_sec = " << email.pop3_poll_interval_sec << "\n";
    file << "pop3_max_messages = " << email.pop3_max_messages << "\n";
    file << "gateway_enabled = " << (email.gateway_enabled ? "true" : "false") << "\n";
    file << "winlink_telnet_enabled = " << (email.winlink_telnet_enabled ? "true" : "false") << "\n";
    file << "winlink_telnet_host = \"" << escapeToml(email.winlink_telnet_host) << "\"\n";
    file << "winlink_telnet_port = " << email.winlink_telnet_port << "\n";
    file << "winlink_telnet_use_tls = " << (email.winlink_telnet_use_tls ? "true" : "false") << "\n";
    file << "winlink_telnet_callsign = \"" << escapeToml(email.winlink_telnet_callsign) << "\"\n";
    file << "# winlink_telnet_password stored securely\n";
    file << "winlink_fetch_interval_sec = " << email.winlink_fetch_interval_sec << "\n";
    file << "winlink_fetch_max_messages = " << email.winlink_fetch_max_messages << "\n";
    file << "winlink_compat_mode = " << (email.winlink_compat_mode ? "true" : "false") << "\n";
    file << "weak_signal_profile_enabled = " << (email.weak_signal_profile_enabled ? "true" : "false") << "\n";
    file << "weak_signal_snr_threshold_db = " << email.weak_signal_snr_threshold_db << "\n";
    file << "weak_signal_ack_timeout_ms = " << email.weak_signal_ack_timeout_ms << "\n";
    file << "weak_signal_max_retries = " << email.weak_signal_max_retries << "\n";
    file << "weak_signal_enable_compression = "
         << (email.weak_signal_enable_compression ? "true" : "false") << "\n";
    file << "hf_store_forward_enabled = " << (email.hf_store_forward_enabled ? "true" : "false") << "\n";
    file << "hf_store_forward_ttl = " << email.hf_store_forward_ttl << "\n";
    file << "hf_store_forward_hop_limit = " << email.hf_store_forward_hop_limit << "\n";
    file << "hf_store_forward_dedup_window_sec = " << email.hf_store_forward_dedup_window_sec << "\n";
    file << "\n";

    // Store section
    file << "[store]\n";
    file << "db_path = \"" << escapeToml(store.db_path) << "\"\n";
    file << "message_expiry_days = " << store.message_expiry_days << "\n";
    file << "max_pending_messages = " << store.max_pending_messages << "\n";
    file << "auto_cleanup = " << (store.auto_cleanup ? "true" : "false") << "\n";
    file << "\n";

    // Files section
    file << "[files]\n";
    file << "max_file_size = " << files.max_file_size << "\n";
    file << "download_path = \"" << escapeToml(files.download_path) << "\"\n";
    file << "auto_accept = " << (files.auto_accept ? "true" : "false") << "\n";
    file << "\n";

    // Beacon section
    file << "[beacon]\n";
    file << "enabled = " << (beacon.enabled ? "true" : "false") << "\n";
    file << "interval_sec = " << beacon.interval_sec << "\n";
    file << "beacon_text = \"" << escapeToml(beacon.beacon_text) << "\"\n";
    file << "notify_pending = " << (beacon.notify_pending ? "true" : "false") << "\n";
    file << "\n";

    // PSK Reporter section
    file << "[psk_reporter]\n";
    file << "enabled = " << (psk_reporter.enabled ? "true" : "false") << "\n";
    file << "antenna = \"" << escapeToml(psk_reporter.antenna) << "\"\n";
    file << "min_snr = " << psk_reporter.min_snr << "\n";
    file << "upload_interval_sec = " << psk_reporter.upload_interval_sec << "\n";
    file << "\n";

    // UI section
    file << "[ui]\n";
    file << "dark_mode = " << (ui.dark_mode ? "true" : "false") << "\n";
    file << "font_size = " << ui.font_size << "\n";
    file << "show_timestamps = " << (ui.show_timestamps ? "true" : "false") << "\n";
    file << "sound_notifications = " << (ui.sound_notifications ? "true" : "false") << "\n";
    file << "window_width = " << ui.window_width << "\n";
    file << "window_height = " << ui.window_height << "\n";

    return true;
}

bool AppConfig::load(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        setDefaults();
        return false;
    }

    // Start with defaults
    setDefaults();

    std::string line;
    std::string current_section;

    while (std::getline(file, line)) {
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') continue;

        // Trim whitespace
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        // Check for section header
        if (line[0] == '[') {
            size_t end = line.find(']');
            if (end != std::string::npos) {
                current_section = line.substr(1, end - 1);
            }
            continue;
        }

        // Parse key = value
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        // Trim whitespace
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));

        // Remove quotes from string values and unescape
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = unescapeToml(value.substr(1, value.size() - 2));
        } else if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
            // Single-quoted TOML: literal string, no escaping
            value = value.substr(1, value.size() - 2);
        }

        // Parse based on section
        if (current_section == "station") {
            if (key == "callsign") station.callsign = value;
            else if (key == "grid_square") station.grid_square = value;
            else if (key == "name") station.name = value;
            else if (key == "latitude") station.latitude = safeStod(value);
            else if (key == "longitude") station.longitude = safeStod(value);
            else if (key == "altitude_m") station.altitude_m = safeStod(value);
        }
        else if (current_section == "modem") {
            if (key == "host") modem.host = value;
            else if (key == "command_port") modem.command_port = safeUint16(value);
            else if (key == "data_port") modem.data_port = safeUint16(value);
            else if (key == "voice_port") modem.voice_port = safeUint16(value);
            else if (key == "family") modem.family = value;
            else if (key == "auto_connect") modem.auto_connect = (value == "true");
            else if (key == "reconnect_interval_sec") modem.reconnect_interval_sec = safeStoi(value, 5);
        }
        else if (current_section == "voice") {
            if (key == "auto_start") voice.auto_start = (value == "true");
            else if (key == "codec") voice.codec = value;
            else if (key == "mode") voice.mode = value;
            else if (key == "ptt_hotkey") voice.ptt_hotkey = value;
            else if (key == "capture_device") voice.capture_device = value;
            else if (key == "playback_device") voice.playback_device = value;
            else if (key == "capture_gain_percent") voice.capture_gain_percent = safeStoi(value, 100);
            else if (key == "playback_gain_percent") voice.playback_gain_percent = safeStoi(value, 100);
        }
        else if (current_section == "radio") {
            if (key == "enabled" || key == "cat_enabled") radio.enabled = (value == "true");
            else if (key == "backend") radio.backend = value;
            else if (key == "device") radio.device = value;
            else if (key == "baud") radio.baud = safeStoi(value, 38400);
            else if (key == "hamlib_model") radio.hamlib_model = safeStoi(value, 0);
            else if (key == "hamlib_manufacturer") radio.hamlib_manufacturer = value;
            else if (key == "kenwood_host") radio.kenwood_host = value;
            else if (key == "kenwood_port") radio.kenwood_port = safeUint16(value);
            else if (key == "auto_connect") radio.auto_connect = (value == "true");
            else if (key == "ptt_server_enabled") radio.ptt_server_enabled = (value == "true");
            else if (key == "ptt_server_bind") radio.ptt_server_bind = value;
            else if (key == "ptt_server_port") radio.ptt_server_port = safeUint16(value);
            else if (key == "default_band") radio.default_band = value;
            else if (key == "auto_qsy") radio.auto_qsy = (value == "true");
            else if (key == "qsy_ack_timeout_ms") radio.qsy_ack_timeout_ms = safeStoi(value, 8000);
            else if (key == "qsy_reconnect_delay_ms") radio.qsy_reconnect_delay_ms = safeStoi(value, 2500);
            else if (key == "qsy_reconnect_window_ms") radio.qsy_reconnect_window_ms = safeStoi(value, 15000);
            else if (key == "qsy_connect_timeout_ms") radio.qsy_connect_timeout_ms = safeStoi(value, 12000);
            else if (key == "qsy_max_reconnect_attempts") radio.qsy_max_reconnect_attempts = safeStoi(value, 3);
        }
        else if (current_section == "aprs") {
            if (key == "enabled") aprs.enabled = (value == "true");
            else if (key == "server") aprs.server = value;
            else if (key == "port") aprs.port = safeUint16(value);
            else if (key == "passcode") aprs.passcode = value;
            else if (key == "beacon_interval_sec") aprs.beacon_interval_sec = safeStoi(value, 900);
            else if (key == "symbol_table") { if (!value.empty()) aprs.symbol_table = value[0]; }
            else if (key == "symbol") { if (!value.empty()) aprs.symbol = value[0]; }
            else if (key == "comment") aprs.comment = value;
            else if (key == "gps_enabled") aprs.gps_enabled = (value == "true");
            else if (key == "gps_port") aprs.gps_port = value;
            else if (key == "gps_baud") aprs.gps_baud = safeStoi(value, 9600);
        }
        else if (current_section == "email") {
            if (key == "smtp_host") email.smtp_host = value;
            else if (key == "smtp_port") email.smtp_port = safeUint16(value);
            else if (key == "smtp_username") email.smtp_username = value;
            else if (key == "smtp_password") email.smtp_password = value;
            else if (key == "smtp_use_tls") email.smtp_use_tls = (value == "true");
            else if (key == "pop3_host") email.pop3_host = value;
            else if (key == "pop3_port") email.pop3_port = safeUint16(value);
            else if (key == "pop3_username") email.pop3_username = value;
            else if (key == "pop3_password") email.pop3_password = value;
            else if (key == "pop3_use_tls") email.pop3_use_tls = (value == "true");
            else if (key == "pop3_poll_interval_sec") email.pop3_poll_interval_sec = safeStoi(value, 300);
            else if (key == "pop3_max_messages") email.pop3_max_messages = safeStoi(value, 50);
            else if (key == "gateway_enabled") email.gateway_enabled = (value == "true");
            else if (key == "winlink_telnet_enabled") email.winlink_telnet_enabled = (value == "true");
            else if (key == "winlink_telnet_host") email.winlink_telnet_host = value;
            else if (key == "winlink_telnet_port") email.winlink_telnet_port = safeUint16(value);
            else if (key == "winlink_telnet_use_tls") email.winlink_telnet_use_tls = (value == "true");
            else if (key == "winlink_telnet_callsign") email.winlink_telnet_callsign = value;
            else if (key == "winlink_telnet_password") email.winlink_telnet_password = value;
            else if (key == "winlink_fetch_interval_sec") email.winlink_fetch_interval_sec = safeStoi(value, 300);
            else if (key == "winlink_fetch_max_messages") email.winlink_fetch_max_messages = safeStoi(value, 20);
            else if (key == "winlink_compat_mode") email.winlink_compat_mode = (value == "true");
            else if (key == "weak_signal_profile_enabled") email.weak_signal_profile_enabled = (value == "true");
            else if (key == "weak_signal_snr_threshold_db") email.weak_signal_snr_threshold_db = safeStoi(value, -8);
            else if (key == "weak_signal_ack_timeout_ms") email.weak_signal_ack_timeout_ms = safeStoi(value, 45000);
            else if (key == "weak_signal_max_retries") email.weak_signal_max_retries = safeStoi(value, 5);
            else if (key == "weak_signal_enable_compression") email.weak_signal_enable_compression = (value == "true");
            else if (key == "hf_store_forward_enabled") email.hf_store_forward_enabled = (value == "true");
            else if (key == "hf_store_forward_ttl") email.hf_store_forward_ttl = safeStoi(value, 3);
            else if (key == "hf_store_forward_hop_limit") email.hf_store_forward_hop_limit = safeStoi(value, 6);
            else if (key == "hf_store_forward_dedup_window_sec") email.hf_store_forward_dedup_window_sec = safeStoi(value, 300);
        }
        else if (current_section == "store") {
            if (key == "db_path") store.db_path = value;
            else if (key == "message_expiry_days") store.message_expiry_days = safeStoi(value, 7);
            else if (key == "max_pending_messages") store.max_pending_messages = safeStoi(value, 100);
            else if (key == "auto_cleanup") store.auto_cleanup = (value == "true");
        }
        else if (current_section == "files") {
            if (key == "max_file_size") files.max_file_size = safeStoull(value, 50 * 1024);
            else if (key == "download_path") files.download_path = value;
            else if (key == "auto_accept") files.auto_accept = (value == "true");
        }
        else if (current_section == "beacon") {
            if (key == "enabled") beacon.enabled = (value == "true");
            else if (key == "interval_sec") beacon.interval_sec = safeStoi(value, 600);
            else if (key == "beacon_text") beacon.beacon_text = value;
            else if (key == "notify_pending") beacon.notify_pending = (value == "true");
        }
        else if (current_section == "psk_reporter") {
            if (key == "enabled") psk_reporter.enabled = (value == "true");
            else if (key == "antenna") psk_reporter.antenna = value;
            else if (key == "min_snr") psk_reporter.min_snr = safeStoi(value, -20);
            else if (key == "upload_interval_sec") psk_reporter.upload_interval_sec = safeStoi(value, 300);
        }
        else if (current_section == "ui") {
            if (key == "dark_mode") ui.dark_mode = (value == "true");
            else if (key == "font_size") ui.font_size = safeStoi(value, 14);
            else if (key == "show_timestamps") ui.show_timestamps = (value == "true");
            else if (key == "sound_notifications") ui.sound_notifications = (value == "true");
            else if (key == "window_width") ui.window_width = safeStoi(value, 1024);
            else if (key == "window_height") ui.window_height = safeStoi(value, 768);
        }
    }

    if (voice.codec.empty()) {
        voice.codec = "AUTO";
    } else {
        voice.codec = "AUTO";
    }

    if (radio.backend.empty()) {
        if (!radio.kenwood_host.empty()) {
            radio.backend = "kenwood_tcp";
        } else if (radio.enabled || radio.hamlib_model > 0 || !radio.device.empty()) {
            radio.backend = "hamlib";
        } else {
            radio.backend = "none";
        }
    }

    {
        std::string upper = modem.family;
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        modem.family = (upper == "MID" || upper == "NARROW" || upper == "ULTRA")
                           ? upper
                           : "WIDE";
    }

    return true;
}

} // namespace skylink
