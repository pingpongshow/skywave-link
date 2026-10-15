// ModemClient - TCP client for the Skywave HF modem.
//
// Speaks the three localhost ports described in
// Modem/skywave/docs/INTERFACES.md:
//
//   data    9100  application byte stream + 0xFB binary monitor records
//   control 9101  line commands, verb-echo responses, EVT:/MON: notifications
//   voice   9102  framed PCM16LE media records
//
// Protocol notes that shape this class:
//   * a command succeeds when the response echoes the verb; failures are
//     `ERROR:<reason>` (`ERROR:UNSUPPORTED` = feature compiled out,
//     `ERROR:UNKNOWN` = typo, `ERROR:STATE` = wrong FSM state)
//   * asynchronous lines start with `EVT:` or `MON:` and interleave freely
//     with command responses, so every reader dispatches on prefix
//   * the modem emits no periodic SNR event, so link quality is polled
//     from `STATUS`

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>
#include <queue>
#include <deque>
#include <optional>
#include <chrono>

#include "skywave_link/skywave_protocol.hpp"

namespace skylink {

// Default Skywave TCP ports (INTERFACES.md).
inline constexpr uint16_t kDefaultDataPort = 9100;
inline constexpr uint16_t kDefaultCommandPort = 9101;
inline constexpr uint16_t kDefaultVoicePort = 9102;

// Modem connection state
enum class ModemState {
    Disconnected,
    Connecting,
    Connected,
    Error
};

// Session state from the modem FSM. Skywave's IDLE and LISTENING both map
// to Idle here; `ModemStatus::listening` carries the distinction.
enum class LinkState {
    Idle,
    Connecting,
    Connected,
    Disconnecting
};

enum class VoiceMode {
    Off,
    Connected,
    Broadcast
};

// Skywave VoiceFsm states (VOICE.md), named for this app's UI:
//   CLOSED->Idle  OPENING->Opening  OPEN->Ready  REQUESTING->TxRequested
//   TALKING->TxActive  RECEIVING->RxActive
enum class VoiceState {
    Idle,
    Opening,
    Ready,
    TxRequested,
    TxActive,
    RxActive,
    Closing
};

struct VoiceStatus {
    VoiceMode mode = VoiceMode::Off;
    VoiceState state = VoiceState::Idle;
    std::string codec = "OFF";     // AUTO(2400), 700C, 1300, 2400, 3200
    bool ptt = false;
    bool granted = false;
    bool active = false;
    bool local = false;
    bool broadcast = false;
    bool open = false;
    bool ready = false;
    bool rx_loss = false;
    int tx_queued = 0;             // TXQ: queued encoder samples
    int rx_queued = 0;             // RXQ: jitter-buffer samples
    int transport_mode = -1;       // ladder index carrying the voice stream
    std::string last_event = "IDLE";
};

// Snapshot mirroring the modem's `STATUS` line (INTERFACES.md).
struct ModemStatus {
    ModemState state = ModemState::Disconnected;
    LinkState link_state = LinkState::Idle;
    std::string remote_callsign;
    std::string my_callsign;
    float snr_db = 0.0f;             // local RX SNR
    float remote_snr_db = 0.0f;      // peer's report, -99 when unknown
    bool remote_snr_valid = false;
    bool ptt_active = false;
    bool busy_detected = false;
    bool listening = false;
    size_t tx_buffer_bytes = 0;

    std::string family = "WIDE";     // WIDE | MID | NARROW | ULTRA
    std::string mode = "AUTO";       // "M8" when pinned, "AUTO(M8)" otherwise
    int mode_index = 0;              // ladder index, 0..14
    bool mode_pinned = false;
    double net_bps = 0.0;
    float retry_ratio = 0.0f;
    float tx_align_hz = 0.0f;        // CFO: TX frequency-alignment shift
    bool compression = false;
    bool encryption = false;
    int crypto_epoch = 0;
    bool resume = false;
    int masked_carriers = 0;
    uint64_t frames_ok = 0;
    uint64_t frames_failed = 0;

