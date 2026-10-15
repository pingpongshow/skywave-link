// RadioController - local radio control and TCP PTT service

#pragma once

#include "config.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace skylink {

class RadioController {
public:
    struct Status {
        bool configured = false;
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
    bool reconnect();

    bool isConfigured() const;
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

    mutable std::mutex mutex_;
};

} // namespace skylink
