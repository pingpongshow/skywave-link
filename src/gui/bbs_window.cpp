#include "skywave_link/bbs_window.hpp"
#include "skywave_link/gui_util.hpp"
#include "skywave_link/modern_style.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace skylink {

namespace {

// Log lines that mean the action did not happen.
bool looksLikeFailure(const std::string& text) {
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* word : {"fail", "error", "cannot", "can't", "not ", "denied", "refused",
                             "unable", "invalid", "missing", "too large"}) {
        if (lower.find(word) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

BBSWindow::BBSWindow(SharedAppState& state)
    : WindowBase(state)
{
}

template <typename Fn>
void BBSWindow::runAndReport(Fn&& action, const std::string& fallback) {
    const size_t mark = state_.chatHistorySize();
    action();
    const std::string logged = state_.systemMessageSince(mark);
    if (logged.empty()) {
        postStatus(fallback, StatusKind::Info);
    } else {
        postStatus(logged, looksLikeFailure(logged) ? StatusKind::Error : StatusKind::Success);
    }
}

void BBSWindow::render() {
    const ui::Palette& pal = ui::palette();
    ImGui::Text("Bulletin Board System");
    ImGui::Separator();
    ui::StatusLine(status_);

    // BBS enable toggle
    bool bbs_enabled = state_.config->bbs.enabled;
    if (ImGui::Checkbox("Enable BBS", &bbs_enabled)) {
        state_.config->bbs.enabled = bbs_enabled;
        std::string error;
        if (gui::saveAppConfig(*state_.config, error)) {
            postStatus(bbs_enabled ? "BBS enabled" : "BBS disabled", StatusKind::Success);
        } else {
            postStatus(std::string("BBS ") + (bbs_enabled ? "enabled" : "disabled") +
                           " for this session, but not saved: " + error,
                       StatusKind::Error);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When enabled, other stations can connect and download files.");
    }

    if (!state_.config->bbs.enabled) {
        ImGui::TextDisabled("BBS is disabled. Enable to share files.");
        // Uploading to another station's BBS needs no local BBS.
        ImGui::Separator();
        ImGui::Text("Upload a file to the linked station's BBS:");
        renderPathInput();
        renderRemoteUploadButton();
        return;
    }

    ImGui::Separator();

    // Upload section
    ImGui::Text("Upload File to BBS:");
    renderPathInput();
    ImGui::InputText("Description", state_.bbs_file_desc, state_.bbs_file_desc_size);

    if (ImGui::Button("Upload")) {
        const std::string path = gui::trimCopy(state_.bbs_upload_path);
        std::error_code ec;
        if (path.empty()) {
            postStatus("Enter or choose a file to upload", StatusKind::Error);
        } else if (!std::filesystem::is_regular_file(path, ec)) {
            postStatus("No such file: " + path, StatusKind::Error);
        } else if (!onUpload) {
            postStatus("BBS upload is not available", StatusKind::Error);
        } else {
            const size_t files_before = state_.bbs_files ? state_.bbs_files->size() : 0;
            const size_t mark = state_.chatHistorySize();
            onUpload(path, state_.bbs_file_desc);
            const size_t files_after = state_.bbs_files ? state_.bbs_files->size() : 0;
            const std::string logged = state_.systemMessageSince(mark);
            const bool failed = looksLikeFailure(logged) ||
                                (logged.empty() && files_after <= files_before);
            if (failed) {
                // Keep the fields so the operator can fix and retry.
                postStatus(logged.empty() ? "Upload failed" : logged, StatusKind::Error);
            } else {
                postStatus(logged.empty() ? "Added " + std::filesystem::path(path).filename().string()
                                          : logged,
                           StatusKind::Success);
                state_.bbs_upload_path[0] = '\0';
                state_.bbs_file_desc[0] = '\0';
            }
        }
    }

    ImGui::SameLine();
    renderRemoteUploadButton();

    ImGui::Separator();
    ImGui::Text("BBS Files:");

    // Refresh button
    if (ImGui::Button("Refresh")) {
        if (onRefreshFiles) {
            onRefreshFiles();
            postStatus("File list refreshed (" +
                           std::to_string(state_.bbs_files ? state_.bbs_files->size() : 0) +
                           " files)",
                       StatusKind::Info);
        }
    }

    // File list
    const float em = ImGui::GetFontSize();
    if (ImGui::BeginChild("BBSFileList", ImVec2(-1, em * 16.0f), ImGuiChildFlags_Borders)) {
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
                                      ImGuiTableFlags_BordersInnerH;
        if (ImGui::BeginTable("BBSColumns", 5, flags)) {
            ImGui::TableSetupColumn("Filename", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch, 2.4f);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed,
                                    ImGui::CalcTextSize("Download Delete").x + em * 1.5f);
            ImGui::TableHeadersRow();

            std::string delete_name;
            const BBSFile* download_file = nullptr;
            for (size_t i = 0; state_.bbs_files && i < state_.bbs_files->size(); ++i) {
                const auto& file = (*state_.bbs_files)[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(file.filename.c_str());
                ImGui::TableNextColumn();
                if (file.remote) {
                    ImGui::TextColored(pal.info, "%s", file.uploader.c_str());
                } else {
                    ImGui::TextUnformatted("Local");
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(file.description.c_str());
                ImGui::TableNextColumn();
                if (file.size_bytes >= 1024) {
                    ImGui::Text("%.1f KB", static_cast<double>(file.size_bytes) / 1024.0);
                } else {
                    ImGui::Text("%zu B", file.size_bytes);
                }
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::SmallButton(file.remote ? "Fetch" : "Download")) {
                    download_file = &file;
                }
                if (!file.remote) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Delete")) {
                        delete_name = file.filename;
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (state_.bbs_files && state_.bbs_files->empty()) {
                ImGui::TextDisabled("No files. Upload one, or press Refresh.");
            }

            if (download_file && onDownload) {
                const BBSFile file = *download_file;
                runAndReport([&]() { onDownload(file); },
                             file.remote ? "Requested " + file.filename + " from " + file.uploader
                                         : "Saved " + file.filename);
            }
            if (!delete_name.empty() && onDelete) {
                runAndReport([&]() { onDelete(delete_name); }, "Deleted " + delete_name);
            }
        }
    }
    ImGui::EndChild();

    ImGui::Separator();
    ImGui::Text("BBS Download Progress:");
    bool has_bbs_download = false;
    if (state_.pending_bbs_downloads) {
        for (const auto& pending : *state_.pending_bbs_downloads) {
            has_bbs_download = true;
            const std::string owner = pending.owner_callsign.empty() ? "unknown" : pending.owner_callsign;
            ImGui::Text("%s from %s", pending.filename.c_str(), owner.c_str());

            const float pulse = 0.20f + 0.75f *
                static_cast<float>(std::fmod(ImGui::GetTime() * 0.6, 1.0));
            const char* label = pending.request_sent
                ? "Request sent; waiting for transfer start"
                : "Queued; connecting to source station";
            ImGui::ProgressBar(pending.request_sent ? pulse : 0.05f, ImVec2(-1, 0), label);
            ImGui::Separator();
        }
    }

    if (state_.transfers) {
        std::vector<const FileTransfer*> downloads;
        downloads.reserve(state_.transfers->size());
        for (const auto& transfer : *state_.transfers) {
            if (transfer.is_upload) {
                continue;
            }
            downloads.push_back(&transfer);
        }

        std::sort(downloads.begin(), downloads.end(),
                  [](const FileTransfer* a, const FileTransfer* b) {
                      return a->started_at > b->started_at;
                  });

        constexpr size_t kMaxShown = 10;
        const size_t shown = std::min(downloads.size(), kMaxShown);
        for (size_t i = 0; i < shown; ++i) {
            const auto& transfer = *downloads[i];
            has_bbs_download = true;
            const float progress = transfer.total_bytes > 0
                ? static_cast<float>(transfer.transferred_bytes) /
                  static_cast<float>(transfer.total_bytes)
                : (transfer.completed ? 1.0f : 0.0f);
            ImGui::Text("%s from %s",
                        transfer.filename.c_str(),
                        transfer.remote_callsign.empty() ? "remote" : transfer.remote_callsign.c_str());
            if (transfer.completed) {
                ImGui::ProgressBar(1.0f, ImVec2(-1, 0), "Download complete");
                ImGui::TextDisabled("%zu / %zu bytes",
                                    transfer.transferred_bytes,
                                    transfer.total_bytes);
            } else if (transfer.failed || transfer.cancelled) {
                ImGui::ProgressBar(progress, ImVec2(-1, 0),
                                   transfer.cancelled ? "Download cancelled" : "Download failed");
                ImGui::PushStyleColor(ImGuiCol_Text, pal.danger);
                ImGui::TextWrapped("%s", transfer.error.empty() ? "Transfer failed"
                                                                : transfer.error.c_str());
                ImGui::PopStyleColor();
            } else {
                ImGui::ProgressBar(progress, ImVec2(-1, 0), "Downloading");
                ImGui::TextDisabled("%zu / %zu bytes",
                                    transfer.transferred_bytes,
                                    transfer.total_bytes);
            }
            ImGui::Separator();
        }
    }
    if (!has_bbs_download) {
        ImGui::TextDisabled("No BBS download is currently in progress.");
    }

    renderSettings();
}

void BBSWindow::renderPathInput() {
    ImGui::InputText("File Path", state_.bbs_upload_path, state_.bbs_upload_path_size);
    ImGui::SameLine();
    ImGui::BeginDisabled(!onOpenFileDialog);
    if (ImGui::Button("Browse##BBS")) {
        onOpenFileDialog([this](const std::string& selected) {
            if (!selected.empty() && state_.bbs_upload_path) {
                std::strncpy(state_.bbs_upload_path, selected.c_str(), state_.bbs_upload_path_size - 1);
                state_.bbs_upload_path[state_.bbs_upload_path_size - 1] = '\0';
            }
        });
    }
    ImGui::EndDisabled();
    if (!onOpenFileDialog) {
        ImGui::SetItemTooltip("No file picker on this platform: type the path.");
    }

}

void BBSWindow::renderRemoteUploadButton() {
    // Upload to the linked station's BBS (it accepts only when its operator
    // allows uploads from this callsign).
    const bool linked = gui::isStationLinked(state_.modem);
    ImGui::BeginDisabled(!linked || !onUploadToRemote);
    if (ImGui::Button("Upload to remote BBS")) {
        const std::string path = gui::trimCopy(state_.bbs_upload_path);
        std::error_code ec;
        if (path.empty()) {
            postStatus("Enter or choose a file to upload", StatusKind::Error);
        } else if (!std::filesystem::is_regular_file(path, ec)) {
            postStatus("No such file: " + path, StatusKind::Error);
        } else {
            const size_t mark = state_.chatHistorySize();
            const bool started = onUploadToRemote(path);
            const std::string logged = state_.systemMessageSince(mark);
            if (started) {
                postStatus(logged.empty() ? "Upload offered to the remote BBS" : logged,
                           StatusKind::Success);
            } else {
                postStatus(logged.empty() ? "Upload to the remote BBS not started" : logged,
                           StatusKind::Error);
            }
        }
    }
    ImGui::EndDisabled();
    if (!linked) {
        ImGui::SetItemTooltip("Link to a station first; its BBS must allow uploads from you.");
    }

}

void BBSWindow::renderSettings() {
    ImGui::Separator();
    if (!ImGui::CollapsingHeader("BBS Settings")) {
        return;
    }
    auto& bbs = state_.config->bbs;

    // Edit buffers, reloaded only when the config changes underneath.
    static char welcome[256] = "";
    static char bbs_dir[256] = "";
    static char uploaders[512] = "";
    static bool allow_uploads = false;
    static int max_upload_kb = 50;
    static std::string loaded_key;
    const std::string key = bbs.welcome_message + '\x1f' + bbs.bbs_path + '\x1f' +
                            joinCallsignList(bbs.allowed_uploaders) + '\x1f' +
                            (bbs.allow_uploads ? "1" : "0") + '\x1f' +
                            std::to_string(bbs.max_upload_size);
    if (key != loaded_key) {
        std::snprintf(welcome, sizeof(welcome), "%s", bbs.welcome_message.c_str());
        std::snprintf(bbs_dir, sizeof(bbs_dir), "%s", bbs.bbs_path.c_str());
        std::snprintf(uploaders, sizeof(uploaders), "%s",
                      joinCallsignList(bbs.allowed_uploaders).c_str());
        allow_uploads = bbs.allow_uploads;
        max_upload_kb = static_cast<int>(bbs.max_upload_size / 1024);
        loaded_key = key;
    }

    ImGui::InputText("Welcome Message", welcome, sizeof(welcome));
    ImGui::SetItemTooltip("Sent to a station when it opens a BBS session.");
    ImGui::InputText("BBS Directory", bbs_dir, sizeof(bbs_dir));

    ImGui::Checkbox("Allow Remote Uploads", &allow_uploads);
    ImGui::BeginDisabled(!allow_uploads);
    ImGui::InputTextWithHint("Allowed Uploaders", "e.g. N0CALL, N0CALL-2", uploaders,
                             sizeof(uploaders));
    ImGui::SetItemTooltip("Only these stations may upload (comma-separated). Empty: nobody.");
    ImGui::SliderInt("Max Upload (KB)", &max_upload_kb, 1, 1024);
    ImGui::EndDisabled();
    if (allow_uploads && gui::trimCopy(uploaders).empty()) {
        ImGui::TextDisabled("No uploader listed: remote uploads stay closed.");
    }

    if (ImGui::Button("Apply BBS Settings")) {
        const std::string dir = gui::trimCopy(bbs_dir).empty() ? std::string("bbs")
                                                               : gui::trimCopy(bbs_dir);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            postStatus("Cannot use BBS directory " + dir + ": " + ec.message(), StatusKind::Error);
            return;
        }
        const bool dir_changed = dir != bbs.bbs_path;
        bbs.welcome_message = welcome;
        bbs.bbs_path = dir;
        bbs.allow_uploads = allow_uploads;
        bbs.allowed_uploaders = parseCallsignList(uploaders);
        bbs.max_upload_size = static_cast<size_t>(std::clamp(max_upload_kb, 1, 1024)) * 1024;
        if (dir_changed && onRefreshFiles) {
            onRefreshFiles();  // list the new folder's files
        }
        std::string error;
        if (gui::saveAppConfig(*state_.config, error)) {
            postStatus("BBS settings applied and saved", StatusKind::Success);
        } else {
            postStatus("BBS settings applied for this session but not saved: " + error,
                       StatusKind::Error);
        }
    }
}

} // namespace skylink