    VoiceStatus voice;
};

struct NoiseFloorReading {
    float noise_dbfs = 0.0f;
    bool busy = false;
};

struct AudioLevels {
    float rx_dbfs = -120.0f;
    float tx_dbfs = -120.0f;
};

struct FrequencyInfo {
    float center_hz = 0.0f;
    float shift_hz = 0.0f;   // TX pre-shift the modem is applying
    float cfo_hz = 0.0f;     // last measured RX carrier offset
};

// A frame observed by the modem's passive monitor. `payload` is the LINK
// FRAME BODY with the 12-byte envelope stripped; `truncated` is set when
// the source was a `MON:` line, which the modem caps at 64 bytes.
struct MonitorFrame {
    std::string src_callsign;
    std::string dst_callsign;
    std::string frame_type;          // "CQ", "BEACON", "BCAST_DATA", ...
    int frame_type_id = 0;
    std::string family;
    int mode = 0;
    std::vector<uint8_t> payload;
    bool truncated = false;
    bool encrypted = false;
};

// Callback types
using StateCallback = std::function<void(const std::string& state)>;
using DataCallback = std::function<void(const std::vector<uint8_t>& data)>;
using SnrCallback = std::function<void(float snr_db)>;
using ConnectedCallback = std::function<void(const std::string& callsign)>;
using DisconnectedCallback = std::function<void()>;
using VoiceStatusCallback = std::function<void(const VoiceStatus&)>;
using MonitorCallback = std::function<void(const MonitorFrame&)>;
// Fires when a broadcast fragment chain (CQ/BEACON/BCAST_DATA) completes.
using BroadcastCallback =
    std::function<void(const std::string& src, const std::vector<uint8_t>&)>;
using EventCallback =
    std::function<void(const std::string& name, const std::string& args)>;

class ModemClient {
public:
    ModemClient();
    ~ModemClient();

    // Non-copyable
    ModemClient(const ModemClient&) = delete;
    ModemClient& operator=(const ModemClient&) = delete;

    // Connection management
    bool connect(const std::string& host,
                 uint16_t cmd_port = kDefaultCommandPort,
                 uint16_t data_port = kDefaultDataPort,
                 uint16_t voice_port = kDefaultVoicePort);
    void disconnect();
    bool isConnected() const;
    // False when the modem was built without ENABLE_VOICE (it never binds
    // the port) or the media socket dropped; the session is unaffected.
    bool hasVoiceMedia() const;
    ModemState getState() const {
        std::lock_guard<std::mutex> lock(status_mutex_);
        return status_.state;
    }
    ModemStatus getStatus() const {
        std::lock_guard<std::mutex> lock(status_mutex_);
        return status_;
    }

    // Polling (call from main loop)
    void poll();

    // Command interface
    bool sendCommand(const std::string& cmd);
    std::optional<std::string> getResponse(int timeout_ms = 100);

    // High-level commands
    bool setCallsign(const std::string& callsign);
    bool connectToStation(const std::string& callsign);
    bool disconnectStation();
    bool abort();
    bool setCompression(bool enabled);
    bool setListenMode(bool enabled);
    bool setMonitorMode(bool enabled);
    bool sendBeacon();
    bool sendBroadcastData(const std::vector<uint8_t>& data);
    bool sendCq();
    bool setCwId(bool enabled);

    // Waveform / ladder control (INTERFACES.md "Mode and Link")
    bool setFamily(const std::string& family);       // WIDE|MID|NARROW|ULTRA
    bool setMode(const std::string& mode);           // AUTO or M4..M14
    bool setMaxMode(const std::string& mode);
    bool setCpMode(const std::string& cp);           // AUTO|SHORT|MEDIUM|LONG
    bool setHarq(const std::string& harq);           // AUTO|CHASE|IR
    bool setAutoMode(bool enabled);

    // Data interface
    bool sendData(const std::vector<uint8_t>& data);
    bool sendText(const std::string& text);
    std::optional<std::vector<uint8_t>> receiveData();
    int64_t millisSinceLastDataSend() const;

    // Query commands
    std::string getVersion();
    size_t getBufferSize();
    bool refreshStatus(int timeout_ms = 500);   // STATUS -> ModemStatus
    std::optional<NoiseFloorReading> getNoiseFloor();
    std::optional<AudioLevels> getAudioLevels();
    std::optional<FrequencyInfo> getFrequencyInfo();
    // Report the rig's dial frequency to the modem (`FREQINFO <hz>`). It
    // keys the modem's channel-memory buckets so its reconnect seeding
    // distinguishes the same peer worked on different bands. 0 clears it.
    bool reportDialFrequency(double hz);
    VoiceStatus getVoiceStatus() const {
        std::lock_guard<std::mutex> lock(status_mutex_);
        return status_.voice;
    }

