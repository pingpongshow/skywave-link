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
class MeshRelay;

// Chat message for display
struct ChatMessage {
    std::string from;
    std::string to;
    std::string text;
    int64_t timestamp = 0;
    bool is_outgoing = false;
    bool is_broadcast = false;
    bool delivered = false;
    std::string via;             // Mesh relay that delivered it ("" = heard directly)
    std::string band;            // Band it was sent/heard on ("" = not recorded)
};

// File transfer state
struct FileTransfer {
    std::string transfer_id;     // FileTransferManager id (needed to cancel)
    std::string filename;
    std::string remote_callsign;
    size_t total_bytes = 0;
    size_t transferred_bytes = 0;
    bool is_upload = false;
    bool completed = false;
    bool failed = false;
    bool cancelled = false;
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
    bool snr_valid = false;      // snr_db is the modem's measurement of this frame
    std::string band;            // band it was heard on ("" = not recorded)
    std::string family;          // sender's bandwidth family ("" = unknown)
    int64_t timestamp = 0;
    bool has_pending_messages = false;
    bool email_gateway = false;
};

// CQ call heard from another station (main panel "Last heard CQ calls").
struct HeardCall {
    std::string callsign;
    int64_t timestamp = 0;       // Unix seconds
    std::string band;            // "" = not recorded
    std::string family;          // "" = unknown
    float snr_db = 0.0f;
    bool snr_valid = false;      // measured SNR of the CQ frame
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
    std::string via;             // Mesh relay that delivered it ("" = heard directly)
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
};

// PSK Reporter client status for Settings (App refreshes it about once a
// second while PSK Reporter is enabled).
struct PskReporterStatus {
    bool active = false;            // client running (enabled and a callsign set)
    size_t queued_spots = 0;
    uint64_t spots_sent = 0;
    uint64_t last_upload_unix = 0;  // 0 = nothing uploaded yet this session
    uint64_t next_upload_unix = 0;  // 0 = not scheduled
    bool upload_in_progress = false;
    std::string last_error;
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
    const MeshRelay* mesh_relay = nullptr;     // read-only: route table display

    // UI state
    AppTab* current_tab = nullptr;
    bool* show_settings = nullptr;

    // Chat state
    std::vector<ChatMessage>* chat_history = nullptr;
    char* chat_input = nullptr;
    size_t chat_input_size = 0;
    std::string* current_chat_partner = nullptr;
    bool* chat_scroll_to_bottom = nullptr;

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
    std::deque<HeardCall>* heard_calls = nullptr;

    // Channel sniffer
    bool* channel_sniffer_active = nullptr;
    int* sniffer_current_slot = nullptr;
    std::vector<std::pair<int, bool>>* slot_status = nullptr;

    // GPS state
    bool* gps_fix_valid = nullptr;
    std::string* gps_port = nullptr;
    int* gps_baud = nullptr;

    // Email gateway
    bool* email_gateway_active = nullptr;
    RelayDiagnostics* relay_diagnostics = nullptr;

    // BBS
    std::vector<BBSFile>* bbs_files = nullptr;
    std::vector<PendingBbsDownload>* pending_bbs_downloads = nullptr;
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
    std::chrono::steady_clock::time_point* qso_start_time = nullptr;

    // Away mode
    bool* away_mode_active = nullptr;

    // Ping
    std::deque<PingResult>* ping_results = nullptr;
    bool* ping_in_progress = nullptr;

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

    // PSK Reporter
    const PskReporterStatus* psk_status = nullptr;
    uint32_t* current_frequency_hz = nullptr;

    // ---- Helper methods ----

    // Add a system message to chat history
    void addSystemMessage(const std::string& text);

    // Windows report the outcome of App actions they trigger: these look
    // at what the action appended to chat_history from index `since` on.
    size_t chatHistorySize() const { return chat_history ? chat_history->size() : 0; }
    // Text of the newest SYSTEM line added since `since` ("" if none).
    std::string systemMessageSince(size_t since) const {
        if (!chat_history) return {};
        for (size_t i = chat_history->size(); i > since; --i) {
            const ChatMessage& msg = (*chat_history)[i - 1];
            if (msg.from == "SYSTEM") return msg.text;
        }
        return {};
    }
    // True if an outgoing (non-SYSTEM) line was added since `since`.
    bool outgoingMessageSince(size_t since) const {
        if (!chat_history) return false;
        for (size_t i = since; i < chat_history->size(); ++i) {
            const ChatMessage& msg = (*chat_history)[i];
            if (msg.is_outgoing && msg.from != "SYSTEM") return true;
        }
        return false;
    }

    // QSO duration "HH:MM:SS" ("--:--:--" while no QSO is running).
    // File type helpers live in gui_util.hpp (gui::isImageFile,
    // gui::fileTypeDescription).
    std::string formatQSODuration() const;
};

} // namespace skylink
