#pragma once
#include "window_base.hpp"
#include <functional>
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
    std::function<void()> onSaveConfig;
    std::function<bool(const std::string&, int)> onOpenGPSPort;
    std::function<void()> onCloseGPSPort;
    std::function<void()> onStartEmailGateway;
    std::function<void()> onStopEmailGateway;
    std::function<void()> onApplyRadioConfig;
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
    std::function<void()> onSetupStyle;

    // Beacon timer reference (set by App so the countdown can be displayed)
    const std::chrono::steady_clock::time_point* last_beacon_sent = nullptr;
};

} // namespace skylink
