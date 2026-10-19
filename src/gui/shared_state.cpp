// SharedAppState helper method implementations

#include "skywave_link/shared_state.hpp"
#include "skywave_link/gui_util.hpp"

#include <chrono>

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

std::string SharedAppState::formatQSODuration() const {
    if (!qso_active || !*qso_active || !qso_start_time) return "--:--:--";
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - *qso_start_time);
    return gui::formatDuration(elapsed.count());
}

} // namespace skylink
