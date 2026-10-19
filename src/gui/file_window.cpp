// FileWindow - File transfer UI

#include "skywave_link/file_window.hpp"
#include "skywave_link/shared_state.hpp"
#include "imgui.h"
#include <cstring>
#include <cstdio>

namespace skylink {

FileWindow::FileWindow(SharedAppState& state) : WindowBase(state) {}

void FileWindow::render() {
    if (!state_.transfers) return;
    auto& transfers = *state_.transfers;

    ImGui::Text("File Transfer");
    ImGui::Separator();

    // File path input
    if (state_.file_path_input) {
        ImGui::InputText("File Path", state_.file_path_input, state_.file_path_input_size);
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) {
            if (onOpenFileDialog) {
                onOpenFileDialog([this](const std::string& selected) {
                    if (!selected.empty() && state_.file_path_input) {
                        std::strncpy(state_.file_path_input, selected.c_str(), state_.file_path_input_size - 1);
                        state_.file_path_input[state_.file_path_input_size - 1] = '\0';
                    }
                });
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Send File")) {
            if (std::strlen(state_.file_path_input) > 0 && onSendFile) {
                onSendFile(state_.file_path_input);
            }
        }

        // Show file type for the input path
        if (std::strlen(state_.file_path_input) > 0) {
            std::string file_type = SharedAppState::getFileTypeDescription(state_.file_path_input);
            ImGui::SameLine();
            if (SharedAppState::isImageFile(state_.file_path_input)) {
                ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.8f, 1.0f), "[%s]", file_type.c_str());
            } else {
                ImGui::TextDisabled("[%s]", file_type.c_str());
            }
        }
    }

    ImGui::Separator();
    ImGui::Text("Supported file types: Images (JPG, PNG, GIF), Logs (ADIF, Cabrillo), QSL Cards, PDFs, Text");
    ImGui::Separator();
    ImGui::Text("Transfers:");

    if (ImGui::BeginChild("TransferList", ImVec2(-1, -1), true)) {
        for (const auto& transfer : transfers) {
            float progress = transfer.total_bytes > 0 ?
                static_cast<float>(transfer.transferred_bytes) / transfer.total_bytes : 0.0f;

            std::string file_type = SharedAppState::getFileTypeDescription(transfer.filename);
            bool is_image = SharedAppState::isImageFile(transfer.filename);

            if (is_image) {
                ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.8f, 1.0f), "[IMG]");
            } else {
                ImGui::TextDisabled("[%s]", file_type.substr(0, 3).c_str());
            }
            ImGui::SameLine();
            ImGui::Text("%s %s", transfer.is_upload ? "UP" : "DN", transfer.filename.c_str());
            ImGui::SameLine(250);
            ImGui::ProgressBar(progress, ImVec2(180, 0));
            ImGui::SameLine();
            if (transfer.completed) {
                ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Complete");
            } else if (transfer.failed) {
                ImGui::TextColored(ImVec4(0.8f, 0.2f, 0.2f, 1.0f), "Failed");
            } else {
                ImGui::Text("%zu / %zu bytes", transfer.transferred_bytes, transfer.total_bytes);
            }
        }
    }
    ImGui::EndChild();
}

} // namespace skylink
