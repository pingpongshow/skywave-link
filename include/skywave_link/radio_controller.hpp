// RadioController - local radio control and TCP PTT service

#pragma once

#include "config.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace skylink {

// The modem's default PTT TCP port (INTERFACES.md "PTT": `PTTPORT
// host[:port]`, default port 4532).
inline constexpr uint16_t kDefaultPttServerPort = 4532;

// What the controller should do for `source` (the user's radio settings):
// backend "hamlib" uses the configured Hamlib model and device; backend
// "kenwood_tcp" drives a Kenwood rig over its LAN port with a Kenwood Hamlib
// model (the configured one when it is a Kenwood model, else the TS-890S)
// and `device = host:port`; anything else disables radio control.
RadioConfig resolveRadioConfig(const RadioConfig& source);

class RadioController {
public:
    struct Status {
        bool configured = false;
        // No Hamlib model selected (model 0): nothing is controlled. Every
        // frequency/mode/PTT request fails with last_error saying so, and
        // the PTT server answers KEY with an error.
        bool dry_run = false;
        bool connected = false;
        bool ptt = false;
        uint64_t frequency_hz = 0;
        std::string mode = "USB";
        std::string device;
        int hamlib_model = 0;
        bool ptt_server_running = false;
        std::string ptt_bind;
        uint16_t ptt_port = 0;
        std::string last_error;
    };

    RadioController();
    ~RadioController();

    RadioController(const RadioController&) = delete;
    RadioController& operator=(const RadioController&) = delete;

    void applyConfig(const RadioConfig& config);
    bool connect();
    void disconnect();

    bool isConnected() const;
    bool isDryRun() const;

    bool setFrequency(uint64_t hz);
    std::optional<uint64_t> getFrequency();

    bool setMode(const std::string& mode);
    std::optional<std::string> getMode();

    bool setPtt(bool enabled);
    bool getPtt();

    bool startPttServer();
    void stopPttServer();

    // Called (from the PTT server's client thread) whenever the modem asks
    // to key (true) or unkey (false), and with false when a client that
    // keyed disconnects. Reports the modem's keying intent even when the
    // rig itself could not be keyed. Set before startPttServer().
    void onPttRequest(std::function<void(bool keyed)> cb);

    Status getStatus() const;

private:
    void setErrorLocked(const std::string& message);
    bool ensureConnectedLocked();
    bool connectLocked();
    void disconnectLocked();

    bool setFrequencyLocked(uint64_t hz);
    std::optional<uint64_t> getFrequencyLocked();
    bool setModeLocked(const std::string& mode);
    std::optional<std::string> getModeLocked();
    bool setPttLocked(bool enabled);
    bool getPttLocked();

    bool startPttServerLocked();
    void stopPttServerLocked();
    void pttServerLoop();
    void pttClientLoop(int client_fd);

    RadioConfig config_;
    bool dry_run_ = false;
    bool connected_ = false;
    bool ptt_on_ = false;
    uint64_t frequency_hz_ = 0;
    std::string mode_ = "USB";
    std::string last_error_;

    void* rig_ = nullptr;

    std::atomic<bool> ptt_server_stop_{false};
    bool ptt_server_running_ = false;
    int ptt_listener_fd_ = -1;
    std::thread ptt_server_thread_;
    std::vector<std::thread> ptt_client_threads_;
    std::vector<int> ptt_client_fds_;  // open client sockets (for shutdown on stop)
    std::mutex ptt_clients_mutex_;     // guards ptt_client_threads_ / ptt_client_fds_
    std::mutex ptt_lifecycle_mutex_;   // serializes start/stop; taken before mutex_
    std::mutex ptt_callback_mutex_;
    std::function<void(bool)> ptt_request_callback_;
    void notifyPttRequest(bool keyed);

    mutable std::mutex mutex_;
};

} // namespace skylink
