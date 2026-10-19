// Settings Window - Extracted from App::renderSettingsTab()

#include "skywave_link/settings_window.hpp"
#include "skywave_link/aprs_client.hpp"
#include "skywave_link/gui_util.hpp"
#include "skywave_link/mesh_relay.hpp"
#include "skywave_link/modern_style.hpp"
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

static int collectHamlibModel(const struct rig_caps* caps, rig_ptr_t data) {
    auto* models = static_cast<std::vector<HamlibModelEntry>*>(data);
    if (!models || !caps || caps->rig_model == RIG_MODEL_NONE ||
        !caps->mfg_name || !caps->model_name) {
        return 1;
    }

    HamlibModelEntry entry;
    entry.model = static_cast<int>(caps->rig_model);
    entry.manufacturer = skylink::gui::trimCopy(caps->mfg_name);
    entry.name = skylink::gui::trimCopy(caps->model_name);
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

bool SettingsWindow::saveSettings(const std::string& what) {
    std::string error;
    bool ok = false;
    if (onSaveSettings) {
        ok = onSaveSettings(error);
    } else if (state_.config) {
        ok = gui::saveAppConfig(*state_.config, error);
    } else {
        error = "no configuration loaded";
    }

    if (ok) {
        postStatus(what.empty() ? std::string("Settings saved")
                                : what + " applied and saved",
                   StatusKind::Success);
    } else {
        postStatus((what.empty() ? std::string("Settings not saved")
                                 : what + " applied for this session but not saved") +
                       ": " + error,
                   StatusKind::Error);
    }
    return ok;
}

void SettingsWindow::sendModemControl(const std::string& command, const std::string& what) {
    ModemClient* modem = state_.modem;
    if (!modem || !modem->isConnected()) {
        postStatus(what + " not sent: the modem is not connected", StatusKind::Error);
        return;
    }
    std::weak_ptr<int> alive = alive_;
    const bool sent = modem->sendCommandAsync(
        command, [this, alive, command, what](bool ok, const std::string& response) {
            if (alive.expired()) return;
            if (ok) {
                postStatus(what + ": modem accepted " + command, StatusKind::Success);
                parseModemControlReply(response);
            } else {
                postStatus(what + " refused: " +
                               (response.empty() ? std::string("no answer from the modem")
                                                 : response),
                           StatusKind::Error);
            }
            modem_controls_queried_ = false;  // re-read the live values
        });
    if (!sent) {
        postStatus(what + " not sent: modem command channel unavailable", StatusKind::Error);
    }
}

void SettingsWindow::parseModemControlReply(const std::string& response) {
    const std::string reply = gui::upperCopy(gui::trimCopy(response));
    if (reply.rfind("CWID ", 0) == 0) {
        const std::string value = gui::trimCopy(reply.substr(5));
        if (value == "ON") modem_cwid_ = 1;
        else if (value == "OFF") modem_cwid_ = 0;
    } else if (reply.rfind("MAXMODE ", 0) == 0) {
        modem_max_mode_ = gui::trimCopy(reply.substr(8));
    }
}

void SettingsWindow::queryModemControls() {
    ModemClient* modem = state_.modem;
    if (!modem || !modem->isConnected()) {
        modem_controls_queried_ = false;
        modem_cwid_ = -1;
        modem_max_mode_.clear();
        return;
    }
    if (modem_controls_queried_) return;
    modem_controls_queried_ = true;
    std::weak_ptr<int> alive = alive_;
    for (const char* query : {"CWID", "MAXMODE"}) {
        (void)modem->sendCommandAsync(query, [this, alive](bool ok, const std::string& response) {
            if (!alive.expired() && ok) parseModemControlReply(response);
        });
    }
}

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
    const ui::Palette& pal = ui::palette();

    // Most fields take effect at once; "Save Settings" (and every Apply
    // button) writes them to the config file so they survive a crash or
    // force-quit, not only a clean exit.
    ui::PushButtonColor(pal.accent);
    if (ImGui::Button("Save Settings")) {
        saveSettings("");
    }
    ui::PopButtonColor();
    ImGui::SetItemTooltip("Write all settings to %s",
                          AppConfig::getDefaultConfigPath().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("Apply buttons also save.");
    ui::StatusLine(status_);

    // Eleven tabs do not fit the window side by side; scroll instead of
    // squeezing every label down to "Away M...".
    if (ImGui::BeginTabBar("SettingsTabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
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
                const std::string call = gui::normalizeCallsign(callsign_input_);
                if (call.empty()) {
                    postStatus("Enter a callsign first", StatusKind::Error);
                } else {
                    std::strncpy(callsign_input_, call.c_str(), state_.callsign_input_size - 1);
                    callsign_input_[state_.callsign_input_size - 1] = '\0';
                    if (onApplyCallsign) {
                        onApplyCallsign(call);
                    } else {
                        config_.station.callsign = call;
                    }
                    saveSettings("Station settings");
                }
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Modem")) {
            struct ModemFamilyChoice {
                const char* value;
                const char* display;
            };
            // ULTRA is opt-in on the modem: choosing it sends ULTRA ON, then
            // FAMILY ULTRA (App::setModemFamily).
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
                config_.modem.data_port =
                    static_cast<uint16_t>(std::clamp(data_port_input, 1, 65535));
            }

            static int voice_port_input = kDefaultVoicePort;
            voice_port_input = config_.modem.voice_port;
            if (ImGui::InputInt("Voice Port", &voice_port_input)) {
                config_.modem.voice_port =
                    static_cast<uint16_t>(std::clamp(voice_port_input, 1, 65535));
            }
            ImGui::TextDisabled("Host and port changes take effect on the next modem "
                                "connect (Disconnect, then Connect).");

            config_.modem.family = gui::normalizeModemFamily(config_.modem.family);
            int family_idx = current_choice_index(kFamilyChoices, kFamilyChoiceCount,
                                                  config_.modem.family);
            if (ImGui::BeginCombo("Bandwidth Family", kFamilyChoices[family_idx].display)) {
                for (int i = 0; i < kFamilyChoiceCount; ++i) {
                    const bool selected = (family_idx == i);
                    std::string item_label = std::string(kFamilyChoices[i].display) +
                                             "##modem_family_" + kFamilyChoices[i].value;
                    if (ImGui::Selectable(item_label.c_str(), selected) && !selected) {
                        // Applied to the modem at once; a refusal reverts
                        // the choice and says why in the log.
                        if (onSetModemFamily) {
                            onSetModemFamily(kFamilyChoices[i].value);
                        } else {
                            config_.modem.family = kFamilyChoices[i].value;
                        }
                    }
                }
                ImGui::EndCombo();
            }
            const bool modem_up = modem_ && modem_->isConnected();
            const ModemStatus ms = modem_ ? modem_->getStatus() : ModemStatus{};
            if (modem_up) {
                ImGui::TextDisabled("Modem is running %s, mode %s.", ms.family.c_str(),
                                    ms.mode.c_str());
            }
            ImGui::TextDisabled("The modem saves its family and accepts a change "
                                "only while idle.");

            // Live ladder controls (INTERFACES.md "Mode and Link"): sent to
            // the modem at once, never stored in the app's config.
            ImGui::SeparatorText("Link mode");
            queryModemControls();
            if (!modem_up) ImGui::BeginDisabled();
            {
                const std::string mode_preview =
                    !modem_up ? std::string("(modem offline)")
                              : (ms.mode_pinned ? ms.mode : std::string("AUTO"));
                if (ImGui::BeginCombo("Mode", mode_preview.c_str())) {
                    if (ImGui::Selectable("AUTO", !ms.mode_pinned)) {
                        sendModemControl("MODE AUTO", "Mode");
                    }
                    for (int m = gui::kMinLadderMode; m <= gui::kMaxLadderMode; ++m) {
                        const std::string name = gui::ladderModeName(m);
                        if (ImGui::Selectable(name.c_str(), ms.mode_pinned && ms.mode == name)) {
                            sendModemControl("MODE " + name, "Mode");
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("AUTO lets the modem adapt; a fixed mode pins it.");

                const std::string max_preview =
                    !modem_up ? std::string("(modem offline)")
                              : (modem_max_mode_.empty() ? std::string("(unknown)")
                                                         : modem_max_mode_);
                if (ImGui::BeginCombo("Max Mode", max_preview.c_str())) {
                    for (int m = gui::kMinLadderMode; m <= gui::kMaxLadderMode; ++m) {
                        const std::string name = gui::ladderModeName(m);
                        if (ImGui::Selectable(name.c_str(), modem_max_mode_ == name)) {
                            sendModemControl("MAXMODE " + name, "Max mode");
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("Highest mode the modem may promote to (M%d = no cap).",
                                      gui::kMaxLadderMode);

                bool cwid = modem_cwid_ == 1;
                if (ImGui::Checkbox("CW ID after disconnect", &cwid)) {
                    sendModemControl(cwid ? "CWID ON" : "CWID OFF", "CW ID");
                }
                if (modem_up && modem_cwid_ < 0) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(state unknown)");
                }
            }
            if (!modem_up) ImGui::EndDisabled();

            bool auto_connect = config_.modem.auto_connect;
            if (ImGui::Checkbox("Auto-connect on startup", &auto_connect)) {
                config_.modem.auto_connect = auto_connect;
            }

            ImGui::Separator();
            if (ImGui::Button("Connect")) {
                config_.modem.host = modem_host_input_;
                config_.modem.command_port = static_cast<uint16_t>(std::clamp(modem_port_input_, 1, 65535));
                if (onConnectModem) onConnectModem();
            }
            ImGui::SameLine();
            if (ImGui::Button("Disconnect")) {
                if (onDisconnectModem) onDisconnectModem();
            }
            ImGui::SameLine();
            if (ImGui::Button("Apply Modem Settings")) {
                config_.modem.host = modem_host_input_;
                config_.modem.command_port =
                    static_cast<uint16_t>(std::clamp(modem_port_input_, 1, 65535));
                if (onApplyModemConfig) onApplyModemConfig();
                saveSettings("Modem settings");
            }

            // Show connection status
            ImGui::SameLine();
            if (modem_up) {
                ImGui::TextColored(pal.success, "Connected");
            } else {
                ImGui::TextColored(pal.danger, "Disconnected");
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

            std::string backend = gui::normalizeRadioBackend(config_.radio.backend);
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
                if (radio_status.dry_run) {
                    ui::WarningText("DRY-RUN: no rig is controlled (no Hamlib model chosen). "
                                    "Frequency changes and PTT are simulated only; nothing "
                                    "is keyed or tuned.");
                }
                ImGui::Text("PTT Server: %s", radio_status.ptt_server_running ? "Running" : "Stopped");
                if (!radio_status.last_error.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, pal.danger);
                    ImGui::TextWrapped("Last Error: %s", radio_status.last_error.c_str());
                    ImGui::PopStyleColor();
                }
            }

            if (ImGui::Button("Apply Radio Config")) {
                if (onApplyRadioConfig) onApplyRadioConfig();
                saveSettings("Radio settings");
                if (state_.radio) {
                    const RadioController::Status after = state_.radio->getStatus();
                    if (!after.last_error.empty()) {
                        postStatus("Radio settings saved, but the radio reports: " +
                                       after.last_error,
                                   StatusKind::Error);
                    }
                }
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Voice")) {
            const VoiceStatus voice_status =
                modem_ ? modem_->getVoiceStatus() : VoiceStatus{};

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

            // AUTO: Skywave picks the Codec2 rate from the live ladder mode
            // (VOICE.md AUTO table) and steps it down when AMC demotes. A
            // fixed rate is sent as VOICECODEC on Apply Voice Config.
            config_.voice.codec = gui::normalizeVoiceCodec(config_.voice.codec);
            if (ImGui::BeginCombo("Codec", config_.voice.codec.c_str())) {
                for (const std::string& codec : gui::voiceCodecChoices()) {
                    if (ImGui::Selectable(codec.c_str(), config_.voice.codec == codec)) {
                        config_.voice.codec = codec;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled(config_.voice.codec == "AUTO"
                                    ? "AUTO picks the rate from the link mode."
                                    : "A fixed rate the link mode cannot carry in real "
                                      "time is buffered (RT=NO).");

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
                ImGui::TextColored(pal.warning,
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
                saveSettings("Voice settings");
            }
            ImGui::SameLine();
            if (ImGui::Button("Refresh Audio")) {
                if (onRefreshVoiceAudio) onRefreshVoiceAudio();
            }

            static bool ptt_button_held = false;
            if (modem_ && modem_->isConnected()) {
                if (ImGui::Button("Start Voice")) {
                    if (onVoiceStart) onVoiceStart();
                }
                ImGui::SameLine();
                if (ImGui::Button("Stop Voice")) {
                    if (onVoiceStop) onVoiceStop();
                }
                ImGui::SameLine();
                // The colour is pushed and popped under one cached
                // condition: ptt_button_held changes in between.
                const bool ptt_lit = voice_status.ptt || ptt_button_held;
                if (ptt_lit) ui::PushButtonColor(pal.danger);
                ImGui::Button("PTT");
                const bool button_held_now = ImGui::IsItemActive();
                if (ptt_lit) ui::PopButtonColor();
                if (button_held_now != ptt_button_held) {
                    ptt_button_held = button_held_now;
                    if (onVoicePtt) onVoicePtt(ptt_button_held);
                }
            } else if (ptt_button_held) {
                ptt_button_held = false;
                if (onVoicePtt) onVoicePtt(false);
            }

            ImGui::Separator();
            ImGui::Text("Status:");
            if (modem_ && modem_->isConnected() && !modem_->hasVoiceMedia()) {
                ui::WarningText("Voice media socket closed: the modem has no voice port "
                                "open (built without voice, or the connection dropped). "
                                "Voice audio cannot flow until it reopens.");
            } else if (modem_ && modem_->isConnected()) {
                ImGui::TextColored(pal.success, "Voice media socket: open");
            }
            ImGui::Text("Codec: %s", voice_status.codec.c_str());
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
                    // The field above reloads from config_ when it changes.
                    AppConfig* config = &config_;
                    onBrowseDownloadPath([config](const std::string& selected) {
                        if (!selected.empty()) {
                            config->files.download_path = selected;
                        }
                    });
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

            static char trusted_buf[512] = "";
            static std::vector<std::string> last_loaded_trusted;
            static bool trusted_initialised = false;
            if (!trusted_initialised || last_loaded_trusted != config_.files.trusted_callsigns) {
                const std::string joined = joinCallsignList(config_.files.trusted_callsigns);
                std::strncpy(trusted_buf, joined.c_str(), sizeof(trusted_buf) - 1);
                trusted_buf[sizeof(trusted_buf) - 1] = '\0';
                last_loaded_trusted = config_.files.trusted_callsigns;
                trusted_initialised = true;
            }
            ImGui::BeginDisabled(auto_accept);
            if (ImGui::InputText("Trusted Callsigns", trusted_buf, sizeof(trusted_buf))) {
                config_.files.trusted_callsigns = parseCallsignList(trusted_buf);
                last_loaded_trusted = config_.files.trusted_callsigns;
            }
            ImGui::EndDisabled();
            ImGui::TextDisabled(auto_accept
                ? "Auto-accept is on: every offer is accepted."
                : "Offers from these stations (comma-separated) skip the Incoming File prompt.");

            // Creates the folder, hands it to the transfer manager and saves.
            auto apply_download_path = [&](const std::string& path) {
                config_.files.download_path = path.empty() ? std::string("downloads") : path;
                std::strncpy(download_path, config_.files.download_path.c_str(),
                             sizeof(download_path) - 1);
                download_path[sizeof(download_path) - 1] = '\0';
                last_loaded_download_path = config_.files.download_path;
                std::error_code ec;
                std::filesystem::create_directories(config_.files.download_path, ec);
                if (ec) {
                    postStatus("Cannot create " + config_.files.download_path + ": " +
                                   ec.message() + " (received files keep going to the "
                                   "previous folder)",
                               StatusKind::Error);
                    return;
                }
                if (state_.file_transfer_mgr) {
                    state_.file_transfer_mgr->setDownloadPath(config_.files.download_path);
                }
                saveSettings("File settings");
            };
            if (ImGui::Button("Apply File Settings")) {
                apply_download_path(download_path);
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset Path")) {
                apply_download_path("downloads");
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
            static char filter[256] = "";
            static int beacon_interval = 0;
            static std::string aprs_loaded_key;
            {
                const std::string filter_value = aprs_filter ? *aprs_filter : std::string();
                const std::string key = config_.aprs.server + '\x1f' +
                                        std::to_string(config_.aprs.beacon_interval_sec) +
                                        '\x1f' + filter_value;
                if (key != aprs_loaded_key) {
                    std::strncpy(server, config_.aprs.server.c_str(), sizeof(server) - 1);
                    server[sizeof(server) - 1] = '\0';
                    std::strncpy(filter, filter_value.c_str(), sizeof(filter) - 1);
                    filter[sizeof(filter) - 1] = '\0';
                    beacon_interval = config_.aprs.beacon_interval_sec;
                    aprs_loaded_key = key;
                }
            }
            ImGui::InputText("Server", server, sizeof(server));
            if (aprs_filter) {
                ImGui::InputTextWithHint("Receive Filter", "e.g. r/40.7/-74.0/100 b/N0CALL*",
                                         filter, sizeof(filter));
                ImGui::SetItemTooltip("APRS-IS server-side filter sent at login (port 14580 "
                                      "sends nothing without one). Empty = the default "
                                      "range around your position plus your own callsign.");
            }

            // Auto-generated passcode from callsign (APRS-IS standard hash)
            if (!config_.station.callsign.empty()) {
                int32_t code = APRSClient::calculatePasscode(config_.station.callsign);
                ImGui::Text("Passcode: %d (auto-generated from %s)",
                            code, config_.station.callsign.c_str());
            } else {
                ImGui::TextColored(pal.warning, "Passcode: set callsign first");
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
                config_.aprs.gps_baud = std::clamp(gps_baud_val, 1200, 115200);
            }

            // Opens the configured GPS port and says how it went.
            auto open_gps = [&]() -> bool {
                if (config_.aprs.gps_port.empty()) {
                    postStatus("Choose a GPS serial port first", StatusKind::Error);
                    return false;
                }
                if (!onOpenGPSPort) {
                    postStatus("GPS input is not available in this build", StatusKind::Error);
                    return false;
                }
                if (onCloseGPSPort) onCloseGPSPort();
                const bool opened = onOpenGPSPort(config_.aprs.gps_port, config_.aprs.gps_baud);
                if (opened) {
                    postStatus("GPS port " + config_.aprs.gps_port + " opened at " +
                                   std::to_string(config_.aprs.gps_baud) + " baud",
                               StatusKind::Success);
                } else {
                    postStatus("Could not open GPS port " + config_.aprs.gps_port +
                                   " (in use, unplugged or no permission)",
                               StatusKind::Error);
                }
                return opened;
            };

            if (config_.aprs.gps_enabled) {
                if (!gps_port_.empty()) {
                    ImGui::TextColored(pal.success,
                                       "GPS connected: %s @ %d", gps_port_.c_str(), gps_baud_);
                    if (gps_fix_valid_) {
                        ImGui::Text("GPS fix: %.6f, %.6f",
                                    state_.my_lat ? *state_.my_lat : 0.0,
                                    state_.my_lon ? *state_.my_lon : 0.0);
                    } else {
                        ImGui::TextColored(pal.warning, "GPS fix: waiting...");
                    }
                } else {
                    ImGui::TextColored(pal.danger, "GPS disconnected");
                }

                if (ImGui::Button("Open GPS Port")) {
                    (void)open_gps();
                }
                ImGui::SameLine();
                if (ImGui::Button("Close GPS Port")) {
                    if (onCloseGPSPort) onCloseGPSPort();
                    postStatus("GPS port closed", StatusKind::Info);
                }
            }

            if (ImGui::Button("Apply APRS Settings")) {
                config_.aprs.server = server;
                config_.aprs.passcode.clear();  // Always use auto-generated passcode
                config_.aprs.beacon_interval_sec = std::clamp(beacon_interval, 60, 86400);
                beacon_interval = config_.aprs.beacon_interval_sec;
                if (aprs_filter) {
                    *aprs_filter = gui::trimCopy(filter);
                }

                bool gps_ok = true;
                if (!config_.aprs.gps_enabled || config_.aprs.gps_port.empty()) {
                    if (onCloseGPSPort) onCloseGPSPort();
                } else if (gps_port_ != config_.aprs.gps_port || gps_baud_ != config_.aprs.gps_baud ||
                           gps_port_.empty()) {
                    gps_ok = open_gps();
                }

                const bool saved = saveSettings("APRS settings");
                if (saved && gps_ok) {
                    if (onApplyAPRSConfig) {
                        // App (re)connects or disconnects APRS-IS and posts
                        // the outcome here.
                        onApplyAPRSConfig();
                    } else {
                        postStatus("APRS settings saved; the APRS-IS connection uses them "
                                   "after the next restart",
                                   StatusKind::Warning);
                    }
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
                bool smtp_tls = true;
                char pop3_host[256] = "";
                int pop3_port = 0;
                char pop3_user[128] = "";
                bool pop3_tls = true;
                char allowed_relays[512] = "";
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
                    (e.smtp_use_tls ? "1" : "0") + sep +
                    e.pop3_host + sep + std::to_string(e.pop3_port) + sep + e.pop3_username + sep +
                    (e.pop3_use_tls ? "1" : "0") + sep + joinCallsignList(e.allowed_relays) + sep +
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
                    email_edit.smtp_tls = e.smtp_use_tls;
                    copyField(email_edit.pop3_host, sizeof(email_edit.pop3_host), e.pop3_host);
                    email_edit.pop3_port = e.pop3_port;
                    copyField(email_edit.pop3_user, sizeof(email_edit.pop3_user), e.pop3_username);
                    email_edit.pop3_tls = e.pop3_use_tls;
                    copyField(email_edit.allowed_relays, sizeof(email_edit.allowed_relays),
                              joinCallsignList(e.allowed_relays));
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
            EmailEdit& ed = email_edit;

            // Passwords are never shown or reloaded into the field: an empty
            // field on Apply keeps the stored one.
            static char smtp_pass[128] = "";
            static char pop3_pass[128] = "";
            static char winlink_pass[128] = "";
            auto password_hint = [](const std::string& stored) {
                return stored.empty() ? "not set" : "stored; leave blank to keep";
            };

            ImGui::SeparatorText("Outgoing mail (SMTP)");
            ImGui::InputText("SMTP Host", ed.smtp_host, sizeof(ed.smtp_host));
            ImGui::InputInt("SMTP Port", &ed.smtp_port);
            ImGui::InputText("SMTP Username", ed.smtp_user, sizeof(ed.smtp_user));
            ImGui::InputTextWithHint("SMTP Password", password_hint(config_.email.smtp_password),
                                     smtp_pass, sizeof(smtp_pass), ImGuiInputTextFlags_Password);
            ImGui::Checkbox("SMTP TLS", &ed.smtp_tls);
            ImGui::SetItemTooltip("STARTTLS on 587, implicit TLS on 465.");

            ImGui::SeparatorText("Incoming mail (POP3)");
            ImGui::InputText("POP3 Host", ed.pop3_host, sizeof(ed.pop3_host));
            ImGui::InputInt("POP3 Port", &ed.pop3_port);
            ImGui::InputText("POP3 Username", ed.pop3_user, sizeof(ed.pop3_user));
            ImGui::InputTextWithHint("POP3 Password", password_hint(config_.email.pop3_password),
                                     pop3_pass, sizeof(pop3_pass), ImGuiInputTextFlags_Password);
            ImGui::Checkbox("POP3 TLS", &ed.pop3_tls);
            ImGui::SetItemTooltip("Usually port 995 with TLS.");
            if ((!ed.smtp_tls && ed.smtp_host[0]) || (!ed.pop3_tls && ed.pop3_host[0])) {
                ui::WarningText("TLS is off: the password crosses the internet in clear text.");
            }

            ImGui::SeparatorText("Gateway for other stations");
            bool gateway_enabled = config_.email.gateway_enabled;
            if (ImGui::Checkbox("Enable Email Gateway Relay", &gateway_enabled)) {
                config_.email.gateway_enabled = gateway_enabled;
            }
            ImGui::InputTextWithHint("Allowed Relay Callsigns", "e.g. N0CALL, N0CALL-2",
                                     ed.allowed_relays, sizeof(ed.allowed_relays));
            ImGui::SetItemTooltip("Stations allowed to send internet mail through your "
                                  "account (comma-separated).");
            if (gui::trimCopy(ed.allowed_relays).empty()) {
                ImGui::TextDisabled("Empty list: no station may relay through this gateway.");
            }

            ImGui::SeparatorText("Winlink");
            bool winlink_telnet_enabled = config_.email.winlink_telnet_enabled;
            if (ImGui::Checkbox("Use Winlink Telnet (internet)", &winlink_telnet_enabled)) {
                config_.email.winlink_telnet_enabled = winlink_telnet_enabled;
            }

            if (!winlink_telnet_enabled) ImGui::BeginDisabled();
            ImGui::InputText("Winlink Host", ed.winlink_host, sizeof(ed.winlink_host));
            ImGui::InputInt("Winlink Port", &ed.winlink_port);
            ImGui::InputText("Winlink Callsign", ed.winlink_call, sizeof(ed.winlink_call));
            ImGui::InputTextWithHint("Winlink Password",
                                     password_hint(config_.email.winlink_telnet_password),
                                     winlink_pass, sizeof(winlink_pass), ImGuiInputTextFlags_Password);
            ImGui::TextWrapped("The password is only used to answer the CMS secure-login "
                               "challenge (;PQ/;PR); it is never sent to the server. It is stored "
                               "in the system keychain where one is available.");

            bool winlink_tls = config_.email.winlink_telnet_use_tls;
            if (ImGui::Checkbox("Winlink Telnet TLS", &winlink_tls)) {
                config_.email.winlink_telnet_use_tls = winlink_tls;
            }
            if (!winlink_tls) {
                ImGui::TextColored(pal.warning, "Without TLS the session (not the password) is cleartext.");
            }
            ImGui::TextDisabled("Default CMS server: cms.winlink.org:8773 (TLS)");
            if (!winlink_telnet_enabled) ImGui::EndDisabled();

            bool winlink_compat = config_.email.winlink_compat_mode;
            if (ImGui::Checkbox("Winlink-compatible B2F envelope", &winlink_compat)) {
                config_.email.winlink_compat_mode = winlink_compat;
            }

            ImGui::SeparatorText("Weak-Signal Profile");
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

            ImGui::SeparatorText("HF Store-and-Forward");
            bool sf_enabled = config_.email.hf_store_forward_enabled;
            if (ImGui::Checkbox("Enable HF mesh forwarding", &sf_enabled)) {
                config_.email.hf_store_forward_enabled = sf_enabled;
            }
            ImGui::InputInt("Relay TTL", &ed.sf_ttl);
            ImGui::InputInt("Relay Hop Limit", &ed.sf_hop_limit);
            ImGui::InputInt("Relay Dedup Window (sec)", &ed.sf_dedup_window);

            // Learned mesh routes (K16), read-only: a fresh route makes a
            // relay directed (next hop); otherwise packets are flooded.
            if (state_.mesh_relay) {
                const std::vector<RouteEntry> routes = state_.mesh_relay->getRouteTable();
                char routes_label[64];
                std::snprintf(routes_label, sizeof(routes_label), "Mesh routes (%zu)###MeshRoutes",
                              routes.size());
                if (ImGui::TreeNode(routes_label)) {
                    if (routes.empty()) {
                        ImGui::TextDisabled("No routes learned yet (beacons and relayed packets "
                                            "teach them).");
                    } else if (ImGui::BeginTable("MeshRouteTable", 5,
                                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                                     ImGuiTableFlags_SizingStretchProp)) {
                        ImGui::TableSetupColumn("Destination");
                        ImGui::TableSetupColumn("Next hop");
                        ImGui::TableSetupColumn("Hops");
                        ImGui::TableSetupColumn("SNR");
                        ImGui::TableSetupColumn("Age");
                        ImGui::TableHeadersRow();
                        const int64_t now = static_cast<int64_t>(std::time(nullptr));
                        for (const auto& r : routes) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(r.destination.c_str());
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(r.next_hop.c_str());
                            ImGui::TableNextColumn();
                            ImGui::Text("%u", static_cast<unsigned>(r.hop_count));
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(
                                (r.snr_known ? gui::formatSnr(r.snr_db) : std::string("-")).c_str());
                            ImGui::TableNextColumn();
                            const int64_t age = std::max<int64_t>(0, now - r.last_seen);
                            ImGui::Text("%lld min", static_cast<long long>(age / 60));
                        }
                        ImGui::EndTable();
                    }
                    ImGui::TreePop();
                }
            }

            ImGui::SeparatorText("Inbound Polling");
            ImGui::InputInt("POP3 Poll Interval (sec)", &ed.pop3_poll_interval_sec);
            ImGui::InputInt("POP3 Max Messages", &ed.pop3_max_messages);
            ImGui::InputInt("Winlink Fetch Interval (sec)", &ed.winlink_fetch_interval_sec);
            ImGui::InputInt("Winlink Max Messages", &ed.winlink_fetch_max_messages);

            if (ImGui::Button("Apply Email Settings")) {
                auto& e = config_.email;
                e.smtp_host = gui::trimCopy(ed.smtp_host);
                e.smtp_port = static_cast<uint16_t>(std::clamp(ed.smtp_port, 1, 65535));
                e.smtp_username = gui::trimCopy(ed.smtp_user);
                e.smtp_use_tls = ed.smtp_tls;
                if (smtp_pass[0] != '\0') {
                    e.smtp_password = smtp_pass;
                }
                e.pop3_host = gui::trimCopy(ed.pop3_host);
                e.pop3_port = static_cast<uint16_t>(std::clamp(ed.pop3_port, 1, 65535));
                e.pop3_username = gui::trimCopy(ed.pop3_user);
                e.pop3_use_tls = ed.pop3_tls;
                if (pop3_pass[0] != '\0') {
                    e.pop3_password = pop3_pass;
                }
                e.allowed_relays = parseCallsignList(ed.allowed_relays);
                e.winlink_telnet_host = gui::trimCopy(ed.winlink_host);
                e.winlink_telnet_port = static_cast<uint16_t>(std::clamp(ed.winlink_port, 1, 65535));
                e.winlink_telnet_callsign = gui::normalizeCallsign(ed.winlink_call);
                if (winlink_pass[0] != '\0') {
                    e.winlink_telnet_password = winlink_pass;
                }
                e.pop3_poll_interval_sec = std::clamp(ed.pop3_poll_interval_sec, 30, 3600);
                e.pop3_max_messages = std::clamp(ed.pop3_max_messages, 1, 500);
                e.winlink_fetch_interval_sec = std::clamp(ed.winlink_fetch_interval_sec, 30, 3600);
                e.winlink_fetch_max_messages = std::clamp(ed.winlink_fetch_max_messages, 1, 500);
                e.hf_store_forward_ttl = std::clamp(ed.sf_ttl, 1, 15);
                e.hf_store_forward_hop_limit = std::clamp(ed.sf_hop_limit, 1, 20);
                e.hf_store_forward_dedup_window_sec = std::clamp(ed.sf_dedup_window, 30, 3600);
                // Do not keep secrets in the edit buffers once stored.
                std::memset(smtp_pass, 0, sizeof(smtp_pass));
                std::memset(pop3_pass, 0, sizeof(pop3_pass));
                std::memset(winlink_pass, 0, sizeof(winlink_pass));

                if (e.gateway_enabled && !email_gateway_active_) {
                    if (onStartEmailGateway) onStartEmailGateway();
                } else if (!e.gateway_enabled && email_gateway_active_) {
                    if (onStopEmailGateway) onStopEmailGateway();
                }
                if (saveSettings("Email settings") && e.gateway_enabled &&
                    !email_gateway_active_) {
                    postStatus("Email settings saved, but the gateway did not start (check "
                               "the host/account fields and the Log)",
                               StatusKind::Error);
                }
            }

            ImGui::SeparatorText("Relay Diagnostics");
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
                ImGui::TextColored(healthy ? pal.success : pal.danger,
                                   "Gateway Health: %s", diag.gateway_health.c_str());
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
                if (!diag.last_error.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, pal.danger);
                    ImGui::TextWrapped("Last Error: %s", diag.last_error.c_str());
                    ImGui::PopStyleColor();
                } else {
                    ImGui::TextDisabled("Last Error: none");
                }
            } else {
                ImGui::TextDisabled("Relay diagnostics unavailable");
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
                ImGui::TextColored(pal.success, "Enabled");
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
                ImGui::TextDisabled("Disabled");
            }

            if (ImGui::Button("Send Beacon Now")) {
                if (onSendBeacon) onSendBeacon();
            }
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Away Mode")) {
            static char away_msg[512] = "";
            std::strncpy(away_msg, config_.away.away_message.c_str(), sizeof(away_msg) - 1);
            if (ImGui::InputTextMultiline("Away Message", away_msg, sizeof(away_msg),
                                          ImVec2(ImGui::CalcItemWidth(), ImGui::GetTextLineHeight() * 4.0f))) {
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
                ImGui::TextColored(pal.warning, "Away Mode: ACTIVE");
                ImGui::BeginDisabled(!onDisableAwayMode);
                if (ImGui::Button("Disable Away Mode")) {
                    onDisableAwayMode();
                }
                ImGui::EndDisabled();
            } else {
                ImGui::Text("Away Mode: Inactive");
                ImGui::BeginDisabled(!onEnableAwayMode);
                if (ImGui::Button("Enable Away Mode")) {
                    onEnableAwayMode();
                }
                ImGui::EndDisabled();
            }
            if (!onEnableAwayMode || !onDisableAwayMode) {
                ImGui::TextDisabled("Away mode control is not available in this build.");
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

            ImGui::TextWrapped("Spots are uploaded about every 5 minutes, the shortest interval "
                               "pskreporter.info allows; a station is reported at most once per 5 "
                               "minutes (once an hour while band and mode are unchanged). Spots "
                               "need a rig-reported dial frequency.");

            ImGui::Separator();
            ImGui::Text("Status:");
            auto clock_text = [](uint64_t ts) -> std::string {
                if (ts == 0) return "never";
                std::time_t t = static_cast<std::time_t>(ts);
                std::tm local_tm = *std::localtime(&t);
                char buf[32];
                std::strftime(buf, sizeof(buf), "%H:%M:%S", &local_tm);
                return buf;
            };
            if (!config_.psk_reporter.enabled) {
                ImGui::TextDisabled("Disabled");
            } else if (!state_.psk_status || !state_.psk_status->active) {
                ImGui::TextColored(pal.warning, "Not running: set your callsign (Settings > Station)");
            } else {
                const PskReporterStatus& st = *state_.psk_status;
                if (!st.last_error.empty()) {
                    ImGui::TextColored(pal.danger, "Error: %s", st.last_error.c_str());
                } else if (st.upload_in_progress) {
                    ImGui::TextColored(pal.info, "Uploading...");
                } else {
                    ImGui::TextColored(pal.success, "Running");
                }
                ImGui::Text("Queued spots: %zu   Sent this session: %llu", st.queued_spots,
                            static_cast<unsigned long long>(st.spots_sent));
                ImGui::Text("Last upload: %s", clock_text(st.last_upload_unix).c_str());
                if (st.next_upload_unix > 0 && st.queued_spots > 0) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(next due %s)", clock_text(st.next_upload_unix).c_str());
                }
            }

            ImGui::BeginDisabled(!onUploadPSKReporterSpots || !config_.psk_reporter.enabled);
            if (ImGui::Button("Upload Now")) {
                onUploadPSKReporterSpots();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextWrapped("Mode: SKYWAVE");

            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("UI")) {
            bool dark_mode = config_.ui.dark_mode;
            auto apply_style = [&]() {
                if (onSetupStyle) {
                    onSetupStyle();
                } else {
                    ui::applyModernStyle(config_.ui.dark_mode,
                                         static_cast<float>(config_.ui.font_size));
                }
            };
            if (ImGui::Checkbox("Dark Mode", &dark_mode)) {
                config_.ui.dark_mode = dark_mode;
                apply_style();
            }

            ImGui::SliderInt("Font Size", &config_.ui.font_size, 10, 24);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                apply_style();  // on release: re-styling mid-drag would jitter the slider
            }

            bool show_ts = config_.ui.show_timestamps;
            if (ImGui::Checkbox("Show Timestamps", &show_ts)) {
                config_.ui.show_timestamps = show_ts;
            }
            ImGui::SetItemTooltip("Time column in the chat and the Log panel.");

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
