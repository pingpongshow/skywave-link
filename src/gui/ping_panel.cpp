// PingPanel - Ping / status check display

#include "skywave_link/ping_panel.hpp"
#include "imgui.h"
#include <cstring>
#include <ctime>

namespace skylink {

PingPanel::PingPanel(SharedAppState& state) : WindowBase(state) {}

void PingPanel::render() {
    if (!state_.ping_results) return;
    auto& ping_results = *state_.ping_results;

    ImGui::Text("Ping / Status Check");
    ImGui::Separator();

    static char ping_callsign[32] = "";
    ImGui::InputText("Callsign", ping_callsign, sizeof(ping_callsign));
    ImGui::SameLine();
    if (ImGui::Button("Ping")) {
        if (std::strlen(ping_callsign) > 0 && onSendPing) {
            onSendPing(ping_callsign);
        }
    }

    if (state_.ping_in_progress && *state_.ping_in_progress) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.2f, 1.0f), "Waiting...");
    }

    ImGui::Separator();
    ImGui::Text("Recent Results:");

    if (ImGui::BeginChild("PingResults", ImVec2(-1, 150), true)) {
        if (ping_results.empty()) {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No ping results yet");
        } else {
            ImGui::Columns(4, "PingColumns");
            ImGui::Text("Callsign"); ImGui::NextColumn();
            ImGui::Text("Grid"); ImGui::NextColumn();
            ImGui::Text("SNR"); ImGui::NextColumn();
            ImGui::Text("Status"); ImGui::NextColumn();
            ImGui::Separator();

            for (const auto& result : ping_results) {
                ImGui::Text("%s", result.callsign.c_str());
                ImGui::NextColumn();

                ImGui::Text("%s", result.grid_square.c_str());
                ImGui::NextColumn();

                if (result.online) {
                    ImGui::Text("%.0f dB", result.snr_db);
                } else {
                    ImGui::Text("-");
                }
                ImGui::NextColumn();

                if (result.online) {
                    ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Online");
                } else {
                    ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.2f, 1.0f), "Timeout");
                }
                ImGui::NextColumn();
            }
            ImGui::Columns(1);
        }
    }
    ImGui::EndChild();
}

} // namespace skylink
