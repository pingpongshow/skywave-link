// Settings Window - Extracted from App::renderSettingsTab()

#include "skywave_link/settings_window.hpp"
#include "skywave_link/aprs_client.hpp"
#include "skywave_link/radio_controller.hpp"
#include <SDL.h>
#include <imgui.h>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <vector>
#include <string>
#include <mutex>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif
#include <hamlib/rig.h>
#include <hamlib/riglist.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

// Serial port enumeration (platform-specific)
static std::vector<std::string> enumerateSerialPorts() {
    std::vector<std::string> ports;
#ifdef _WIN32
    for (int i = 1; i <= 256; ++i) {
        std::string name = "COM" + std::to_string(i);
        HANDLE h = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            ports.push_back(name);
            CloseHandle(h);
        }
    }
#elif defined(__APPLE__)
    for (const auto& entry : std::filesystem::directory_iterator("/dev")) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("cu.", 0) == 0 || name.rfind("tty.usb", 0) == 0 ||
            name.rfind("tty.Bluetooth", 0) == 0) {
            ports.push_back(entry.path().string());
        }
    }
#else
    // Linux: /dev/ttyUSB*, /dev/ttyACM*, /dev/ttyS*
    for (const auto& entry : std::filesystem::directory_iterator("/dev")) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("ttyUSB", 0) == 0 || name.rfind("ttyACM", 0) == 0 ||
            name.rfind("ttyS", 0) == 0) {
            ports.push_back(entry.path().string());
        }
    }
#endif
    std::sort(ports.begin(), ports.end());
    return ports;
}

static std::vector<std::string> enumerateAudioDevices(bool capture) {
    std::vector<std::string> devices;
    if ((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0) {
        return devices;
    }

    const int count = SDL_GetNumAudioDevices(capture ? SDL_TRUE : SDL_FALSE);
    for (int i = 0; i < count; ++i) {
        const char* name = SDL_GetAudioDeviceName(i, capture ? SDL_TRUE : SDL_FALSE);
        if (name && *name) {
            devices.emplace_back(name);
        }
    }

    std::sort(devices.begin(), devices.end());
    devices.erase(std::unique(devices.begin(), devices.end()), devices.end());
    return devices;
}

struct HamlibModelEntry {
    int model = 0;
    std::string manufacturer;
    std::string name;
};

static std::string trimCopy(const std::string& input) {
    const auto first = input.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = input.find_last_not_of(" \t\r\n");
    return input.substr(first, last - first + 1);
}

static std::string upperCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

static int collectHamlibModel(const struct rig_caps* caps, rig_ptr_t data) {
    auto* models = static_cast<std::vector<HamlibModelEntry>*>(data);
    if (!models || !caps || caps->rig_model == RIG_MODEL_NONE ||
        !caps->mfg_name || !caps->model_name) {
        return 1;
    }

    HamlibModelEntry entry;
    entry.model = static_cast<int>(caps->rig_model);
    entry.manufacturer = trimCopy(caps->mfg_name);
    entry.name = trimCopy(caps->model_name);
    if (entry.manufacturer.empty()) {
        entry.manufacturer = "Other";
    }
    if (entry.name.empty()) {
        return 1;
    }
    models->push_back(std::move(entry));
    return 1;
}

static const std::vector<HamlibModelEntry>& enumerateHamlibModels() {
    static std::vector<HamlibModelEntry> models;
    static std::once_flag once;

    std::call_once(once, []() {
        rig_load_all_backends();
        rig_list_foreach(collectHamlibModel, &models);
        std::sort(models.begin(), models.end(),
                  [](const HamlibModelEntry& a, const HamlibModelEntry& b) {
                      if (a.manufacturer != b.manufacturer) {
                          return a.manufacturer < b.manufacturer;
                      }
                      if (a.name != b.name) {
                          return a.name < b.name;
                      }
                      return a.model < b.model;
                  });
        models.erase(std::unique(models.begin(), models.end(),
                                 [](const HamlibModelEntry& a, const HamlibModelEntry& b) {
                                     return a.model == b.model;
                                 }),
                     models.end());
    });

    return models;
}

static std::vector<std::string> enumerateHamlibManufacturers() {
    std::vector<std::string> manufacturers;
    for (const auto& model : enumerateHamlibModels()) {
        if (manufacturers.empty() || manufacturers.back() != model.manufacturer) {
            manufacturers.push_back(model.manufacturer);
        }
    }
    return manufacturers;
}

static const HamlibModelEntry* findHamlibModelById(int model_id) {
    for (const auto& model : enumerateHamlibModels()) {
        if (model.model == model_id) {
            return &model;
        }
    }
    return nullptr;
}

static std::string normalizeRadioBackend(const std::string& backend) {
    const std::string upper = upperCopy(trimCopy(backend));
    if (upper == "HAMLIB") {
        return "hamlib";
    }
    if (upper == "KENWOOD_TCP" || upper == "KENWOOD TCP") {
        return "kenwood_tcp";
    }
    return "none";
}

// Skywave bandwidth families (WAVEFORM.md).
static std::string normalizeModemFamily(const std::string& family) {
    const std::string upper = upperCopy(trimCopy(family));
    if (upper == "MID" || upper == "NARROW" || upper == "ULTRA") {
        return upper;
    }
    return "WIDE";
}

static const char* liveVoiceCodecDisplayLabel(const std::string& codec) {
    // Codec2 rates offered by Skywave (VOICE.md).
    if (codec == "700C") return "700C";
    if (codec == "1300") return "1300";
    if (codec == "2400") return "2400";
    if (codec == "3200") return "3200";
    return codec.c_str();
}

static std::string liveVoiceHotkeyDisplayLabel(const std::string& hotkey) {
    constexpr char kScancodePrefix[] = "SCANCODE:";
    if (hotkey.empty()) {
        return "NONE";
    }
    if (hotkey.rfind(kScancodePrefix, 0) == 0) {
        const char* value = hotkey.c_str() + (sizeof(kScancodePrefix) - 1);
        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        if (end != value && *end == '\0' &&
            parsed > 0 && parsed < SDL_NUM_SCANCODES) {
            const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(parsed));
            if (name && *name) {
                return name;
            }
        }
    }
    return hotkey;
}

