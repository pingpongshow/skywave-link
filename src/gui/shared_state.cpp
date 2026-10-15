// SharedAppState helper method implementations

#include "skywave_link/shared_state.hpp"
#include <chrono>
#include <algorithm>
#include <cctype>

namespace skylink {

void SharedAppState::addSystemMessage(const std::string& text) {
    if (!chat_history) return;
    ChatMessage msg;
    msg.from = "SYSTEM";
    msg.text = text;
    msg.is_outgoing = false;
    msg.delivered = false;
    msg.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    chat_history->push_back(msg);
    if (chat_scroll_to_bottom) *chat_scroll_to_bottom = true;
}

void SharedAppState::syncTransfers() {
    if (!transfers || !file_transfer_mgr) return;
    transfers->clear();
    auto active = file_transfer_mgr->getActiveTransfers();
    for (const auto& t : active) {
        FileTransfer ft;
        ft.filename = t.filename;
        ft.remote_callsign = t.remote_callsign;
        ft.total_bytes = t.total_bytes;
        ft.transferred_bytes = t.transferred_bytes;
        ft.is_upload = (t.direction == TransferDirection::Upload);
        ft.completed = (t.state == TransferState::Completed);
        ft.failed = (t.state == TransferState::Failed);
        ft.error = t.error_message;
        ft.started_at = t.started_at;
        transfers->push_back(ft);
    }
}

std::string SharedAppState::formatQSODuration() const {
    if (!qso_active || !*qso_active || !qso_start_time) return "--:--:--";
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - *qso_start_time);
    int total_secs = static_cast<int>(elapsed.count());
    int hours = total_secs / 3600;
    int mins = (total_secs % 3600) / 60;
    int secs = total_secs % 60;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hours, mins, secs);
    return buf;
}

bool SharedAppState::isImageFile(const std::string& filename) {
    std::string ext;
    auto dot = filename.rfind('.');
    if (dot != std::string::npos) {
        ext = filename.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".gif" || ext == ".bmp";
}

std::string SharedAppState::getFileTypeDescription(const std::string& filename) {
    std::string ext;
    auto dot = filename.rfind('.');
    if (dot != std::string::npos) {
        ext = filename.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }
    if (ext == ".png") return "PNG Image";
    if (ext == ".jpg" || ext == ".jpeg") return "JPEG Image";
    if (ext == ".gif") return "GIF Image";
    if (ext == ".bmp") return "BMP Image";
    if (ext == ".txt") return "Text File";
    if (ext == ".pdf") return "PDF Document";
    if (ext == ".wav") return "WAV Audio";
    if (ext == ".mp3") return "MP3 Audio";
    if (ext == ".zip") return "ZIP Archive";
    if (ext == ".c2v") return "Voice Message";
    return "File";
}

} // namespace skylink
