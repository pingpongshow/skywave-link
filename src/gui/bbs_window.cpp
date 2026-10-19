#include "skywave_link/bbs_window.hpp"
#include <imgui.h>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <vector>

namespace skylink {

BBSWindow::BBSWindow(SharedAppState& state)
    : WindowBase(state)
{
}

void BBSWindow::render() {
    ImGui::Text("Bulletin Board System");
    ImGui::Separator();

    // BBS enable toggle
    bool bbs_enabled = state_.config->bbs.enabled;
    if (ImGui::Checkbox("Enable BBS", &bbs_enabled)) {
        state_.config->bbs.enabled = bbs_enabled;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When enabled, other stations can connect and download files.");
    }

    if (!state_.config->bbs.enabled) {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "BBS is disabled. Enable to share files.");
        return;
    }

    ImGui::Separator();

    // Upload section
    ImGui::Text("Upload File to BBS:");
    ImGui::InputText("File Path", state_.bbs_upload_path, state_.bbs_upload_path_size);
    ImGui::SameLine();
    if (ImGui::Button("Browse##BBS")) {
        if (onOpenFileDialog) {
            onOpenFileDialog([this](const std::string& selected) {
                if (!selected.empty() && state_.bbs_upload_path) {
                    std::strncpy(state_.bbs_upload_path, selected.c_str(), state_.bbs_upload_path_size - 1);
                    state_.bbs_upload_path[state_.bbs_upload_path_size - 1] = '\0';
                }
            });
        }
    }

    ImGui::InputText("Description", state_.bbs_file_desc, state_.bbs_file_desc_size);

    if (ImGui::Button("Upload")) {
        if (std::strlen(state_.bbs_upload_path) > 0) {
            if (onUpload) {
                onUpload(state_.bbs_upload_path, state_.bbs_file_desc);
            }
            state_.bbs_upload_path[0] = '\0';
            state_.bbs_file_desc[0] = '\0';
        }
    }

    // File browser popup for BBS
    if (ImGui::BeginPopupModal("BBSFileBrowser", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static char browse_path[512] = "";
        ImGui::Text("Enter file path:");
        ImGui::InputText("##BBSBrowsePath", browse_path, sizeof(browse_path));
        ImGui::Separator();
        if (ImGui::Button("Select")) {
            std::strncpy(state_.bbs_upload_path, browse_path, state_.bbs_upload_path_size - 1);
            state_.bbs_upload_path[state_.bbs_upload_path_size - 1] = '\0';
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::Separator();
    ImGui::Text("BBS Files:");

    // Refresh button
    if (ImGui::Button("Refresh")) {
        if (onRefreshFiles) {
            onRefreshFiles();
        }
    }

    // File list
    if (ImGui::BeginChild("BBSFileList", ImVec2(-1, 300), true)) {
        ImGui::Columns(5, "BBSColumns");
        ImGui::SetColumnWidth(0, 180);
        ImGui::SetColumnWidth(1, 120);
        ImGui::SetColumnWidth(2, 240);
        ImGui::SetColumnWidth(3, 80);
        ImGui::SetColumnWidth(4, 120);

        ImGui::Text("Filename"); ImGui::NextColumn();
        ImGui::Text("Source"); ImGui::NextColumn();
        ImGui::Text("Description"); ImGui::NextColumn();
        ImGui::Text("Size"); ImGui::NextColumn();
        ImGui::Text("Action"); ImGui::NextColumn();
        ImGui::Separator();

        for (size_t i = 0; i < state_.bbs_files->size(); ++i) {
            const auto& file = (*state_.bbs_files)[i];
            ImGui::Text("%s", file.filename.c_str()); ImGui::NextColumn();
            if (file.remote) {
                ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "%s", file.uploader.c_str());
            } else {
                ImGui::Text("Local");
            }
            ImGui::NextColumn();
            ImGui::Text("%s", file.description.c_str()); ImGui::NextColumn();

            // Format size
            if (file.size_bytes >= 1024) {
                ImGui::Text("%.1f KB", file.size_bytes / 1024.0);
            } else {
                ImGui::Text("%zu B", file.size_bytes);
            }
            ImGui::NextColumn();

            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton(file.remote ? "Fetch" : "Download")) {
                if (onDownload) {
                    onDownload(file);
                }
            }
            if (!file.remote) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Delete")) {
                    if (onDelete) {
                        onDelete(file.filename);
                    }
                }
            }
            ImGui::PopID();
            ImGui::NextColumn();
        }
        ImGui::Columns(1);
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
            } else if (transfer.failed) {
                ImGui::ProgressBar(progress, ImVec2(-1, 0), "Download failed");
                if (!transfer.error.empty()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", transfer.error.c_str());
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "Transfer failed");
                }
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

    // BBS Settings
    ImGui::Separator();
    if (ImGui::CollapsingHeader("BBS Settings")) {
        bool allow_uploads = state_.config->bbs.allow_uploads;
        if (ImGui::Checkbox("Allow Remote Uploads", &allow_uploads)) {
            state_.config->bbs.allow_uploads = allow_uploads;
        }

        static char welcome[256] = "";
        std::strncpy(welcome, state_.config->bbs.welcome_message.c_str(), sizeof(welcome) - 1);
        if (ImGui::InputText("Welcome Message", welcome, sizeof(welcome))) {
            state_.config->bbs.welcome_message = welcome;
        }

        static char bbs_dir[256] = "";
        std::strncpy(bbs_dir, state_.config->bbs.bbs_path.c_str(), sizeof(bbs_dir) - 1);
        if (ImGui::InputText("BBS Directory", bbs_dir, sizeof(bbs_dir))) {
            state_.config->bbs.bbs_path = bbs_dir;
        }
    }
}

} // namespace skylink