namespace skylink {

SettingsWindow::SettingsWindow(SharedAppState& state)
    : WindowBase(state) {}

void SettingsWindow::render() {
    auto& config_ = *state_.config;
    auto* modem_ = state_.modem;
    auto* callsign_input_ = state_.callsign_input;
    auto* modem_host_input_ = state_.modem_host_input;
    auto& modem_port_input_ = *state_.modem_port_input;
    auto& gps_port_ = *state_.gps_port;
    auto& gps_baud_ = *state_.gps_baud;
    auto& gps_fix_valid_ = *state_.gps_fix_valid;
    auto& email_gateway_active_ = *state_.email_gateway_active;

    if (ImGui::BeginTabBar("SettingsTabs")) {
        if (ImGui::BeginTabItem("Station")) {
            ImGui::InputText("Callsign", callsign_input_, state_.callsign_input_size);

            static char grid[16] = "";
            std::strncpy(grid, config_.station.grid_square.c_str(), sizeof(grid) - 1);
            if (ImGui::InputText("Grid Square", grid, sizeof(grid))) {
                config_.station.grid_square = grid;
            }

            static char name[64] = "";
            std::strncpy(name, config_.station.name.c_str(), sizeof(name) - 1);
            if (ImGui::InputText("Operator Name", name, sizeof(name))) {
                config_.station.name = name;
            }

            if (ImGui::Button("Apply")) {
                config_.station.callsign = callsign_input_;
                if (modem_) {
                    modem_->setCallsign(config_.station.callsign);
                }
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Modem")) {
            struct ModemFamilyChoice {
                const char* value;
                const char* display;
            };
            // ULTRA additionally requires modem.ultra_enable in the modem's
            // own INI; the modem rejects it otherwise.
            static const ModemFamilyChoice kFamilyChoices[] = {
                {"WIDE", "WIDE (2.4 kHz)"},
                {"MID", "MID (1 kHz)"},
                {"NARROW", "NARROW (500 Hz)"},
                {"ULTRA", "ULTRA (2.7 kHz)"},
            };
            constexpr int kFamilyChoiceCount = 4;
            auto current_choice_index = [](const auto& values, int count, const std::string& current) {
                for (int i = 0; i < count; ++i) {
                    if (current == values[i].value) return i;
                }
                return 0;
            };

            ImGui::InputText("Host", modem_host_input_, state_.modem_host_input_size);
            ImGui::InputInt("Command Port", &modem_port_input_);

            static int data_port_input = kDefaultDataPort;
            data_port_input = config_.modem.data_port;
            if (ImGui::InputInt("Data Port", &data_port_input)) {
                config_.modem.data_port = static_cast<uint16_t>(data_port_input);
            }

            static int voice_port_input = kDefaultVoicePort;
            voice_port_input = config_.modem.voice_port;
            if (ImGui::InputInt("Voice Port", &voice_port_input)) {
                config_.modem.voice_port = static_cast<uint16_t>(voice_port_input);
            }

            config_.modem.family = normalizeModemFamily(config_.modem.family);
            int family_idx = current_choice_index(kFamilyChoices, kFamilyChoiceCount,
                                                  config_.modem.family);
            if (ImGui::BeginCombo("Bandwidth Family", kFamilyChoices[family_idx].display)) {
                for (int i = 0; i < kFamilyChoiceCount; ++i) {
                    const bool selected = (family_idx == i);
                    std::string item_label = std::string(kFamilyChoices[i].display) +
                                             "##modem_family_" + kFamilyChoices[i].value;
                    if (ImGui::Selectable(item_label.c_str(), selected)) {
                        config_.modem.family = kFamilyChoices[i].value;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("Family is fixed per session; the modem accepts "
                                "a change only while idle.");

            bool auto_connect = config_.modem.auto_connect;
            if (ImGui::Checkbox("Auto-connect on startup", &auto_connect)) {
                config_.modem.auto_connect = auto_connect;
            }

            ImGui::Separator();
            if (ImGui::Button("Connect")) {
                config_.modem.host = modem_host_input_;
                config_.modem.command_port = static_cast<uint16_t>(modem_port_input_);
                if (onConnectModem) onConnectModem();
            }
            ImGui::SameLine();
            if (ImGui::Button("Disconnect")) {
                if (onDisconnectModem) onDisconnectModem();
            }
            ImGui::SameLine();
            if (ImGui::Button("Apply Modem Settings")) {
                config_.modem.host = modem_host_input_;
                config_.modem.command_port = static_cast<uint16_t>(modem_port_input_);
                if (onApplyModemConfig) onApplyModemConfig();
            }

            // Show connection status
            if (modem_ && modem_->isConnected()) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Connected");
            } else {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.2f, 1.0f), "Disconnected");
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Radio")) {
            struct RadioBackendChoice {
                const char* value;
                const char* label;
            };
            static const RadioBackendChoice kBackendChoices[] = {
                {"none", "(none)"},
                {"hamlib", "Hamlib"},
                {"kenwood_tcp", "Kenwood TCP"},
            };
            static const int kBaudChoices[] = {
                1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200
            };
            static const char* kBaudLabels[] = {
                "1200", "2400", "4800", "9600", "19200", "38400", "57600", "115200"
            };

            std::string backend = normalizeRadioBackend(config_.radio.backend);
            config_.radio.backend = backend;
            config_.radio.enabled = (backend != "none");

            bool radio_auto_connect = config_.radio.auto_connect;
            if (ImGui::Checkbox("Auto-connect radio on startup", &radio_auto_connect)) {
                config_.radio.auto_connect = radio_auto_connect;
            }

            static std::vector<std::string> cached_radio_ports;
            static auto last_radio_scan = std::chrono::steady_clock::time_point{};
            const auto radio_now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(radio_now - last_radio_scan).count() >= 3) {
                cached_radio_ports = enumerateSerialPorts();
                last_radio_scan = radio_now;
            }

            const char* backend_preview = kBackendChoices[0].label;
            for (const auto& choice : kBackendChoices) {
                if (backend == choice.value) {
                    backend_preview = choice.label;
                    break;
                }
            }
            if (ImGui::BeginCombo("Radio Device", backend_preview)) {
                for (const auto& choice : kBackendChoices) {
                    const bool selected = (backend == choice.value);
                    if (ImGui::Selectable(choice.label, selected)) {
                        backend = choice.value;
                        config_.radio.backend = backend;
                        config_.radio.enabled = (backend != "none");
                    }
                }
                ImGui::EndCombo();
            }

            if (backend == "hamlib") {
                const auto manufacturers = enumerateHamlibManufacturers();
                if (config_.radio.hamlib_manufacturer.empty()) {
                    if (const HamlibModelEntry* selected = findHamlibModelById(config_.radio.hamlib_model)) {
                        config_.radio.hamlib_manufacturer = selected->manufacturer;
                    } else if (!manufacturers.empty()) {
                        config_.radio.hamlib_manufacturer = manufacturers.front();
                    }
                }

                if (!manufacturers.empty()) {
                    auto current_manufacturer = std::find(manufacturers.begin(),
                                                          manufacturers.end(),
                                                          config_.radio.hamlib_manufacturer);
                    if (current_manufacturer == manufacturers.end()) {
                        config_.radio.hamlib_manufacturer = manufacturers.front();
                    }
                    if (ImGui::BeginCombo("Hamlib Manufacturer",
                                          config_.radio.hamlib_manufacturer.c_str())) {
                        for (const auto& manufacturer : manufacturers) {
                            const bool selected =
                                (config_.radio.hamlib_manufacturer == manufacturer);
                            if (ImGui::Selectable(manufacturer.c_str(), selected)) {
                                config_.radio.hamlib_manufacturer = manufacturer;
                            }
                        }
                        ImGui::EndCombo();
                    }

                    std::vector<const HamlibModelEntry*> filtered_models;
                    for (const auto& model : enumerateHamlibModels()) {
                        if (model.manufacturer == config_.radio.hamlib_manufacturer) {
                            filtered_models.push_back(&model);
                        }
                    }
                    if (!filtered_models.empty()) {
                        const HamlibModelEntry* selected_model =
                            findHamlibModelById(config_.radio.hamlib_model);
                        if (!selected_model ||
                            selected_model->manufacturer != config_.radio.hamlib_manufacturer) {
                            config_.radio.hamlib_model = filtered_models.front()->model;
                            selected_model = filtered_models.front();
                        }
                        const std::string preview =
                            selected_model ? selected_model->name : std::string("(none)");
                        if (ImGui::BeginCombo("Hamlib Model", preview.c_str())) {
                            for (const HamlibModelEntry* model : filtered_models) {
                                const bool selected =
                                    (config_.radio.hamlib_model == model->model);
                                const std::string label =
                                    model->name + "##hamlib_model_" +
                                    std::to_string(model->model);
                                if (ImGui::Selectable(label.c_str(), selected)) {
                                    config_.radio.hamlib_model = model->model;
                                }
                            }
                            ImGui::EndCombo();
                        }
                    }
                }

                const std::string device_preview =
                    config_.radio.device.empty() ? std::string("(none)") : config_.radio.device;
                if (!cached_radio_ports.empty() &&
                    ImGui::BeginCombo("Serial Device", device_preview.c_str())) {
                    if (ImGui::Selectable("(none)", config_.radio.device.empty())) {
                        config_.radio.device.clear();
                    }
                    for (const std::string& port : cached_radio_ports) {
                        if (ImGui::Selectable(port.c_str(), config_.radio.device == port)) {
                            config_.radio.device = port;
                        }
                    }
                    ImGui::EndCombo();
                }

                const bool show_manual_device =
                    cached_radio_ports.empty() ||
                    (!config_.radio.device.empty() &&
                     std::find(cached_radio_ports.begin(),
                               cached_radio_ports.end(),
                               config_.radio.device) == cached_radio_ports.end());
                if (show_manual_device) {
                    static char radio_device_buf[256] = "";
                    static std::string last_radio_device;
                    if (last_radio_device != config_.radio.device) {
                        std::strncpy(radio_device_buf,
                                     config_.radio.device.c_str(),
                                     sizeof(radio_device_buf) - 1);
                        radio_device_buf[sizeof(radio_device_buf) - 1] = '\0';
                        last_radio_device = config_.radio.device;
                    }
                    if (ImGui::InputText("Manual Device Path",
                                         radio_device_buf,
                                         sizeof(radio_device_buf))) {
                        config_.radio.device = radio_device_buf;
                        last_radio_device = config_.radio.device;
                    }
                }

                int baud_idx = 0;
                for (int i = 0; i < IM_ARRAYSIZE(kBaudChoices); ++i) {
                    if (config_.radio.baud == kBaudChoices[i]) {
                        baud_idx = i;
                        break;
                    }
                }
                if (ImGui::BeginCombo("Baud", kBaudLabels[baud_idx])) {
                    for (int i = 0; i < IM_ARRAYSIZE(kBaudChoices); ++i) {
                        const bool selected = (config_.radio.baud == kBaudChoices[i]);
                        if (ImGui::Selectable(kBaudLabels[i], selected)) {
                            config_.radio.baud = kBaudChoices[i];
                        }
                    }
                    ImGui::EndCombo();
                }
            } else if (backend == "kenwood_tcp") {
                static char kenwood_host_buf[128] = "";
                static std::string last_kenwood_host;
                if (last_kenwood_host != config_.radio.kenwood_host) {
                    std::strncpy(kenwood_host_buf,
                                 config_.radio.kenwood_host.c_str(),
                                 sizeof(kenwood_host_buf) - 1);
                    kenwood_host_buf[sizeof(kenwood_host_buf) - 1] = '\0';
                    last_kenwood_host = config_.radio.kenwood_host;
                }
                if (ImGui::InputText("IP Address",
                                     kenwood_host_buf,
                                     sizeof(kenwood_host_buf))) {
                    config_.radio.kenwood_host = kenwood_host_buf;
                    last_kenwood_host = config_.radio.kenwood_host;
                }

                int kenwood_port = config_.radio.kenwood_port;
                if (ImGui::InputInt("Port", &kenwood_port)) {
                    config_.radio.kenwood_port =
                        static_cast<uint16_t>(std::clamp(kenwood_port, 1, 65535));
                }

                ImGui::TextDisabled(
                    "Kenwood TCP uses the companion app radio backend and forwards PTT through the local TCP bridge.");
            }

            bool ptt_server_enabled = config_.radio.ptt_server_enabled;
            if (ImGui::Checkbox("Enable Local TCP PTT Server", &ptt_server_enabled)) {
                config_.radio.ptt_server_enabled = ptt_server_enabled;
            }

            static char ptt_bind_buf[128] = "";
            static std::string last_ptt_bind;
            if (last_ptt_bind != config_.radio.ptt_server_bind) {
                std::strncpy(ptt_bind_buf, config_.radio.ptt_server_bind.c_str(), sizeof(ptt_bind_buf) - 1);
                ptt_bind_buf[sizeof(ptt_bind_buf) - 1] = '\0';
                last_ptt_bind = config_.radio.ptt_server_bind;
            }
            if (ImGui::InputText("PTT Bind", ptt_bind_buf, sizeof(ptt_bind_buf))) {
                config_.radio.ptt_server_bind = ptt_bind_buf;
                last_ptt_bind = config_.radio.ptt_server_bind;
            }

            int ptt_port = config_.radio.ptt_server_port;
            if (ImGui::InputInt("PTT Port", &ptt_port)) {
                config_.radio.ptt_server_port = static_cast<uint16_t>(std::clamp(ptt_port, 1, 65535));
            }

            static char default_band[32] = "";
            static std::string last_default_band;
            if (last_default_band != config_.radio.default_band) {
                std::strncpy(default_band, config_.radio.default_band.c_str(), sizeof(default_band) - 1);
                default_band[sizeof(default_band) - 1] = '\0';
                last_default_band = config_.radio.default_band;
            }
            if (ImGui::InputText("Default Band", default_band, sizeof(default_band))) {
                config_.radio.default_band = default_band;
                last_default_band = config_.radio.default_band;
            }

            bool auto_qsy = config_.radio.auto_qsy;
            if (ImGui::Checkbox("Auto QSY", &auto_qsy)) {
                config_.radio.auto_qsy = auto_qsy;
            }

            int qsy_ack_timeout_ms = config_.radio.qsy_ack_timeout_ms;
            if (ImGui::InputInt("QSY ACK Timeout (ms)", &qsy_ack_timeout_ms)) {
                config_.radio.qsy_ack_timeout_ms = std::clamp(qsy_ack_timeout_ms, 2000, 60000);
            }

            int qsy_reconnect_delay_ms = config_.radio.qsy_reconnect_delay_ms;
            if (ImGui::InputInt("QSY Reconnect Delay (ms)", &qsy_reconnect_delay_ms)) {
                config_.radio.qsy_reconnect_delay_ms = std::clamp(qsy_reconnect_delay_ms, 250, 30000);
            }

            int qsy_reconnect_window_ms = config_.radio.qsy_reconnect_window_ms;
            if (ImGui::InputInt("QSY Reconnect Window (ms)", &qsy_reconnect_window_ms)) {
                config_.radio.qsy_reconnect_window_ms = std::clamp(qsy_reconnect_window_ms, 1000, 120000);
            }

            int qsy_connect_timeout_ms = config_.radio.qsy_connect_timeout_ms;
            if (ImGui::InputInt("QSY Connect Timeout (ms)", &qsy_connect_timeout_ms)) {
                config_.radio.qsy_connect_timeout_ms = std::clamp(qsy_connect_timeout_ms, 2000, 60000);
            }

            int qsy_max_reconnect_attempts = config_.radio.qsy_max_reconnect_attempts;
            if (ImGui::InputInt("QSY Max Reconnect Attempts", &qsy_max_reconnect_attempts)) {
                config_.radio.qsy_max_reconnect_attempts = std::clamp(qsy_max_reconnect_attempts, 1, 10);
            }

            if (state_.radio) {
                const RadioController::Status radio_status = state_.radio->getStatus();
                ImGui::Separator();
                ImGui::Text("Status: %s%s",
                            radio_status.connected ? "Connected" : "Disconnected",
                            radio_status.dry_run ? " (dry-run)" : "");
                ImGui::Text("PTT Server: %s", radio_status.ptt_server_running ? "Running" : "Stopped");
                if (!radio_status.last_error.empty()) {
                    ImGui::TextColored(ImVec4(0.85f, 0.45f, 0.35f, 1.0f),
                                       "Last Error: %s",
                                       radio_status.last_error.c_str());
                }
            }

            if (ImGui::Button("Apply Radio Config")) {
                if (onApplyRadioConfig) onApplyRadioConfig();
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Voice")) {
            const VoiceStatus voice_status =
                modem_ ? modem_->getVoiceStatus() : VoiceStatus{};
            config_.voice.codec = "AUTO";

            static const char* kVoiceModes[] = {
                "OFF", "CONNECTED", "BROADCAST"
            };
            auto current_index = [](const char* const* values, int count, const std::string& current) {
                for (int i = 0; i < count; ++i) {
                    if (current == values[i]) return i;
                }
                return 0;
            };
            int mode_idx = current_index(kVoiceModes, 3, config_.voice.mode);
            if (ImGui::BeginCombo("Usage", kVoiceModes[mode_idx])) {
                for (int i = 0; i < 3; ++i) {
                    const bool selected = (mode_idx == i);
                    if (ImGui::Selectable(kVoiceModes[i], selected)) {
                        config_.voice.mode = kVoiceModes[i];
                    }
                }
                ImGui::EndCombo();
            }

            // Skywave picks the Codec2 rate from the live ladder mode
            // (VOICE.md AUTO table) and steps it down when AMC demotes, so
            // the app has no separate voice-bandwidth selector.
            ImGui::TextDisabled("Codec rate is chosen automatically from the "
                                "link mode.");

            bool auto_start = config_.voice.auto_start;
            if (ImGui::Checkbox("Auto-start Voice", &auto_start)) {
                config_.voice.auto_start = auto_start;
            }

            const bool hotkey_capture_active =
                isVoiceHotkeyCaptureActive ? isVoiceHotkeyCaptureActive() : false;
            const std::string hotkey_label =
                liveVoiceHotkeyDisplayLabel(config_.voice.ptt_hotkey);
            ImGui::Text("PTT Hotkey: %s", hotkey_label.c_str());
            if (hotkey_capture_active) {
                ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.28f, 1.0f),
                                   "Press the key you want to use. Esc cancels. Backspace clears.");
                if (ImGui::Button("Cancel Hotkey Capture")) {
                    if (onCancelVoiceHotkeyCapture) onCancelVoiceHotkeyCapture();
                }
            } else {
                if (ImGui::Button("Capture PTT Hotkey")) {
                    if (onBeginVoiceHotkeyCapture) onBeginVoiceHotkeyCapture();
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear PTT Hotkey")) {
                    if (onClearVoiceHotkey) onClearVoiceHotkey();
                }
                ImGui::TextDisabled("Hotkey works whenever the Skywave Link window has focus.");
            }

            const std::vector<std::string> capture_devices = enumerateAudioDevices(true);
            const std::vector<std::string> playback_devices = enumerateAudioDevices(false);
            auto render_audio_combo =
                [](const char* label,
                   const std::vector<std::string>& devices,
                   std::string* selected_value) {
                    const std::string preview =
                        selected_value->empty() ? std::string("System Default")
                                                : *selected_value;
                    if (ImGui::BeginCombo(label, preview.c_str())) {
                        if (ImGui::Selectable("System Default", selected_value->empty())) {
                            selected_value->clear();
                        }
                        for (const std::string& device : devices) {
                            if (ImGui::Selectable(device.c_str(), *selected_value == device)) {
                                *selected_value = device;
                            }
                        }
                        ImGui::EndCombo();
                    }
                };

            if (!capture_devices.empty()) {
                render_audio_combo("Capture Device", capture_devices, &config_.voice.capture_device);
            } else {
                static char capture_device[256] = "";
                static std::string last_capture_device;
                if (last_capture_device != config_.voice.capture_device) {
                    std::strncpy(capture_device,
                                 config_.voice.capture_device.c_str(),
                                 sizeof(capture_device) - 1);
                    capture_device[sizeof(capture_device) - 1] = '\0';
                    last_capture_device = config_.voice.capture_device;
                }
                if (ImGui::InputText("Capture Device", capture_device, sizeof(capture_device))) {
                    config_.voice.capture_device = capture_device;
                    last_capture_device = config_.voice.capture_device;
                }
            }

            if (!playback_devices.empty()) {
                render_audio_combo("Playback Device", playback_devices, &config_.voice.playback_device);
            } else {
                static char playback_device[256] = "";
                static std::string last_playback_device;
                if (last_playback_device != config_.voice.playback_device) {
                    std::strncpy(playback_device,
                                 config_.voice.playback_device.c_str(),
                                 sizeof(playback_device) - 1);
                    playback_device[sizeof(playback_device) - 1] = '\0';
                    last_playback_device = config_.voice.playback_device;
                }
                if (ImGui::InputText("Playback Device", playback_device, sizeof(playback_device))) {
                    config_.voice.playback_device = playback_device;
                    last_playback_device = config_.voice.playback_device;
                }
            }
            ImGui::TextDisabled("Leave device blank to use the system default input/output.");

            int capture_gain = config_.voice.capture_gain_percent;
            if (ImGui::SliderInt("Capture Gain %", &capture_gain, 0, 400)) {
                config_.voice.capture_gain_percent = capture_gain;
            }

            int playback_gain = config_.voice.playback_gain_percent;
            if (ImGui::SliderInt("Playback Gain %", &playback_gain, 0, 400)) {
                config_.voice.playback_gain_percent = playback_gain;
            }

            ImGui::Separator();
            if (ImGui::Button("Apply Voice Config")) {
                if (onApplyVoiceConfig) onApplyVoiceConfig();
            }
            ImGui::SameLine();
            if (ImGui::Button("Refresh Audio")) {
                if (onRefreshVoiceAudio) onRefreshVoiceAudio();
            }

            if (modem_ && modem_->isConnected()) {
                if (ImGui::Button("Start Voice")) {
                    if (onVoiceStart) onVoiceStart();
                }
                ImGui::SameLine();
                if (ImGui::Button("Stop Voice")) {
                    if (onVoiceStop) onVoiceStop();
                }
                ImGui::SameLine();
                static bool ptt_button_held = false;
                if (voice_status.ptt || ptt_button_held) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.84f, 0.26f, 0.24f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.89f, 0.32f, 0.30f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.76f, 0.20f, 0.18f, 1.0f));
                }
                ImGui::Button("PTT");
                const bool button_held_now = ImGui::IsItemActive();
                if (button_held_now != ptt_button_held) {
                    ptt_button_held = button_held_now;
                    if (onVoicePtt) onVoicePtt(ptt_button_held);
                }
                if (voice_status.ptt || ptt_button_held) {
                    ImGui::PopStyleColor(3);
                }
            }