    // Voice control/media
    bool setVoiceCodec(const std::string& codec);   // AUTO|700C|1300|2400|3200
    bool setVoiceMode(VoiceMode mode);              // VOICE OFF|CONNECTED|BROADCAST
    bool voiceStart();
    bool voiceStop();
    bool setVoicePtt(bool enabled);
    std::optional<VoiceStatus> queryVoiceStatus();
    bool sendVoicePcm(const int16_t* samples, size_t sample_count);
    std::vector<int16_t> receiveVoicePcm(size_t max_samples = 1600);

    // Callbacks
    void onStateChange(StateCallback cb) { state_callback_ = std::move(cb); }
    void onDataReceived(DataCallback cb) { data_callback_ = std::move(cb); }
    void onSnrUpdate(SnrCallback cb) { snr_callback_ = std::move(cb); }
    void onConnected(ConnectedCallback cb) { connected_callback_ = std::move(cb); }
    void onDisconnected(DisconnectedCallback cb) { disconnected_callback_ = std::move(cb); }
    void onVoiceStatus(VoiceStatusCallback cb) { voice_callback_ = std::move(cb); }
    void onMonitorFrame(MonitorCallback cb) { monitor_callback_ = std::move(cb); }
    void onBroadcast(BroadcastCallback cb) { broadcast_callback_ = std::move(cb); }
    void onEvent(EventCallback cb) { event_callback_ = std::move(cb); }

private:
    // Socket handles
#ifdef _WIN32
    using socket_t = unsigned long long;
    static constexpr socket_t INVALID_SOCK = ~0ULL;
#else
    using socket_t = int;
    static constexpr socket_t INVALID_SOCK = -1;
#endif

    socket_t cmd_socket_ = INVALID_SOCK;
    socket_t data_socket_ = INVALID_SOCK;
    socket_t voice_socket_ = INVALID_SOCK;

    // Connection info
    std::string host_;
    uint16_t cmd_port_ = kDefaultCommandPort;
    uint16_t data_port_ = kDefaultDataPort;
    uint16_t voice_port_ = kDefaultVoicePort;

    // Status
    ModemStatus status_;
    mutable std::mutex status_mutex_;

    // Receive buffers
    std::string cmd_buffer_;
    std::vector<uint8_t> data_buffer_;
    std::vector<uint8_t> voice_buffer_;
    std::deque<int16_t> voice_rx_samples_;
    mutable std::mutex voice_mutex_;
    uint32_t voice_tx_sequence_ = 0;
    uint64_t voice_tx_sample_counter_ = 0;

    // Broadcast fragment chains, one per source of monitor evidence.
    BroadcastReassembler mon_bcast_;
    BroadcastReassembler rec_bcast_;

    // Response queue
    std::queue<std::string> response_queue_;
    std::mutex response_mutex_;
    std::atomic<int64_t> last_data_send_ms_{0};

    // Callbacks
    StateCallback state_callback_;
    DataCallback data_callback_;
    SnrCallback snr_callback_;
    ConnectedCallback connected_callback_;
    DisconnectedCallback disconnected_callback_;
    VoiceStatusCallback voice_callback_;
    MonitorCallback monitor_callback_;
    BroadcastCallback broadcast_callback_;
    EventCallback event_callback_;

    // Internal helpers
    bool connectSocket(socket_t& sock, const std::string& host, uint16_t port);
    void closeSocket(socket_t& sock);
    bool setNonBlocking(socket_t sock);
    // A Skywave command succeeded when its response is not `ERROR:...`.
    bool sendCommandOk(const std::string& cmd, int timeout_ms = 500);
    std::optional<std::string> sendCommandForResponse(const std::string& cmd,
                                                      int timeout_ms = 500);
    void clearResponseQueue();
    void processCommandResponse(const std::string& line);
    void handleMonitorLine(const std::string& body);
    void handleEventLine(const std::string& body);
    void applyStatusLine(const std::string& fields);
    void handleMonitorRecord(const std::vector<uint8_t>& record);
    void dispatchMonitorFrame(MonitorFrame frame,
                              const std::vector<uint8_t>& body,
                              BroadcastReassembler& reasm);
    void readCommandSocket();
    void readDataSocket();
    void readVoiceSocket();

    // Platform init
    static bool initSockets();
    static bool sockets_initialized_;
};

} // namespace skylink
