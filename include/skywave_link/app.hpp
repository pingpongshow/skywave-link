// Skywave Link Application - Main application class

#pragma once

#include <fstream>

#include "config.hpp"
#include "modem_client.hpp"
#include "message_store.hpp"
#include "freq_manager.hpp"
#include "radio_controller.hpp"
#include "aprs_client.hpp"
#include "file_transfer.hpp"
#include "mesh_relay.hpp"
#include "shared_state.hpp"
#include "window_base.hpp"
#include "email_gateway_job.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <map>
#include <deque>
#include <mutex>
#include <thread>
#include <future>
#include <unordered_map>

// Forward declarations for ImGui
struct SDL_Window;
typedef struct SDL_KeyboardEvent SDL_KeyboardEvent;
typedef void* SDL_GLContext;

namespace skylink {

// Pure receive-path policy helpers used by App (header-only so they can be
// unit tested without the GUI).
namespace app_detail {

// Payload types a station may send while unlinked (BCAST_DATA path). The
// sender is the broadcast's source as reported by the modem.
inline bool isDisconnectedBroadcastPayload(uint8_t msg_type) {
    switch (msg_type) {
        case 0xFB:  // Text broadcast
        case 0xFC:  // CQ
        case 0xFA:  // Advanced beacon
        case 0xF4:  // Position beacon
        case 0xF9:  // Pending-message notification
        case 0xE3:  // Legacy broadcast mail
        case 0xE0:  // Ping request (addressed broadcast)
        case 0xE1:  // Ping response (addressed broadcast)
        case 0xE5:  // Mesh relay packet
        case 0xE7:  // Reliable broadcast mail
        case 0xD0:  // BBS announce
        case 0xD1:  // BBS remove
            return true;
        default:
            return false;
    }
}

// Payload types that act on the current session and are honoured only
// when they arrive over the link from the connected peer.
inline bool isSessionOnlyPayload(uint8_t msg_type) {
    if (msg_type >= 0x10 && msg_type <= 0x18) {  // File transfer
        return true;
    }
    switch (msg_type) {
        case 0xD2:  // BBS file request
        case 0xF1:  // QSY confirm
        case 0xF2:  // QSY suggestion
        case 0xF5:  // Position relay request (relayed to APRS-IS)
        case 0xF6:  // Away-mode request
        case 0xE8:  // Mail ACK/NACK
            return true;
        default:
            return false;
    }
}

// Inner payloads a mesh relay packet addressed to us may deliver: chat
// text and mail only. Never another relay packet or a control type.
inline bool isMeshDeliverablePayload(uint8_t msg_type) {
    switch (msg_type) {
        case 0xE2:  // Legacy private mail
        case 0xE3:  // Legacy broadcast mail
        case 0xE6:  // Reliable private mail
        case 0xE7:  // Reliable broadcast mail
        case 0xFB:  // Text broadcast
            return true;
        default:
            // Plain chat text starts with a printable ASCII character.
            return (msg_type >= 0x20 && msg_type < 0x7F) ||
                   msg_type == '\t' || msg_type == '\n' || msg_type == '\r';
    }
}

// Prefix for chat/mail a mesh relay delivered to us. The source callsign
// comes from the relay header, which the relaying station can set freely,
// so it is shown as unverified alongside the station that relayed it.
inline std::string relayAttributionMarker(const std::string& relayed_by) {
    if (relayed_by.empty()) {
        return "";
    }
    return "[relayed via " + relayed_by + ", sender unverified] ";
}

inline bool isValidLatLon(double lat, double lon) {
    return std::isfinite(lat) && std::isfinite(lon) &&
           lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
}

// Remote text headed for a line-oriented service (APRS-IS): drop CR/LF and
// every other control character, then bound the length.
inline std::string sanitizeRemoteText(const std::string& text, size_t max_len) {
    std::string out;
    out.reserve(std::min(text.size(), max_len));
    for (unsigned char c : text) {
        if (out.size() >= max_len) {
            break;
        }
        if (c < 0x20 || c == 0x7F) {
            continue;
        }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

inline bool containsControlChars(const std::string& text) {
    for (unsigned char c : text) {
        if (c < 0x20 || c == 0x7F) {
            return true;
        }
    }
    return false;
}

// RFC 3986 percent-encoding of everything outside the unreserved set.
inline std::string urlEncode(const std::string& text) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (unsigned char c : text) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                c == '.' || c == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

// Per-sender reassembly of 0xFD chat fragments. A message is identified by
// its total fragment count and starts afresh whenever fragment 0 arrives
// again or the count changes; incomplete messages expire.
class ChatFragmentAssembler {
public:
    static constexpr int64_t kTimeoutSec = 120;
    static constexpr size_t kMaxSenders = 32;

    // Returns true and fills *complete when `from`'s message is finished.
    bool add(const std::string& from, uint8_t frag_idx, uint8_t total_frags,
             const std::string& data, int64_t now_sec, std::string* complete) {
        if (from.empty() || from == "UNKNOWN" || total_frags == 0 || frag_idx >= total_frags) {
            return false;
        }
        expire(now_sec);
        auto it = partial_.find(from);
        if (it != partial_.end()) {
            Partial& p = it->second;
            const bool restart = p.total != total_frags ||
                                 (frag_idx == 0 && p.frags.count(0) > 0);
            if (restart) {
                partial_.erase(it);
                it = partial_.end();
            }
        }
        if (it == partial_.end()) {
            if (partial_.size() >= kMaxSenders) {
                evictOldest();
            }
            Partial fresh;
            fresh.total = total_frags;
            fresh.started = now_sec;
            it = partial_.emplace(from, std::move(fresh)).first;
        }
        Partial& p = it->second;
        p.last = now_sec;
        if (p.frags.count(frag_idx) > 0) {
            return false;  // Duplicate fragment
        }
        p.frags[frag_idx] = data;
        if (p.frags.size() < p.total) {
            return false;
        }
        std::string full;
        for (const auto& frag : p.frags) {
            full += frag.second;
        }
        partial_.erase(it);
        if (complete) {
            *complete = std::move(full);
        }
        return true;
    }

    void expire(int64_t now_sec) {
        for (auto it = partial_.begin(); it != partial_.end();) {
            if (now_sec - it->second.last > kTimeoutSec) {
                it = partial_.erase(it);
            } else {
                ++it;
            }
        }
    }

    size_t pendingSenders() const { return partial_.size(); }

private:
    struct Partial {
        uint8_t total = 0;
        int64_t started = 0;
        int64_t last = 0;
        std::map<uint8_t, std::string> frags;
    };

    void evictOldest() {
        auto oldest = partial_.begin();
        for (auto it = partial_.begin(); it != partial_.end(); ++it) {
            if (it->second.last < oldest->second.last) {
                oldest = it;
            }
        }
        if (oldest != partial_.end()) {
            partial_.erase(oldest);
        }
    }

    std::map<std::string, Partial> partial_;
};

}  // namespace app_detail

// Forward declarations for modular windows
class ChatWindow;
class FileWindow;
class MessageWindow;
class PositionWindow;
class SettingsWindow;
class WeatherWindow;
class MailWindow;
class BBSWindow;
class FrequencyWindow;
class BeaconPanel;
class PingPanel;
class MainContentRenderer;
class StationMapWindow;

class App {
public:
    App();
    ~App();

    // Non-copyable
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Lifecycle
    bool initialize();
    void run();
    void shutdown();

    // Configuration
    AppConfig& config() { return config_; }
    const AppConfig& config() const { return config_; }

private:
    // SDL/OpenGL/ImGui
    SDL_Window* window_ = nullptr;
    SDL_GLContext gl_context_ = nullptr;

    // Core components
    AppConfig config_;
    std::unique_ptr<ModemClient> modem_;
    std::unique_ptr<RadioController> radio_;
    std::unique_ptr<MessageStore> store_;
    std::unique_ptr<FrequencyManager> freq_mgr_;
    std::unique_ptr<APRSClient> aprs_;
    std::unique_ptr<FileTransferManager> file_transfer_mgr_;
    std::unique_ptr<MeshRelay> mesh_relay_;

    // Shared state for modular windows
    SharedAppState shared_state_;

    // Modular windows
    std::unique_ptr<ChatWindow> chat_window_;
    std::unique_ptr<FileWindow> file_window_;
    std::unique_ptr<MessageWindow> message_window_;
    std::unique_ptr<PositionWindow> position_window_;
    std::unique_ptr<SettingsWindow> settings_window_;
    std::unique_ptr<WeatherWindow> weather_window_;
    std::unique_ptr<MailWindow> mail_window_;
    std::unique_ptr<BBSWindow> bbs_window_;
    std::unique_ptr<FrequencyWindow> frequency_window_;
    std::unique_ptr<BeaconPanel> beacon_panel_;
    std::unique_ptr<PingPanel> ping_panel_;
    std::unique_ptr<MainContentRenderer> main_content_;
    std::unique_ptr<StationMapWindow> station_map_window_;

    // UI state
    AppTab current_tab_ = AppTab::Chat;
    bool show_settings_ = false;
    bool running_ = true;
    bool shutdown_done_ = false;
    bool imgui_initialized_ = false;
    std::chrono::steady_clock::time_point app_start_time_;
    int auto_exit_after_ms_ = 0;

    // Chat state
    std::vector<ChatMessage> chat_history_;
    char chat_input_[1024] = {0};
    std::string current_chat_partner_;
    bool chat_scroll_to_bottom_ = false;

    // Typing indicator
    bool remote_is_typing_ = false;
    std::chrono::steady_clock::time_point last_typing_indicator_;
    std::chrono::steady_clock::time_point last_typing_sent_;
    bool last_input_was_empty_ = true;
    static constexpr int TYPING_INDICATOR_TIMEOUT_MS = 3000;
    static constexpr int TYPING_INDICATOR_INTERVAL_MS = 1500;

    static constexpr size_t MAX_CHAT_HISTORY = 2000;

    // Message fragmentation
    static constexpr size_t MAX_CHAT_FRAGMENT_SIZE = 200;
    app_detail::ChatFragmentAssembler fragment_assembler_;
    int outgoing_fragment_counter_ = 0;

    // File transfer state
    std::vector<FileTransfer> transfers_;
    int unread_file_notifications_ = 0;
    char file_path_input_[512] = {0};

    // Message state
    std::vector<Message> inbox_messages_;
    std::vector<Message> outbox_messages_;
    int selected_message_ = -1;

    // Position state
    double my_lat_ = 0.0;
    double my_lon_ = 0.0;
    char position_comment_[256] = {0};

    // Settings state
    char callsign_input_[32] = {0};
    char modem_host_input_[256] = {0};
    // Last dial frequency pushed to the modem, so FREQINFO is sent on
    // change rather than every poll.
    uint32_t last_reported_dial_hz_ = 0;
    int modem_port_input_ = kDefaultCommandPort;
    int voice_port_input_ = kDefaultVoicePort;
    char voice_capture_device_input_[256] = {0};
    char voice_playback_device_input_[256] = {0};

    // Live voice state
    uint32_t voice_status_generation_ = 0;
    bool voice_auto_started_ = false;
    bool voice_start_pending_ = false;
    bool voice_start_inflight_ = false;
    bool voice_local_ptt_ = false;
    bool voice_ptt_after_start_ = false;
    bool voice_ui_ptt_active_ = false;
    bool voice_hotkey_capture_active_ = false;
    bool voice_hotkey_ptt_active_ = false;
    uint32_t voice_playback_samples_queued_ = 0;
    bool voice_playback_started_ = false;
    std::deque<int16_t> voice_playback_buffer_;
    std::chrono::steady_clock::time_point last_voice_status_poll_;
    std::chrono::steady_clock::time_point voice_start_attempted_at_{};
    uint32_t voice_capture_device_id_ = 0;
    uint32_t voice_playback_device_id_ = 0;
    bool voice_audio_ready_ = false;
    static constexpr size_t VOICE_PLAYBACK_PREBUFFER_SAMPLES = 3200;
    static constexpr size_t VOICE_PLAYBACK_REBUFFER_SAMPLES = 1600;
    static constexpr size_t VOICE_PLAYBACK_MIN_CHUNK_SAMPLES = 800;

    // CQ/Broadcast state
    bool cq_active_ = false;
    int cq_repeat_count_ = 1;
    int cq_current_repeat_ = 0;
    std::chrono::steady_clock::time_point last_cq_sent_;
    static constexpr int CQ_INTERVAL_MS = 10000;
    char broadcast_message_[512] = {0};
    std::deque<std::pair<std::string, std::string>> heard_stations_;
    static constexpr size_t MAX_HEARD_STATIONS = 50;

    // Channel sniffer state
    bool channel_sniffer_active_ = false;
    int sniffer_current_slot_ = 0;
    std::string sniffer_band_ = "20m";
    std::chrono::steady_clock::time_point sniffer_slot_start_;
    static constexpr int SNIFFER_DWELL_MS = 5000;
    std::vector<std::pair<int, bool>> slot_status_;

    // GPS state
#ifdef _WIN32
    void* gps_handle_ = nullptr;
#else
    int gps_serial_fd_ = -1;
#endif
    std::string gps_port_;
    int gps_baud_ = 9600;
    std::string gps_buffer_;
    bool gps_fix_valid_ = false;
    std::chrono::steady_clock::time_point last_gps_update_;

    // Email gateway state
    std::vector<Message> email_outbox_;
    std::vector<Message> email_inflight_;   // being delivered by email_worker_
    bool email_send_requested_ = false;
    EmailGatewayWorker email_worker_;
    std::vector<Message> radio_outbox_;
    bool email_gateway_active_ = false;
    std::chrono::steady_clock::time_point last_email_check_;
    std::chrono::steady_clock::time_point last_pop3_check_;
    std::chrono::steady_clock::time_point last_winlink_check_;

    // BBS state
    struct PendingRemoteBbsDownload {
        std::string filename;
        std::string owner_callsign;
        int64_t requested_at = 0;
        bool request_sent = false;
    };
    std::vector<BBSFile> bbs_files_;
    std::unordered_map<std::string, BBSFile> remote_bbs_files_;
    static constexpr size_t MAX_REMOTE_BBS_FILES = 500;
    std::unordered_map<std::string, PendingRemoteBbsDownload> pending_remote_bbs_downloads_;
    std::vector<PendingBbsDownload> pending_bbs_downloads_ui_;
    static constexpr int64_t REMOTE_BBS_DOWNLOAD_REQUEST_TTL_SEC = 180;
    int selected_bbs_file_ = -1;
    char bbs_upload_path_[512] = {0};
    char bbs_file_desc_[256] = {0};

    // Frequency management state
    std::string current_band_ = "20m";
    int current_slot_ = 0;
    bool qsy_suggestion_pending_ = false;
    std::string qsy_suggestion_from_;
    std::string qsy_suggestion_band_;
    int qsy_suggestion_slot_ = 0;
    uint32_t qsy_suggestion_frequency_hz_ = 0;
    int64_t qsy_suggestion_received_unix_ = 0;
    enum class QsyWorkflowState {
        Idle,
        AwaitingAck,
        SidebandActive,
        ReconnectPending,
        ReconnectConnecting
    };
    QsyWorkflowState qsy_workflow_state_ = QsyWorkflowState::Idle;
    bool qsy_initiator_ = false;
    bool pending_connect_auto_qsy_ = false;
    std::string qsy_workflow_peer_;
    std::string pending_connect_target_;
    std::string qsy_target_band_;
    int qsy_target_slot_ = 0;
    uint32_t qsy_target_frequency_hz_ = 0;
    int qsy_reconnect_attempts_ = 0;
    std::chrono::steady_clock::time_point qsy_ack_deadline_;
    std::chrono::steady_clock::time_point qsy_sideband_started_;
    std::chrono::steady_clock::time_point qsy_reconnect_due_;
    std::chrono::steady_clock::time_point qsy_connect_attempt_started_;
    static constexpr int QSY_ACK_TIMEOUT_MS = 8000;
    static constexpr int QSY_RECONNECT_DELAY_MS = 2500;
    static constexpr int QSY_RECONNECT_CONNECT_TIMEOUT_MS = 12000;
    static constexpr int QSY_RECONNECT_WINDOW_MS = 15000;
    static constexpr int QSY_MAX_RECONNECT_ATTEMPTS = 3;

    // Beacon state
    std::deque<HeardBeacon> heard_beacons_;
    std::unordered_map<std::string, int64_t> recent_cq_unix_;
    static constexpr size_t MAX_HEARD_BEACONS = 100;
    std::chrono::steady_clock::time_point last_beacon_sent_;
    bool beacon_enabled_ = false;
    std::vector<StationLocationEntry> station_locations_;
    static constexpr size_t MAX_STATION_LOCATIONS = 250;

    // QSO state
    std::chrono::steady_clock::time_point qso_start_time_;
    bool qso_active_ = false;
    float remote_snr_db_ = 0.0f;
    float local_snr_db_ = 0.0f;
    bool snr_exchanged_ = false;
    uint16_t snr_report_seq_ = 0;
    std::chrono::steady_clock::time_point last_snr_exchange_sent_;
    std::chrono::steady_clock::time_point last_remote_snr_update_;
    std::chrono::steady_clock::time_point next_snr_keepalive_due_;
    static constexpr bool SNR_KEEPALIVE_ENABLED = false;
    static constexpr int SNR_KEEPALIVE_INTERVAL_MS = 20000;
    static constexpr int SNR_KEEPALIVE_TX_GUARD_MS = 3000;
    static constexpr int SNR_KEEPALIVE_LEAD_START_MS = 3000;
    static constexpr int SNR_KEEPALIVE_FOLLOW_START_MS = 13000;

    // Away mode state
    bool away_mode_active_ = false;

    // Ping state
    std::deque<PingResult> ping_results_;
    static constexpr size_t MAX_PING_RESULTS = 50;
    bool ping_in_progress_ = false;
    std::string ping_target_;
    std::chrono::steady_clock::time_point ping_sent_time_;
    static constexpr int PING_TIMEOUT_MS = 120000;  // two broadcast frames at a robust mode

    // Mail state
    std::vector<MailMessage> mail_inbox_;
    std::vector<MailMessage> mail_outbox_;
    // Legacy 0xE2/0xE3 mail has no id: sender|content-hash -> first seen.
    std::unordered_map<std::string, int64_t> legacy_mail_seen_;
    static constexpr int64_t LEGACY_MAIL_DEDUP_WINDOW_SEC = 600;
    char mail_to_input_[256] = {0};
    char mail_subject_input_[128] = {0};
    char mail_body_input_[2048] = {0};
    int mail_priority_input_ = 0;  // 0=routine, 1=priority, 2=emergency
    int selected_mail_ = -1;
    static constexpr int MAIL_ACK_TIMEOUT_MS = 15000;
    static constexpr int MAIL_MAX_RETRIES = 3;
    static constexpr int GATEWAY_CONNECT_RETRY_MS = 10000;
    std::chrono::steady_clock::time_point last_gateway_connect_attempt_;
    std::string auto_gateway_target_;
    struct GatewayStats {
        float snr_accum = 0.0f;
        int snr_samples = 0;
        int relay_success = 0;
        int relay_fail = 0;
        int64_t last_seen = 0;
    };
    std::map<std::string, GatewayStats> gateway_stats_;
    RelayDiagnostics relay_diagnostics_;

    // Timing
    std::chrono::steady_clock::time_point last_poll_;
    std::chrono::steady_clock::time_point last_status_update_;

    // Initialization
    bool initSDL();
    bool initImGui();
    bool initVoiceAudio();
    void shutdownVoiceAudio();
    void pumpVoiceAudio();
    void applyVoiceSettingsToModem(bool restart_if_open);
    void applyModemSettingsToModem();
    bool setVoicePttState(bool enabled);
    bool handleVoiceHotkeyEvent(const SDL_KeyboardEvent& key, bool pressed);
    bool openVoiceAudioDevices();
    void closeVoiceAudioDevices();
    void setupStyle();
    void initializeWindows();
    void initializeSharedState();

    // Main loop
    void processEvents();
    void update();
    // Scripted test hook (SKYLINK_TEST_SCRIPT=<file>): runs one action
    // per line through the app's own methods from the update loop, so
    // end-to-end tests drive exactly the code the GUI drives. See
    // docs/TESTING_HOOK.md.
    void loadTestScript();
    void runTestScript();
    void dumpTestReport(const std::string& path);
    std::vector<std::string> test_script_;
    size_t test_pc_ = 0;
    std::chrono::steady_clock::time_point test_wait_until_{};
    std::string test_wait_cond_;
    std::chrono::steady_clock::time_point test_cond_deadline_{};
    std::ofstream test_log_;
    void testLog(const std::string& line);
    std::string test_shot_path_;  // "screenshot" action: read back next frame
    void render();

    // UI rendering
    void renderMenuBar();
    void renderStatusBar();

    // Where a received payload came from. Only Link traffic is from the
    // connected peer; Broadcast carries the modem-reported source; Mesh is
    // an inner payload delivered by a relay packet.
    enum class RxOrigin { Link, Broadcast, Mesh };

    // Event handlers
    void onModemStateChange(const std::string& state);
    void onDataReceived(const std::vector<uint8_t>& data);
    void onMonitorFrame(const MonitorFrame& frame);
    // Fires once a BCAST_DATA fragment chain has been reassembled.
    void onBroadcastMessage(const std::string& sender,
                            const std::vector<uint8_t>& payload);
    void onConnected(const std::string& callsign);
    void onDisconnected();

    // Actions
    void connectToModem();
    void pollModemConnect();   // finishes a background connectToModem()
    void pollAPRSConnect();    // finishes a background APRS-IS login
    // Native pickers run on a helper thread; the result is handed back to
    // the GUI thread by pollPathPicker(). One picker at a time.
    std::string imgui_ini_path_;   // must outlive the ImGui context
    struct PathPickerJob;
    std::shared_ptr<PathPickerJob> path_picker_job_;
    PathPickedCallback path_picker_done_;
    void startPathPicker(bool folder, PathPickedCallback done);
    void pollPathPicker();
    void onModemConnected();
    void disconnectFromModem();
    void connectToStation(const std::string& callsign);
    void sendChatMessage(const std::string& text);
    void sendFile(const std::string& path);
    void updatePosition(double lat, double lon);
    void applyRadioConfig();

    // BBS functions
    void refreshBBSFiles();
    void uploadToBBS(const std::string& filepath, const std::string& description);
    void downloadFromBBS(const BBSFile& file);
    void downloadFromBBS(const std::string& filename);
    void deleteBBSFile(const std::string& filename);
    void announceBBSFile(const BBSFile& file);
    void announceBBSFileRemoval(const std::string& filename);
    void processBBSAnnouncement(const std::string& from, const std::vector<uint8_t>& data);
    void processBBSRemoval(const std::string& from, const std::vector<uint8_t>& data);
    void processBBSFileRequest(const std::string& from, const std::vector<uint8_t>& data);
    void requestRemoteBBSDownload(const std::string& from, const std::string& filename);
    bool sendRemoteBbsFileRequest(const std::string& owner, const std::string& filename, bool emit_chat);
    bool flushPendingRemoteBbsDownloadsForPeer(const std::string& peer, bool emit_chat = true);
    void prunePendingRemoteBbsDownloads();
    bool shouldAcceptRemoteBbsOffer(const std::string& from, const std::string& filename);
    void syncPendingBbsDownloadsForUi();
    void upsertRemoteBBSFile(const std::string& from, const BBSFile& file);
    void removeRemoteBBSFile(const std::string& from, const std::string& filename);

    // Message handling
    void refreshMessages();
    void sendPendingMessages();
    void processIncomingMessage(const std::vector<uint8_t>& data, const std::string& from_override = "",
                                RxOrigin origin = RxOrigin::Link, int relay_depth = 0,
                                const std::string& relayed_by = "");
    bool isLinkConnected() const;
    std::string linkedPeerCallsign() const;
    void appendSystemChat(const std::string& text);
    // Deferred incoming file offers (Incoming File prompt, test hook).
    bool decidePendingFileOffer(const std::string& transfer_id, bool accept);
    bool decideNewestPendingFileOffer(bool accept);
    void dropPendingFileOffers();
    void renderPendingFileOffers();
    void pruneFragmentBuffers();
    void gracefulDisconnectModem();

    // Chat typing and fragmentation
    void sendTypingIndicator();
    void processTypingIndicator(const std::string& from);
    void sendFragmentedMessage(const std::string& text);
    void processMessageFragment(const std::string& from, const std::vector<uint8_t>& data);

    // CQ and broadcast
    void startCQ();
    void stopCQ();
    void updateCQ();
    void sendBroadcast(const std::string& message);
    void processCQResponse(const std::string& from);
    bool sendBroadcastPayload(const std::vector<uint8_t>& payload);
    void sendQsySuggestion(const std::string& band, int slot, uint32_t frequency_hz);
    void processQsySuggestion(const std::string& from, const std::vector<uint8_t>& data);
    void processQsyConfirm(const std::string& from, const std::vector<uint8_t>& data);
    void respondToQsySuggestion(bool accept);
    void updateQsyWorkflow();
    void resetQsyWorkflow(bool clear_peer);
    bool executeQsyTarget(const std::string& band, int slot, uint32_t frequency_hz);
    bool qsyTargetInBand(const std::string& band, int slot, uint32_t frequency_hz) const;
    void returnToCallingFrequency(const std::string& reason);
    bool buildAutoQsyTarget(std::string* band_out, int* slot_out, uint32_t* freq_hz_out) const;
    void addHeardStation(const std::string& callsign);

    // Channel sniffer
    void startChannelSniffer();
    void stopChannelSniffer();
    void updateChannelSniffer();
    bool tuneOperatingFrequency(uint64_t freq_hz, bool update_selection = true);

    // GPS
    bool openGPSPort(const std::string& port, int baud);
    void closeGPSPort();
    void updateGPS();
    void processGPSData();
    void parseNMEASentence(const std::string& sentence);

    // Email gateway
    void startEmailGateway();
    void stopEmailGateway();
    void updateEmailGateway();
    void processIncomingEmail();
    void relayEmailToRadio(const Message& email);
    void keepInboundLocally(const std::string& from, const std::string& to, const std::string& subject,
                            const std::string& body, const std::string& message_id);
    void relayRadioToEmail(const Message& msg);
    // Gateway network I/O runs on email_worker_; these run on the GUI thread.
    bool startEmailGatewayJob(bool poll_pop3, bool poll_winlink, bool send_outbox);
    void pollEmailGatewayJob();
    void applyInboundEmail(const EmailGatewayJobOutput& output);
    void prepareQueuedEmails(EmailGatewayJobInput& input);
    void applyEmailSendResults(const EmailGatewayJobOutput& output);
    void sendQueuedEmails();   // starts (or requests) a delivery job

    // Beacon system
    void sendBeacon();
    void updateBeacon();
    void processReceivedBeacon(const std::string& from, const std::string& grid_square,
                                  const std::string& message, float snr, bool email_gateway = false);
    void checkPendingMessagesForStation(const std::string& callsign);
    void recordStationLocation(const std::string& callsign,
                               bool has_coordinates,
                               double latitude,
                               double longitude,
                               const std::string& grid_square,
                               const std::string& details,
                               StationLocationSource source,
                               float snr = 0.0f,
                               bool email_gateway = false,
                               int64_t timestamp = 0);
    void seedStationLocationsFromHistory();
    void onAprsPositionReceived(const APRSPosition& pos);

    // QSO management
    void startQSO(const std::string& callsign);
    void endQSO();
    void exchangeSNR(bool force = false);
    void processSNRReport(float remote_snr);
    void updateSNRKeepAlive();
    bool canSendSNRKeepAlive() const;
    std::string formatQSODuration() const;

    // Away mode
    void enableAwayMode();
    void disableAwayMode();
    void processAwayModeRequest(const std::string& from, const std::vector<uint8_t>& data);
    void sendAwayModeResponse(const std::string& to, const std::string& response);

    // Ping
    void sendPing(const std::string& callsign);
    void processPingRequest(const std::string& from, const std::vector<uint8_t>& data);
    void processPingResponse(const std::string& from, const std::vector<uint8_t>& data);
    void updatePing();

    // Mail
    void sendMail(const std::string& to, const std::string& subject, const std::string& body);
    void sendBroadcastMail(const std::string& subject, const std::string& body);
    void processMailReceived(const std::string& from, const std::vector<uint8_t>& data, bool via_link,
                             const std::string& relayed_by = "");
    void processReliableMailReceived(const std::string& from, const std::vector<uint8_t>& data, bool is_broadcast,
                                     bool via_link,
                                     const std::string& relayed_by = "");
    void processMailAck(const std::string& from, const std::vector<uint8_t>& data);
    void sendMailAck(const std::string& message_id, bool ack, const std::string& reason = "");
    void updateReliableMail();
    void processQueuedGatewayMail();
    bool sendMailWirePacket(const MailMessage& mail);
    void recordGatewayRelayResult(const std::string& gateway_callsign, bool success);
    bool isKnownEmailGateway(const std::string& callsign) const;
    bool isWeakSignalLink(const std::string& callsign) const;
    std::string selectBestGateway(float* score_out = nullptr) const;
    void persistMailReadState(const MailMessage& mail);
    void loadMailFromStore();
    void saveMailToStore();
    bool deleteStoredMessage(int64_t store_id, const std::string& message_id);
    bool forwardPacketViaMesh(const std::vector<uint8_t>& payload,
                              const std::string& destination_callsign,
                              const std::string& message_id = "");
    void applyMeshRelaySettings();
    void flushMeshRelayQueue();

    // File transfer helpers
    void syncTransfersFromManager();
    bool isImageFile(const std::string& filename) const;
    std::string getFileTypeDescription(const std::string& filename) const;

    // Weather functions
    void requestWeatherForecast(const std::string& location);
    void requestMETAR(const std::string& airport_code);
    void requestTAF(const std::string& airport_code);
    void requestMarineWeather(const std::string& zone);
    bool isStationLinkConnected(bool refresh = false);
    void processWeatherMessage(const std::string& from, const std::vector<uint8_t>& data);
    void handleWeatherRequest(uint8_t request_type, const std::string& query, const std::string& requester);
    void sendWeatherResponse(uint8_t type, const std::string& location, const std::string& report);
    void fetchWeatherAsync(uint8_t type, const std::string& query, const std::string& requester);
    static std::string fetchWeatherReport(uint8_t type, const std::string& query);
    void processWeatherFetchResult();

    // Weather helpers
    static bool gridSquareToLatLon(const std::string& grid, double& lat, double& lon);
    static std::string httpGet(const std::string& host, const std::string& path, bool use_ssl = true);
    static std::string parseWeatherResponse(uint8_t type, const std::string& raw_response);
    static std::string extractForecastText(const std::string& json);

    // Weather state
    std::vector<WeatherReport> weather_reports_;
    char weather_location_input_[64] = {0};
    char weather_airport_input_[8] = {0};
    char weather_zone_input_[16] = {0};
    bool weather_request_pending_ = false;
    std::chrono::steady_clock::time_point weather_request_time_;
    static constexpr int WEATHER_TIMEOUT_MS = 30000;

    // Async weather fetch state (exposed to SharedAppState; unused by the
    // remote-request worker below).
    std::mutex weather_fetch_mutex_;
    std::unique_ptr<PendingWeatherFetch> pending_weather_fetch_;

    // Remote weather requests: one worker thread drains a bounded queue,
    // one job per requester. The thread holds a shared_ptr to this state,
    // never to App, so shutdown with a fetch in flight is safe.
    struct WeatherJob {
        uint8_t type = 0;
        std::string query;
        std::string requester;
        std::string result;
    };
    struct WeatherWorker {
        static constexpr size_t kMaxQueued = 4;
        std::mutex mutex;
        std::deque<WeatherJob> queue;
        std::vector<WeatherJob> done;
        std::vector<std::string> rejected;
        std::string active_requester;
        bool running = false;
    };
    std::shared_ptr<WeatherWorker> weather_worker_ = std::make_shared<WeatherWorker>();

    // PSK Reporter state
    std::vector<PSKReporterSpot> psk_reporter_queue_;
    std::mutex psk_reporter_mutex_;
    std::chrono::steady_clock::time_point last_psk_reporter_upload_;
    uint32_t current_frequency_hz_ = 14078000;

    // PSK Reporter functions
    void queuePSKReporterSpot(const std::string& callsign, const std::string& grid,
                               float snr, uint32_t freq_hz = 0);
    void uploadPSKReporterSpots();
    void updatePSKReporter();
    std::vector<std::pair<std::string, float>> computeBandRecommendations() const;
    static std::string generatePSKReporterPayload(const std::string& reporter_call,
                                                   const std::string& reporter_grid,
                                                   const std::string& antenna,
                                                   const std::vector<PSKReporterSpot>& spots);
    static bool sendPSKReporterUDP(const std::string& payload);
};

} // namespace skylink