            ImGui::Separator();
            ImGui::Text("Status:");
            ImGui::Text("Codec: %s", liveVoiceCodecDisplayLabel(voice_status.codec));
            ImGui::Text("Voice: %s",
                        voice_status.mode == VoiceMode::Connected ? "CONNECTED" :
                        voice_status.mode == VoiceMode::Broadcast ? "BROADCAST" : "OFF");
            ImGui::Text("State: %s",
                        voice_status.state == VoiceState::Opening ? "OPENING" :
                        voice_status.state == VoiceState::Ready ? "READY" :
                        voice_status.state == VoiceState::TxRequested ? "TX_REQUESTED" :
                        voice_status.state == VoiceState::TxActive ? "TX_ACTIVE" :
                        voice_status.state == VoiceState::RxActive ? "RX_ACTIVE" :
                        voice_status.state == VoiceState::Closing ? "CLOSING" : "IDLE");
            ImGui::Text("Last Event: %s", voice_status.last_event.c_str());
            ImGui::Text("PTT: %s  Active: %s  Local: %s",
                        voice_status.ptt ? "ON" : "OFF",
                        voice_status.active ? "YES" : "NO",
                        voice_status.local ? "YES" : "NO");
            if (voice_status.transport_mode >= 0) {
                ImGui::Text("Transport: M%d", voice_status.transport_mode);
            } else {
                ImGui::Text("Transport: OFF");
            }
            ImGui::Text("Queued TX: %d  Queued RX: %d samples",
                        voice_status.tx_queued,
                        voice_status.rx_queued);

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Files")) {
            ImGui::Text("Received File Storage");
            ImGui::Separator();

