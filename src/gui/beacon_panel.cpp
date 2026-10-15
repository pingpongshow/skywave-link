// BeaconPanel - Heard beacons display

#include "skywave_link/beacon_panel.hpp"
#include "imgui.h"
#include <cstring>
#include <ctime>

namespace skylink {

BeaconPanel::BeaconPanel(SharedAppState& state) : WindowBase(state) {}

void BeaconPanel::render() {
    if (!state_.heard_beacons) return;
    auto& heard_beacons = *state_.heard_beacons;

    ImGui::Text("Heard Beacons");
    ImGui::Separator();

    if (ImGui::BeginChild("HeardBeacons", ImVec2(-1, 200), true)) {
        if (heard_beacons.empty()) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No beacons heard yet");
        } else {
            ImGui::Columns(5, "BeaconColumns");
            ImGui::Text("Callsign"); ImGui::NextColumn();
            ImGui::Text("Grid"); ImGui::NextColumn();
            ImGui::Text("Message"); ImGui::NextColumn();
            ImGui::Text("SNR"); ImGui::NextColumn();
            ImGui::Text("Time"); ImGui::NextColumn();
            ImGui::Separator();

            for (const auto& beacon : heard_beacons) {
                // Highlight if we have pending messages; show @ for email gateway
                if (beacon.has_pending_messages) {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s *", beacon.callsign.c_str());
                } else if (beacon.email_gateway) {
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "%s @", beacon.callsign.c_str());
                } else {
                    ImGui::Text("%s", beacon.callsign.c_str());
                }
                if (beacon.email_gateway && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Email gateway - can relay messages to internet email");
                }
                ImGui::NextColumn();

                ImGui::Text("%s", beacon.grid_square.c_str());
                ImGui::NextColumn();

                ImGui::Text("%s", beacon.message.c_str());
                ImGui::NextColumn();

                ImGui::Text("%.0f dB", beacon.snr_db);
                ImGui::NextColumn();

                auto time_t_val = static_cast<time_t>(beacon.timestamp);
                char time_str[32];
                std::strftime(time_str, sizeof(time_str), "%H:%M:%S", std::localtime(&time_t_val));
                ImGui::Text("%s", time_str);
                ImGui::NextColumn();
            }
            ImGui::Columns(1);
        }
    }
    ImGui::EndChild();
}

} // namespace skylink
