// SharedAppState - Shared data structures accessible by all modular windows
#pragma once

#include "config.hpp"
#include "modem_client.hpp"
#include "message_store.hpp"
#include "freq_manager.hpp"
#include "aprs_client.hpp"
#include "file_transfer.hpp"

#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace skylink {

class RadioController;

// Chat message for display
struct ChatMessage {
    std::string from;
    std::string to;
    std::string text;
    int64_t timestamp = 0;
    bool is_outgoing = false;
    bool is_broadcast = false;
    bool delivered = false;
};

// File transfer state
struct FileTransfer {
    std::string filename;
    std::string remote_callsign;
    size_t total_bytes = 0;
    size_t transferred_bytes = 0;
    bool is_upload = false;
    bool completed = false;
    bool failed = false;
    std::string error;
    int64_t started_at = 0;
};

// Application tab selection
enum class AppTab {
    Chat,
    Files,
    BBS,
    Frequency,
    Messages,
    Mail,
    Weather,
    Email,
    Position,
    StationMap,
    Settings
};

enum class StationLocationSource {
    Beacon,
    PositionBeacon,
    DirectPosition,
    APRS
};

// Beacon heard from another station
struct HeardBeacon {
    std::string callsign;
    std::string grid_square;
    std::string message;
    float snr_db = 0.0f;
    int64_t timestamp = 0;
    bool has_pending_messages = false;
    bool email_gateway = false;
};

// Ping result
struct PingResult {
    std::string callsign;
    float snr_db = 0.0f;
    std::string grid_square;
    int64_t timestamp = 0;
    bool online = false;
};

struct StationLocationEntry {
    std::string callsign;
    std::string grid_square;
    double latitude = 0.0;
    double longitude = 0.0;
    bool has_coordinates = false;
    float snr_db = 0.0f;
    int64_t timestamp = 0;
    std::string details;
    StationLocationSource source = StationLocationSource::Beacon;
    bool email_gateway = false;
};

// Gateway/internet relay diagnostics surfaced to UI
struct RelayDiagnostics {
    int64_t last_attempt_unix = 0;
    int64_t last_success_unix = 0;
    std::string last_mode = "none";
    std::string last_target;
    std::string last_result = "idle";
    std::string last_error;
    std::string last_failover_reason;
    std::string gateway_health = "idle";
    std::string queue_policy_state = "idle";
    std::string last_hold_reason;
    int total_success = 0;
    int total_fail = 0;
    int consecutive_failures = 0;
    int smtp_fallback_count = 0;
    int held_low_priority_count = 0;
    int winlink_fetch_success = 0;
    int winlink_fetch_fail = 0;
    int last_login_attempts = 0;
    float last_gateway_score = 0.0f;
    int last_burst_sent = 0;
};

enum class MailPriority {
    Routine = 0,
    Priority = 1,
    Emergency = 2
};

// Mail message (asynchronous)
struct MailMessage {
    int64_t store_id = 0;
    std::string message_id;
    std::string from;
    std::string to;
    std::string subject;
    std::string body;
    std::string display_body;
    std::string template_name;
    int64_t timestamp = 0;
    bool read = false;
    bool is_broadcast = false;
    bool needs_gateway = false;
    bool weak_signal_profile = false;
    bool winlink_payload = false;
    bool compressed_payload = false;
    MailPriority priority = MailPriority::Routine;
    int retry_count = 0;
    MessageStatus status = MessageStatus::Draft;
};

// Weather report
struct WeatherReport {
    std::string type;
    std::string location;
    std::string report;
    std::string source_station;
    std::string severity;
    std::string expires_utc;
    int64_t timestamp = 0;
    bool is_request_pending = false;
};

// PSK Reporter spot
struct PSKReporterSpot {
    std::string callsign;
    std::string grid_square;
    float snr_db = 0.0f;
    uint32_t frequency_hz = 0;
    int64_t timestamp = 0;
};

// BBS file entry
struct BBSFile {
    std::string filename;
    std::string description;
    size_t size_bytes = 0;
    int64_t upload_time = 0;
    std::string uploader;
    bool remote = false;
};

// Pending remote BBS download request status
struct PendingBbsDownload {
    std::string filename;
    std::string owner_callsign;
    int64_t requested_at = 0;
    bool request_sent = false;
};

// Pending async weather fetch
struct PendingWeatherFetch {
    uint8_t type = 0;
    std::string query;
    std::string result;
    bool complete = false;
    bool success = false;
};

// SharedAppState holds all data that windows need to access.
// The App class owns the actual data; windows get a pointer to this struct.
struct SharedAppState {
    // Non-copyable: all pointers are non-owning and shallow copy is dangerous
    SharedAppState() = default;
    SharedAppState(const SharedAppState&) = delete;
    SharedAppState& operator=(const SharedAppState&) = delete;

    // Core components (owned by App, non-owning pointers here)
    AppConfig* config = nullptr;
    ModemClient* modem = nullptr;
    RadioController* radio = nullptr;
    MessageStore* store = nullptr;
    FrequencyManager* freq_mgr = nullptr;
    APRSClient* aprs = nullptr;
    FileTransferManager* file_transfer_mgr = nullptr;

    // UI state
    AppTab* current_tab = nullptr;
    bool* show_settings = nullptr;
    bool* running = nullptr;

    // Chat state
    std::vector<ChatMessage>* chat_history = nullptr;
    char* chat_input = nullptr;
    size_t chat_input_size = 0;
    std::string* current_chat_partner = nullptr;
    bool* chat_scroll_to_bottom = nullptr;