            static char download_path[512] = "";
            static std::string last_loaded_download_path;
            if (last_loaded_download_path != config_.files.download_path) {
                std::strncpy(download_path, config_.files.download_path.c_str(), sizeof(download_path) - 1);
                download_path[sizeof(download_path) - 1] = '\0';
                last_loaded_download_path = config_.files.download_path;
            }
            if (ImGui::InputText("Download Path", download_path, sizeof(download_path))) {
                config_.files.download_path = download_path;
                last_loaded_download_path = config_.files.download_path;
            }
            ImGui::SameLine();
            if (!onBrowseDownloadPath) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Browse...")) {
                if (onBrowseDownloadPath) {
                    const std::string selected = onBrowseDownloadPath();
                    if (!selected.empty()) {
                        std::strncpy(download_path, selected.c_str(), sizeof(download_path) - 1);
                        download_path[sizeof(download_path) - 1] = '\0';
                        config_.files.download_path = download_path;
                        last_loaded_download_path = config_.files.download_path;
                    }
                }
            }
            if (!onBrowseDownloadPath) {
                ImGui::EndDisabled();
            }

            std::vector<std::string> quick_dirs;
            quick_dirs.emplace_back("downloads");
            if (const char* home = std::getenv("HOME")) {
                const std::string base(home);
                quick_dirs.push_back(base + "/Downloads");
                quick_dirs.push_back(base + "/Desktop");
                quick_dirs.push_back(base + "/Documents");
            }
            quick_dirs.push_back(std::filesystem::current_path().string());
            std::sort(quick_dirs.begin(), quick_dirs.end());
            quick_dirs.erase(std::unique(quick_dirs.begin(), quick_dirs.end()), quick_dirs.end());

            if (ImGui::BeginCombo("Quick Location", "Select preset path")) {
                for (const auto& dir : quick_dirs) {
                    const bool selected = (std::string(download_path) == dir);
                    if (ImGui::Selectable(dir.c_str(), selected)) {
                        std::strncpy(download_path, dir.c_str(), sizeof(download_path) - 1);
                        download_path[sizeof(download_path) - 1] = '\0';
                        config_.files.download_path = download_path;
                        last_loaded_download_path = config_.files.download_path;
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::TextDisabled("Files received over HF are saved in this directory.");

            int max_file_kb = static_cast<int>(config_.files.max_file_size / 1024);
            if (ImGui::SliderInt("Max File Size (KB)", &max_file_kb, 1, 1024)) {
                config_.files.max_file_size =
                    static_cast<size_t>(std::clamp(max_file_kb, 1, 1024)) * 1024;
            }

            bool auto_accept = config_.files.auto_accept;
            if (ImGui::Checkbox("Auto-accept incoming files", &auto_accept)) {
                config_.files.auto_accept = auto_accept;
            }

            if (ImGui::Button("Apply File Settings")) {
                config_.files.download_path = download_path;
                if (config_.files.download_path.empty()) {
                    config_.files.download_path = "downloads";
                }
                std::error_code ec;
                std::filesystem::create_directories(config_.files.download_path, ec);
                if (!ec && state_.file_transfer_mgr) {
                    state_.file_transfer_mgr->setDownloadPath(config_.files.download_path);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset Path")) {
                config_.files.download_path = "downloads";
                std::strncpy(download_path, config_.files.download_path.c_str(), sizeof(download_path) - 1);
                download_path[sizeof(download_path) - 1] = '\0';
            }

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("APRS")) {
            bool aprs_enabled = config_.aprs.enabled;
            if (ImGui::Checkbox("Enable APRS", &aprs_enabled)) {
                config_.aprs.enabled = aprs_enabled;
            }

            // Apply-gated fields keep their edits across frames; they are
            // reloaded from config only when config itself changes (load,
            // Apply), the same pattern as the Files tab's download path.
            static char server[256] = "";
            static int beacon_interval = 0;
            static std::string aprs_loaded_key;
            {
                const std::string key = config_.aprs.server + '\x1f' +
                                        std::to_string(config_.aprs.beacon_interval_sec);
                if (key != aprs_loaded_key) {
                    std::strncpy(server, config_.aprs.server.c_str(), sizeof(server) - 1);
                    server[sizeof(server) - 1] = '\0';
                    beacon_interval = config_.aprs.beacon_interval_sec;
                    aprs_loaded_key = key;
                }
            }
            ImGui::InputText("Server", server, sizeof(server));

            // Auto-generated passcode from callsign (APRS-IS standard hash)
            if (!config_.station.callsign.empty()) {
                int32_t code = APRSClient::calculatePasscode(config_.station.callsign);
                ImGui::Text("Passcode: %d (auto-generated from %s)",
                            code, config_.station.callsign.c_str());
            } else {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f),
                                   "Passcode: set callsign first");
            }

            ImGui::InputInt("Beacon Interval (sec)", &beacon_interval);

            ImGui::Separator();
            ImGui::Text("GPS Position Source");

            bool gps_enabled = config_.aprs.gps_enabled;
            if (ImGui::Checkbox("Enable GPS serial input", &gps_enabled)) {
                config_.aprs.gps_enabled = gps_enabled;
                if (!config_.aprs.gps_enabled) {
                    if (onCloseGPSPort) onCloseGPSPort();
                }
            }

            // Serial port dropdown
            static std::vector<std::string> cached_ports;
            static auto last_scan = std::chrono::steady_clock::time_point{};
            auto now_scan = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now_scan - last_scan).count() >= 3) {
                cached_ports = enumerateSerialPorts();
                last_scan = now_scan;
            }

            // Find current selection index
            int current_idx = 0;  // 0 = "(none)"
            for (size_t i = 0; i < cached_ports.size(); ++i) {
                if (cached_ports[i] == config_.aprs.gps_port) {
                    current_idx = static_cast<int>(i + 1);
                    break;
                }
            }

            const std::string preview = current_idx > 0
                ? cached_ports[current_idx - 1]
                : (config_.aprs.gps_port.empty() ? "(none)" : config_.aprs.gps_port);
            if (ImGui::BeginCombo("GPS Serial Port", preview.c_str())) {
                if (ImGui::Selectable("(none)", current_idx == 0)) {
                    config_.aprs.gps_port.clear();
                }
                for (size_t i = 0; i < cached_ports.size(); ++i) {
                    bool selected = (current_idx == static_cast<int>(i + 1));
                    if (ImGui::Selectable(cached_ports[i].c_str(), selected)) {
                        config_.aprs.gps_port = cached_ports[i];
                    }
                }
                ImGui::EndCombo();
            }

            int gps_baud_val = config_.aprs.gps_baud;
            if (ImGui::InputInt("GPS Baud", &gps_baud_val)) {
                if (gps_baud_val < 1200) gps_baud_val = 1200;
                if (gps_baud_val > 115200) gps_baud_val = 115200;
                config_.aprs.gps_baud = gps_baud_val;
            }

            if (config_.aprs.gps_enabled) {
                if (!gps_port_.empty()) {
                    ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f),
                                       "GPS connected: %s @ %d", gps_port_.c_str(), gps_baud_);
                    if (gps_fix_valid_) {
                        ImGui::Text("GPS fix: %.6f, %.6f",
                                    state_.my_lat ? *state_.my_lat : 0.0,
                                    state_.my_lon ? *state_.my_lon : 0.0);
                    } else {
                        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "GPS fix: waiting...");
                    }
                } else {
                    ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.2f, 1.0f), "GPS disconnected");
                }

                if (ImGui::Button("Open GPS Port")) {
                    if (!config_.aprs.gps_port.empty()) {
                        if (onCloseGPSPort) onCloseGPSPort();
                        if (onOpenGPSPort) onOpenGPSPort(config_.aprs.gps_port, config_.aprs.gps_baud);
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Close GPS Port")) {
                    if (onCloseGPSPort) onCloseGPSPort();
                }
            }

            if (ImGui::Button("Apply APRS Settings")) {
                config_.aprs.server = server;
                config_.aprs.passcode.clear();  // Always use auto-generated passcode
                config_.aprs.beacon_interval_sec = beacon_interval;

                if (!config_.aprs.gps_enabled || config_.aprs.gps_port.empty()) {
                    if (onCloseGPSPort) onCloseGPSPort();
                } else if (gps_port_ != config_.aprs.gps_port || gps_baud_ != config_.aprs.gps_baud) {
                    if (onCloseGPSPort) onCloseGPSPort();
                    if (onOpenGPSPort) onOpenGPSPort(config_.aprs.gps_port, config_.aprs.gps_baud);
                } else if (gps_port_.empty()) {
                    if (onOpenGPSPort) onOpenGPSPort(config_.aprs.gps_port, config_.aprs.gps_baud);
                }
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Email")) {
            // Working copies of the Apply-gated fields. They persist across
            // frames and are reloaded from config only when config changes
            // (load or Apply), so edits are not reverted every frame.
            struct EmailEdit {
                char smtp_host[256] = "";
                int smtp_port = 0;
                char smtp_user[128] = "";
                char winlink_host[256] = "";
                int winlink_port = 0;
                char winlink_call[32] = "";
                int sf_ttl = 0;
                int sf_hop_limit = 0;
                int sf_dedup_window = 0;
                int pop3_poll_interval_sec = 0;
                int pop3_max_messages = 0;
                int winlink_fetch_interval_sec = 0;
                int winlink_fetch_max_messages = 0;
            };
            static EmailEdit email_edit;
            static std::string email_loaded_key;
            {
                const auto& e = config_.email;
                const char sep = '\x1f';
                const std::string key =
                    e.smtp_host + sep + std::to_string(e.smtp_port) + sep + e.smtp_username + sep +
                    e.winlink_telnet_host + sep + std::to_string(e.winlink_telnet_port) + sep +
                    e.winlink_telnet_callsign + sep + std::to_string(e.hf_store_forward_ttl) + sep +
                    std::to_string(e.hf_store_forward_hop_limit) + sep +
                    std::to_string(e.hf_store_forward_dedup_window_sec) + sep +
                    std::to_string(e.pop3_poll_interval_sec) + sep +
                    std::to_string(e.pop3_max_messages) + sep +
                    std::to_string(e.winlink_fetch_interval_sec) + sep +
                    std::to_string(e.winlink_fetch_max_messages);
                if (key != email_loaded_key) {
                    auto copyField = [](char* dst, size_t size, const std::string& src) {
                        std::strncpy(dst, src.c_str(), size - 1);
                        dst[size - 1] = '\0';
                    };
                    copyField(email_edit.smtp_host, sizeof(email_edit.smtp_host), e.smtp_host);
                    email_edit.smtp_port = e.smtp_port;
                    copyField(email_edit.smtp_user, sizeof(email_edit.smtp_user), e.smtp_username);
                    copyField(email_edit.winlink_host, sizeof(email_edit.winlink_host), e.winlink_telnet_host);
                    email_edit.winlink_port = e.winlink_telnet_port;
                    copyField(email_edit.winlink_call, sizeof(email_edit.winlink_call), e.winlink_telnet_callsign);
                    email_edit.sf_ttl = e.hf_store_forward_ttl;
                    email_edit.sf_hop_limit = e.hf_store_forward_hop_limit;
                    email_edit.sf_dedup_window = e.hf_store_forward_dedup_window_sec;
                    email_edit.pop3_poll_interval_sec = e.pop3_poll_interval_sec;
                    email_edit.pop3_max_messages = e.pop3_max_messages;
                    email_edit.winlink_fetch_interval_sec = e.winlink_fetch_interval_sec;
                    email_edit.winlink_fetch_max_messages = e.winlink_fetch_max_messages;
                    email_loaded_key = key;
                }
            }
            auto& smtp_host = email_edit.smtp_host;
            int& smtp_port = email_edit.smtp_port;
            auto& smtp_user = email_edit.smtp_user;
            auto& winlink_host = email_edit.winlink_host;
            int& winlink_port = email_edit.winlink_port;
            auto& winlink_call = email_edit.winlink_call;
            int& sf_ttl = email_edit.sf_ttl;
            int& sf_hop_limit = email_edit.sf_hop_limit;
            int& sf_dedup_window = email_edit.sf_dedup_window;
            int& pop3_poll_interval_sec = email_edit.pop3_poll_interval_sec;
            int& pop3_max_messages = email_edit.pop3_max_messages;
            int& winlink_fetch_interval_sec = email_edit.winlink_fetch_interval_sec;
            int& winlink_fetch_max_messages = email_edit.winlink_fetch_max_messages;

            ImGui::InputText("SMTP Host", smtp_host, sizeof(smtp_host));
            ImGui::InputInt("SMTP Port", &smtp_port);
            ImGui::InputText("SMTP Username", smtp_user, sizeof(smtp_user));

            static char smtp_pass[128] = "";
            ImGui::InputText("SMTP Password", smtp_pass, sizeof(smtp_pass), ImGuiInputTextFlags_Password);

            bool gateway_enabled = config_.email.gateway_enabled;
            if (ImGui::Checkbox("Enable Email Gateway Relay", &gateway_enabled)) {
                config_.email.gateway_enabled = gateway_enabled;
            }

            bool winlink_telnet_enabled = config_.email.winlink_telnet_enabled;
            if (ImGui::Checkbox("Use Winlink Telnet (internet)", &winlink_telnet_enabled)) {
                config_.email.winlink_telnet_enabled = winlink_telnet_enabled;
            }

            if (!winlink_telnet_enabled) ImGui::BeginDisabled();
            ImGui::InputText("Winlink Host", winlink_host, sizeof(winlink_host));
            ImGui::InputInt("Winlink Port", &winlink_port);
            ImGui::InputText("Winlink Callsign", winlink_call, sizeof(winlink_call));

            static char winlink_pass[128] = "";
            ImGui::InputText("Winlink Password", winlink_pass, sizeof(winlink_pass), ImGuiInputTextFlags_Password);

            bool winlink_tls = config_.email.winlink_telnet_use_tls;
            if (ImGui::Checkbox("Winlink Telnet TLS", &winlink_tls)) {
                config_.email.winlink_telnet_use_tls = winlink_tls;
            }
            ImGui::TextDisabled("Default CMS telnet server: server.winlink.org:8772");
            if (!winlink_telnet_enabled) ImGui::EndDisabled();

            bool winlink_compat = config_.email.winlink_compat_mode;
            if (ImGui::Checkbox("Winlink-compatible B2F envelope", &winlink_compat)) {
                config_.email.winlink_compat_mode = winlink_compat;
            }

            ImGui::Separator();
            ImGui::Text("Weak-Signal Profile");
            bool weak_profile = config_.email.weak_signal_profile_enabled;
            if (ImGui::Checkbox("Enable weak-signal profile", &weak_profile)) {
                config_.email.weak_signal_profile_enabled = weak_profile;
            }

            int weak_snr_threshold = config_.email.weak_signal_snr_threshold_db;
            if (ImGui::SliderInt("Weak SNR Threshold (dB)", &weak_snr_threshold, -25, 2)) {
                config_.email.weak_signal_snr_threshold_db = weak_snr_threshold;
            }

            int weak_ack_timeout_ms = config_.email.weak_signal_ack_timeout_ms;
            if (ImGui::SliderInt("Weak ACK Timeout (ms)", &weak_ack_timeout_ms, 15000, 120000)) {
                config_.email.weak_signal_ack_timeout_ms = weak_ack_timeout_ms;
            }

            int weak_max_retries = config_.email.weak_signal_max_retries;
            if (ImGui::SliderInt("Weak Max Retries", &weak_max_retries, 3, 10)) {
                config_.email.weak_signal_max_retries = weak_max_retries;
            }

            bool weak_compress = config_.email.weak_signal_enable_compression;
            if (ImGui::Checkbox("Compress payload on weak links", &weak_compress)) {
                config_.email.weak_signal_enable_compression = weak_compress;
            }

            ImGui::Separator();
            ImGui::Text("HF Store-and-Forward");
            bool sf_enabled = config_.email.hf_store_forward_enabled;
            if (ImGui::Checkbox("Enable HF mesh forwarding", &sf_enabled)) {
                config_.email.hf_store_forward_enabled = sf_enabled;
            }
            ImGui::InputInt("Relay TTL", &sf_ttl);
            ImGui::InputInt("Relay Hop Limit", &sf_hop_limit);
            ImGui::InputInt("Relay Dedup Window (sec)", &sf_dedup_window);

            ImGui::Separator();
            ImGui::Text("Inbound Polling");
            ImGui::InputInt("POP3 Poll Interval (sec)", &pop3_poll_interval_sec);
            ImGui::InputInt("POP3 Max Messages", &pop3_max_messages);
            ImGui::InputInt("Winlink Fetch Interval (sec)", &winlink_fetch_interval_sec);
            ImGui::InputInt("Winlink Max Messages", &winlink_fetch_max_messages);

            ImGui::Separator();
            ImGui::Text("Relay Diagnostics");
            auto format_unix = [](int64_t ts) -> std::string {
                if (ts <= 0) {
                    return "never";
                }
                std::time_t t = static_cast<std::time_t>(ts);
                std::tm local_tm = *std::localtime(&t);
                char buf[32];
                std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local_tm);
                return std::string(buf);
            };

            if (state_.relay_diagnostics) {
                const RelayDiagnostics& diag = *state_.relay_diagnostics;
                const bool healthy = diag.gateway_health.find("fail") == std::string::npos &&
                                     diag.gateway_health.find("no-gateway") == std::string::npos &&
                                     diag.gateway_health.find("missing") == std::string::npos;
                ImGui::Text("Last Result: %s", diag.last_result.c_str());
                ImGui::Text("Last Mode: %s", diag.last_mode.c_str());
                ImGui::Text("Last Target: %s",
                            diag.last_target.empty() ? "(none)" : diag.last_target.c_str());
                ImGui::Text("Last Attempt: %s", format_unix(diag.last_attempt_unix).c_str());
                ImGui::Text("Last Success: %s", format_unix(diag.last_success_unix).c_str());
                ImGui::Text("Gateway Score: %.1f", diag.last_gateway_score);
                ImGui::Text("Queue Policy: %s", diag.queue_policy_state.c_str());
                ImGui::Text("Burst Sent: %d", diag.last_burst_sent);
                ImGui::Text("Held Low Priority: %d", diag.held_low_priority_count);
                if (!diag.last_hold_reason.empty()) {
                    ImGui::TextWrapped("Last Hold Reason: %s", diag.last_hold_reason.c_str());
                }
                if (!diag.last_failover_reason.empty()) {
                    ImGui::TextWrapped("Last Failover: %s", diag.last_failover_reason.c_str());
                }
                ImGui::Text("Winlink Fetch OK/Fail: %d / %d",
                            diag.winlink_fetch_success, diag.winlink_fetch_fail);
                ImGui::Text("Winlink Login Attempts: %d", diag.last_login_attempts);
                ImGui::Text("SMTP Fallback Count: %d", diag.smtp_fallback_count);
                ImGui::Text("Relay Success/Fail: %d / %d",
                            diag.total_success, diag.total_fail);
                ImGui::TextColored(healthy ? ImVec4(0.2f, 0.8f, 0.2f, 1.0f)
                                           : ImVec4(0.9f, 0.4f, 0.2f, 1.0f),
                                   "Gateway Health: %s",
                                   diag.gateway_health.c_str());
                if (!diag.last_error.empty()) {
                    ImGui::TextWrapped("Last Error: %s", diag.last_error.c_str());
                } else {
                    ImGui::TextDisabled("Last Error: none");
                }
            } else {
                ImGui::TextDisabled("Relay diagnostics unavailable");
            }

            if (ImGui::Button("Apply Email Settings")) {
                config_.email.smtp_host = smtp_host;
                config_.email.smtp_port = static_cast<uint16_t>(std::clamp(smtp_port, 1, 65535));
                config_.email.smtp_username = smtp_user;
                if (std::strlen(smtp_pass) > 0) {
                    config_.email.smtp_password = smtp_pass;
                }
                config_.email.winlink_telnet_host = winlink_host;
                config_.email.winlink_telnet_port = static_cast<uint16_t>(std::clamp(winlink_port, 1, 65535));
                config_.email.winlink_telnet_callsign = winlink_call;
                if (std::strlen(winlink_pass) > 0) {
                    config_.email.winlink_telnet_password = winlink_pass;
                }
                config_.email.pop3_poll_interval_sec = std::clamp(pop3_poll_interval_sec, 30, 3600);
                config_.email.pop3_max_messages = std::clamp(pop3_max_messages, 1, 500);
                config_.email.winlink_fetch_interval_sec = std::clamp(winlink_fetch_interval_sec, 30, 3600);
                config_.email.winlink_fetch_max_messages = std::clamp(winlink_fetch_max_messages, 1, 500);
                config_.email.hf_store_forward_ttl = std::clamp(sf_ttl, 1, 15);
                config_.email.hf_store_forward_hop_limit = std::clamp(sf_hop_limit, 1, 20);
                config_.email.hf_store_forward_dedup_window_sec = std::clamp(sf_dedup_window, 30, 3600);
                if (config_.email.gateway_enabled && !email_gateway_active_) {
                    if (onStartEmailGateway) onStartEmailGateway();
                } else if (!config_.email.gateway_enabled && email_gateway_active_) {
                    if (onStopEmailGateway) onStopEmailGateway();
                }
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Beacon")) {
            bool beacon_enabled = state_.beacon_enabled ? *state_.beacon_enabled : false;

            if (ImGui::Checkbox("Enable Beacon", &beacon_enabled)) {
                config_.beacon.enabled = beacon_enabled;
                if (state_.beacon_enabled) *state_.beacon_enabled = beacon_enabled;
            }

            ImGui::InputInt("Beacon Interval (sec)", &config_.beacon.interval_sec);
            if (config_.beacon.interval_sec < 60) config_.beacon.interval_sec = 60;
            if (config_.beacon.interval_sec > 3600) config_.beacon.interval_sec = 3600;

            static char beacon_text[256] = "";
            std::strncpy(beacon_text, config_.beacon.beacon_text.c_str(), sizeof(beacon_text) - 1);
            if (ImGui::InputText("Beacon Message", beacon_text, sizeof(beacon_text))) {
                config_.beacon.beacon_text = beacon_text;
            }

            bool notify_pending = config_.beacon.notify_pending;
            if (ImGui::Checkbox("Notify stations with pending messages", &notify_pending)) {
                config_.beacon.notify_pending = notify_pending;
            }

            ImGui::Separator();
            ImGui::Text("Beacon status:");
            if (beacon_enabled) {
                ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Active");
                if (last_beacon_sent) {
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - *last_beacon_sent);
                    int remaining = config_.beacon.interval_sec - static_cast<int>(elapsed.count());
                    if (remaining > 0) {
                        ImGui::SameLine();
                        ImGui::Text("(next in %d sec)", remaining);
                    }
                }
            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Disabled");
            }

            if (ImGui::Button("Send Beacon Now")) {
                if (onSendBeacon) onSendBeacon();
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Away Mode")) {
            bool away_enabled = config_.away.enabled;
            if (ImGui::Checkbox("Enable Away Mode Config", &away_enabled)) {
                config_.away.enabled = away_enabled;
            }

            static char away_msg[512] = "";
            std::strncpy(away_msg, config_.away.away_message.c_str(), sizeof(away_msg) - 1);
            if (ImGui::InputTextMultiline("Away Message", away_msg, sizeof(away_msg), ImVec2(-1, 60))) {
                config_.away.away_message = away_msg;
            }

            ImGui::Separator();
            ImGui::Text("Remote Station Permissions:");

            bool allow_query = config_.away.allow_message_query;
            if (ImGui::Checkbox("Allow message query", &allow_query)) {
                config_.away.allow_message_query = allow_query;
            }

            bool allow_email = config_.away.allow_email_gateway;
            if (ImGui::Checkbox("Allow email gateway access", &allow_email)) {
                config_.away.allow_email_gateway = allow_email;
            }

            bool allow_bbs = config_.away.allow_bbs_access;
            if (ImGui::Checkbox("Allow BBS file download", &allow_bbs)) {
                config_.away.allow_bbs_access = allow_bbs;
            }

            bool auto_reply = config_.away.auto_reply;
            if (ImGui::Checkbox("Auto-reply with away message", &auto_reply)) {
                config_.away.auto_reply = auto_reply;
            }

            ImGui::Separator();
            bool away_active = state_.away_mode_active ? *state_.away_mode_active : false;
            if (away_active) {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "Away Mode: ACTIVE");
                if (ImGui::Button("Disable Away Mode")) {
                    if (onDisableAwayMode) onDisableAwayMode();
                }
            } else {
                ImGui::Text("Away Mode: Inactive");
                if (ImGui::Button("Enable Away Mode")) {
                    if (onEnableAwayMode) onEnableAwayMode();
                }
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("PSK Reporter")) {
            bool psk_enabled = config_.psk_reporter.enabled;
            if (ImGui::Checkbox("Enable PSK Reporter", &psk_enabled)) {
                config_.psk_reporter.enabled = psk_enabled;
            }

            ImGui::TextWrapped("Automatically upload reception reports to pskreporter.info when "
                               "stations are heard via beacon, CQ, or connection.");

            ImGui::Separator();

            static char antenna[64] = "";
            std::strncpy(antenna, config_.psk_reporter.antenna.c_str(), sizeof(antenna) - 1);
            if (ImGui::InputText("Antenna", antenna, sizeof(antenna))) {
                config_.psk_reporter.antenna = antenna;
            }
            ImGui::SetItemTooltip("Description of your antenna (e.g., \"Dipole\", \"Vertical\", \"Beam\")");

            int min_snr = config_.psk_reporter.min_snr;
            if (ImGui::SliderInt("Minimum SNR (dB)", &min_snr, -30, 10)) {
                config_.psk_reporter.min_snr = min_snr;
            }
            ImGui::SetItemTooltip("Only report spots with SNR above this threshold");

            int upload_interval = config_.psk_reporter.upload_interval_sec;
            if (ImGui::SliderInt("Upload Interval (sec)", &upload_interval, 60, 900)) {
                config_.psk_reporter.upload_interval_sec = upload_interval;
            }
            ImGui::SetItemTooltip("How often to batch-upload spots (default 300 = 5 min)");

            ImGui::Separator();
            ImGui::Text("Status:");

            if (state_.psk_reporter_mutex) {
                std::lock_guard<std::mutex> lock(*state_.psk_reporter_mutex);
                if (config_.psk_reporter.enabled) {
                    ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Active");
                    ImGui::SameLine();
                    size_t queue_size = state_.psk_reporter_queue ? state_.psk_reporter_queue->size() : 0;
                    ImGui::Text("(%zu spots queued)", queue_size);
                } else {
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Disabled");
                }
            }

            if (ImGui::Button("Upload Now")) {
                if (onUploadPSKReporterSpots) onUploadPSKReporterSpots();
            }
            ImGui::SameLine();
            ImGui::TextWrapped("Mode: SKYWAVE");

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("UI")) {
            bool dark_mode = config_.ui.dark_mode;
            if (ImGui::Checkbox("Dark Mode", &dark_mode)) {
                config_.ui.dark_mode = dark_mode;
                if (onSetupStyle) onSetupStyle();
            }

            ImGui::SliderInt("Font Size", &config_.ui.font_size, 10, 24);

            bool show_ts = config_.ui.show_timestamps;
            if (ImGui::Checkbox("Show Timestamps", &show_ts)) {
                config_.ui.show_timestamps = show_ts;
            }

            bool sound = config_.ui.sound_notifications;
            if (ImGui::Checkbox("Sound Notifications", &sound)) {
                config_.ui.sound_notifications = sound;
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

} // namespace skylink
