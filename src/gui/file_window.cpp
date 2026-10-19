// FileWindow - File transfer UI

#include "skywave_link/file_window.hpp"
#include "skywave_link/gui_util.hpp"
#include "skywave_link/modern_style.hpp"
#include "skywave_link/shared_state.hpp"
#include "imgui.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>

namespace skylink {

namespace {

int64_t unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string formatBytes(size_t bytes) {
    char buf[32];
    if (bytes >= 1024 * 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%zu B", bytes);
    }
    return buf;
}

bool isRunning(const FileTransfer& t) {
    return !t.completed && !t.failed && !t.cancelled;
}

}  // namespace

FileWindow::FileWindow(SharedAppState& state) : WindowBase(state) {}

void FileWindow::sendSelectedFile() {
    const std::string path = gui::trimCopy(state_.file_path_input ? state_.file_path_input : "");
    if (path.empty()) {
        postStatus("Enter or choose a file first", StatusKind::Error);
        return;
    }
    std::error_code ec;
    const std::filesystem::path fs_path(path);
    if (!std::filesystem::is_regular_file(fs_path, ec)) {
        postStatus("No such file: " + path, StatusKind::Error);
        return;
    }
    const auto size = std::filesystem::file_size(fs_path, ec);
    const size_t max_size = state_.config ? state_.config->files.max_file_size : 0;
    if (!ec && max_size > 0 && size > max_size) {
        postStatus(fs_path.filename().string() + " is " + formatBytes(static_cast<size_t>(size)) +
                       "; the limit is " + formatBytes(max_size) + " (Settings > Files)",
                   StatusKind::Error);
        return;
    }
    if (!gui::isStationLinked(state_.modem)) {
        postStatus("Connect to a station first: files go over a linked session",
                   StatusKind::Error);
        return;
    }
    if (!onSendFile) {
        postStatus("File sending is not available", StatusKind::Error);
        return;
    }

    // sendFile() reports in the log; a new upload entry means the offer
    // went out.
    size_t uploads_before = 0;
    if (state_.transfers) {
        for (const auto& t : *state_.transfers) uploads_before += t.is_upload ? 1 : 0;
    }
    const size_t log_mark = state_.chatHistorySize();
    onSendFile(path);
    size_t uploads_after = 0;
    if (state_.transfers) {
        for (const auto& t : *state_.transfers) uploads_after += t.is_upload ? 1 : 0;
    }
    if (uploads_after > uploads_before) {
        postStatus("Offered " + fs_path.filename().string() + "; waiting for the other station "
                   "to accept",
                   StatusKind::Success);
    } else {
        const std::string reason = state_.systemMessageSince(log_mark);
        postStatus(reason.empty() ? std::string("File not sent") : reason, StatusKind::Error);
    }
}

void FileWindow::cancelTransfer(const FileTransfer& transfer) {
    if (transfer.transfer_id.empty()) return;
    if (onCancelTransfer) {
        onCancelTransfer(transfer.transfer_id);
    } else if (state_.file_transfer_mgr && state_.modem) {
        state_.file_transfer_mgr->cancelTransfer(*state_.modem, transfer.transfer_id);
    } else {
        postStatus("Cannot cancel: file transfer service unavailable", StatusKind::Error);
        return;
    }
    postStatus("Cancelled " + transfer.filename, StatusKind::Info);
}

void FileWindow::render() {
    if (!state_.transfers) return;
    const ui::Palette& pal = ui::palette();

    ImGui::Text("File Transfer");
    ImGui::Separator();
    ui::StatusLine(status_);

    // File path input
    if (state_.file_path_input) {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float buttons = ImGui::CalcTextSize("Browse...").x + ImGui::CalcTextSize("Send File").x +
                              style.FramePadding.x * 4.0f + style.ItemSpacing.x * 3.0f +
                              ImGui::CalcTextSize("File Path").x + style.ItemInnerSpacing.x;
        ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 8.0f,
                                         ImGui::GetContentRegionAvail().x - buttons));
        ImGui::InputText("File Path", state_.file_path_input, state_.file_path_input_size);
        ImGui::SameLine();
        ImGui::BeginDisabled(!onOpenFileDialog);
        if (ImGui::Button("Browse...")) {
            onOpenFileDialog([this](const std::string& selected) {
                if (!selected.empty() && state_.file_path_input) {
                    std::strncpy(state_.file_path_input, selected.c_str(), state_.file_path_input_size - 1);
                    state_.file_path_input[state_.file_path_input_size - 1] = '\0';
                }
            });
        }
        ImGui::EndDisabled();
        if (!onOpenFileDialog) {
            ImGui::SetItemTooltip("No file picker on this platform: type the path.");
        }