    // Typing indicator
    bool* remote_is_typing = nullptr;
    std::chrono::steady_clock::time_point* last_typing_indicator = nullptr;
    std::chrono::steady_clock::time_point* last_typing_sent = nullptr;
    bool* last_input_was_empty = nullptr;

    // Message fragmentation
    std::map<std::string, std::vector<std::pair<int, std::string>>>* fragment_buffer = nullptr;
    int* outgoing_fragment_counter = nullptr;

    // File transfers
    std::vector<FileTransfer>* transfers = nullptr;
    int* unread_file_notifications = nullptr;
    char* file_path_input = nullptr;
    size_t file_path_input_size = 0;

    // Messages (store-and-forward)
    std::vector<Message>* inbox_messages = nullptr;
    std::vector<Message>* outbox_messages = nullptr;
    int* selected_message = nullptr;

    // Position
    double* my_lat = nullptr;
    double* my_lon = nullptr;
    char* position_comment = nullptr;
    size_t position_comment_size = 0;

    // Settings input buffers
    char* callsign_input = nullptr;
    size_t callsign_input_size = 0;
    char* modem_host_input = nullptr;
    size_t modem_host_input_size = 0;
    int* modem_port_input = nullptr;

    // CQ/Broadcast
    bool* cq_active = nullptr;
    int* cq_repeat_count = nullptr;
    int* cq_current_repeat = nullptr;
    char* broadcast_message = nullptr;
    size_t broadcast_message_size = 0;
    std::deque<std::pair<std::string, std::string>>* heard_stations = nullptr;

    // Channel sniffer
    bool* channel_sniffer_active = nullptr;
    int* sniffer_current_slot = nullptr;
    std::vector<std::pair<int, bool>>* slot_status = nullptr;

    // GPS state
    bool* gps_fix_valid = nullptr;
    std::string* gps_port = nullptr;
    int* gps_baud = nullptr;

    // Email gateway
    std::vector<Message>* email_outbox = nullptr;
    std::vector<Message>* radio_outbox = nullptr;
    bool* email_gateway_active = nullptr;
    RelayDiagnostics* relay_diagnostics = nullptr;

    // BBS
    std::vector<BBSFile>* bbs_files = nullptr;
    std::vector<PendingBbsDownload>* pending_bbs_downloads = nullptr;
    int* selected_bbs_file = nullptr;
    char* bbs_upload_path = nullptr;
    size_t bbs_upload_path_size = 0;
    char* bbs_file_desc = nullptr;
    size_t bbs_file_desc_size = 0;

    // Frequency
    std::string* current_band = nullptr;
    int* current_slot = nullptr;

    // QSY coordination
    bool* qsy_suggestion_pending = nullptr;
    std::string* qsy_suggestion_from = nullptr;
    std::string* qsy_suggestion_band = nullptr;
    int* qsy_suggestion_slot = nullptr;
    uint32_t* qsy_suggestion_frequency_hz = nullptr;

    // Beacons
    std::deque<HeardBeacon>* heard_beacons = nullptr;
    bool* beacon_enabled = nullptr;
    std::vector<StationLocationEntry>* station_locations = nullptr;

    // QSO
    bool* qso_active = nullptr;
    float* remote_snr_db = nullptr;
    float* local_snr_db = nullptr;
    bool* snr_exchanged = nullptr;
    std::chrono::steady_clock::time_point* qso_start_time = nullptr;

    // Away mode
    bool* away_mode_active = nullptr;

    // Ping
    std::deque<PingResult>* ping_results = nullptr;
    bool* ping_in_progress = nullptr;
    std::string* ping_target = nullptr;

    // Mail
    std::vector<MailMessage>* mail_inbox = nullptr;
    std::vector<MailMessage>* mail_outbox = nullptr;
    char* mail_to_input = nullptr;
    size_t mail_to_input_size = 0;
    char* mail_subject_input = nullptr;
    size_t mail_subject_input_size = 0;
    char* mail_body_input = nullptr;
    size_t mail_body_input_size = 0;
    int* mail_priority_input = nullptr;
    int* selected_mail = nullptr;

    // Weather
    std::vector<WeatherReport>* weather_reports = nullptr;
    char* weather_location_input = nullptr;
    size_t weather_location_input_size = 0;
    char* weather_airport_input = nullptr;
    size_t weather_airport_input_size = 0;
    char* weather_zone_input = nullptr;
    size_t weather_zone_input_size = 0;
    bool* weather_request_pending = nullptr;
    std::chrono::steady_clock::time_point* weather_request_time = nullptr;
    std::mutex* weather_fetch_mutex = nullptr;
    std::unique_ptr<PendingWeatherFetch>* pending_weather_fetch = nullptr;

    // PSK Reporter
    std::vector<PSKReporterSpot>* psk_reporter_queue = nullptr;
    std::mutex* psk_reporter_mutex = nullptr;
    uint32_t* current_frequency_hz = nullptr;

    // ---- Helper methods ----

    // Add a system message to chat history
    void addSystemMessage(const std::string& text);

    // Sync file transfers from the manager
    void syncTransfers();

    // Format QSO duration string
    std::string formatQSODuration() const;

    // Check if a file is an image
    static bool isImageFile(const std::string& filename);

    // Get file type description
    static std::string getFileTypeDescription(const std::string& filename);
};

} // namespace skylink
