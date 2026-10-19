#pragma once
#include "window_base.hpp"
#include <functional>
#include <memory>
#include <string>

namespace skylink {

class SettingsWindow : public WindowBase {
public:
    explicit SettingsWindow(SharedAppState& state);
    void render() override;
    const char* name() const override { return "Settings"; }

    // Callbacks wired by App
    std::function<void()> onConnectModem;
    std::function<void()> onDisconnectModem;
    std::function<void()> onApplyModemConfig;
    std::function<void(const std::string&)> onSetModemFamily;
    std::function<void(const std::string&)> onApplyCallsign;
    // Optional: saves the configuration and returns false with `error` set
    // on failure. Unset, the window writes AppConfig to
    // AppConfig::getDefaultConfigPath() itself.
    std::function<bool(std::string& error)> onSaveSettings;
    std::function<bool(const std::string&, int)> onOpenGPSPort;
    std::function<void()> onCloseGPSPort;
    std::function<void()> onStartEmailGateway;
    std::function<void()> onStopEmailGateway;
    std::function<void()> onApplyRadioConfig;
    // APRS Apply: (re)connect or disconnect APRS-IS with the new server,
    // interval and filter (App; G14). Unset, the window says the change
    // applies after a restart.
    std::function<void()> onApplyAPRSConfig;
    std::function<void()> onApplyVoiceConfig;
    std::function<void()> onVoiceStart;
    std::function<void()> onVoiceStop;
    std::function<void(bool)> onVoicePtt;
    std::function<void()> onBeginVoiceHotkeyCapture;
    std::function<void()> onCancelVoiceHotkeyCapture;
    std::function<void()> onClearVoiceHotkey;
    std::function<bool()> isVoiceHotkeyCaptureActive;
    std::function<void()> onRefreshVoiceAudio;
    std::function<void()> onSendBeacon;
    std::function<void()> onEnableAwayMode;
    std::function<void()> onDisableAwayMode;
    std::function<void()> onUploadPSKReporterSpots;
    PathPicker onBrowseDownloadPath;
    // Re-applies the theme/font from config.ui. Unset, the window calls
    // ui::applyModernStyle itself.
    std::function<void()> onSetupStyle;

    // APRS-IS receive filter (K14). App points this at the config field
    // once it exists; while null the field is hidden.
    std::string* aprs_filter = nullptr;

    // Beacon timer reference (set by App so the countdown can be displayed)
    const std::chrono::steady_clock::time_point* last_beacon_sent = nullptr;

    // Brings the tab labelled `name` ("Voice", "Winlink", ...) to the
    // front on the next frame.
    void selectTab(const std::string& name) { select_tab_ = name; }

private:
    std::string select_tab_;
    // Writes the config file; reports the outcome on the status line.
    // `what` names the applied section ("" for the Save button).
    bool saveSettings(const std::string& what);
    // MODE / MAXMODE / CWID go straight to the modem (K25); the answer is
    // reported on the status line.
    void sendModemControl(const std::string& command, const std::string& what);
    void queryModemControls();
    void parseModemControlReply(const std::string& response);

    bool modem_controls_queried_ = false;
    int modem_cwid_ = -1;              // -1 unknown, 0 off, 1 on
    std::string modem_max_mode_;       // "" unknown
    // Guards modem-command callbacks that outlive this window.
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace skylink