        ImGui::SameLine();
        ui::PushButtonColor(pal.accent);
        if (ImGui::Button("Send File")) {
            sendSelectedFile();
        }
        ui::PopButtonColor();

        // Show file type for the input path
        if (std::strlen(state_.file_path_input) > 0) {
            const std::string file_type = gui::fileTypeDescription(state_.file_path_input);
            if (gui::isImageFile(state_.file_path_input)) {
                ImGui::TextColored(pal.info, "[%s]", file_type.c_str());
            } else {
                ImGui::TextDisabled("[%s]", file_type.c_str());
            }
        }
    }

    ImGui::Separator();
    ImGui::TextWrapped("Supported file types: Images (JPG, PNG, GIF, WebP), Logs (ADIF, Cabrillo), "
                       "QSL Cards, PDFs, Text. Limit: %s.",
                       formatBytes(state_.config ? state_.config->files.max_file_size : 0).c_str());
    ImGui::Separator();
    ImGui::Text("Transfers:");

    std::optional<FileTransfer> cancel_request;
    if (ImGui::BeginChild("TransferList", ImVec2(-1, -1), ImGuiChildFlags_Borders)) {
        if (state_.transfers->empty()) {
            ImGui::TextDisabled("No transfers yet.");
        }
        const float em = ImGui::GetFontSize();
        const int64_t now = unixNow();
        int row = 0;
        for (const auto& transfer : *state_.transfers) {
            ImGui::PushID(row++);
            const float progress = transfer.total_bytes > 0
                ? static_cast<float>(transfer.transferred_bytes) / static_cast<float>(transfer.total_bytes)
                : (transfer.completed ? 1.0f : 0.0f);

            if (gui::isImageFile(transfer.filename)) {
                ImGui::TextColored(pal.info, "[IMG]");
            } else {
                ImGui::TextDisabled("[%s]", gui::fileTypeDescription(transfer.filename).substr(0, 3).c_str());
            }
            ImGui::SameLine();
            ImGui::Text("%s %s %s", transfer.is_upload ? "UP to" : "DN from",
                        transfer.remote_callsign.empty() ? "?" : transfer.remote_callsign.c_str(),
                        transfer.filename.c_str());

            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "%.0f%%  %s / %s", progress * 100.0f,
                          formatBytes(transfer.transferred_bytes).c_str(),
                          formatBytes(transfer.total_bytes).c_str());
            const bool running = isRunning(transfer);
            const float cancel_w = (running && !transfer.transfer_id.empty())
                ? ImGui::CalcTextSize("Cancel").x + ImGui::GetStyle().FramePadding.x * 2.0f +
                      ImGui::GetStyle().ItemSpacing.x
                : 0.0f;
            ImGui::ProgressBar(progress, ImVec2(std::max(em * 8.0f, ImGui::GetContentRegionAvail().x - cancel_w), 0),
                               overlay);
            if (cancel_w > 0.0f) {
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) {
                    cancel_request = transfer;
                }
            }

            if (transfer.completed) {
                ImGui::TextColored(pal.success, "Complete");
            } else if (transfer.cancelled) {
                ImGui::TextColored(pal.warning, "Cancelled");
            } else if (transfer.failed) {
                ImGui::PushStyleColor(ImGuiCol_Text, pal.danger);
                ImGui::TextWrapped("Failed%s%s", transfer.error.empty() ? "" : ": ",
                                   transfer.error.c_str());
                ImGui::PopStyleColor();
            } else {
                // Rate over the transfer so far (bytes/s) for the ETA.
                const int64_t elapsed = transfer.started_at > 0 ? now - transfer.started_at : 0;
                if (elapsed > 0 && transfer.transferred_bytes > 0 &&
                    transfer.total_bytes > transfer.transferred_bytes) {
                    const double rate = static_cast<double>(transfer.transferred_bytes) /
                                        static_cast<double>(elapsed);
                    const auto eta = static_cast<int64_t>(
                        static_cast<double>(transfer.total_bytes - transfer.transferred_bytes) / rate);
                    ImGui::TextDisabled("%.0f B/s, about %s left", rate, gui::formatDuration(eta).c_str());
                } else {
                    ImGui::TextDisabled(transfer.transferred_bytes == 0 ? "Waiting to start"
                                                                        : "Transferring");
                }
            }
            ImGui::Separator();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if (cancel_request) {
        cancelTransfer(*cancel_request);
    }
}

} // namespace skylink
