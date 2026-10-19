// MessageWindow - Store-and-forward messages + email tab

#include "skywave_link/message_window.hpp"
#include "skywave_link/gui_util.hpp"
#include "skywave_link/message_store.hpp"
#include "skywave_link/modern_style.hpp"
#include "imgui.h"
#include <cstring>
#include <chrono>

namespace skylink {

namespace {

constexpr double kRefreshIntervalSec = 2.0;

// Same rule as the App's gateway check: one '@', something on both sides,
// a dot in the domain, no whitespace.
bool plausibleEmailAddress(const std::string& input) {
    const std::string addr = gui::trimCopy(input);
    const size_t at = addr.find('@');
    if (at == std::string::npos || at == 0 || at + 1 >= addr.size()) return false;
    if (addr.find('@', at + 1) != std::string::npos) return false;
    if (addr.find_first_of(" \t\r\n") != std::string::npos) return false;
    const size_t dot = addr.find('.', at + 1);
    return dot != std::string::npos && dot + 1 < addr.size() && dot > at + 1;
}

}  // namespace

MessageWindow::MessageWindow(SharedAppState& state) : WindowBase(state) {}

// The lists come from SQLite; reloading them every frame cost two queries
// per frame. Reload when the window (re)opens, every couple of seconds,
// and on demand.
void MessageWindow::refreshIfDue(bool force) {
    const int frame = ImGui::GetFrameCount();
    const double now = ImGui::GetTime();
    const bool reopened = last_render_frame_ < 0 || frame - last_render_frame_ > 1;
    last_render_frame_ = frame;
    if (!onRefreshMessages) return;
    if (force || reopened || last_refresh_ < 0.0 || now - last_refresh_ >= kRefreshIntervalSec) {
        onRefreshMessages();
        last_refresh_ = now;
    }
}

void MessageWindow::render() {
    refreshIfDue(false);
    ui::StatusLine(status_);
    if (ImGui::SmallButton("Refresh")) {
        refreshIfDue(true);
    }
    if (ImGui::BeginTabBar("MessageTabs")) {
        if (ImGui::BeginTabItem("Inbox")) {
            renderInbox();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Outbox")) {
            renderOutbox();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Email Gateway")) {
            renderEmailTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void MessageWindow::renderInbox() {
    if (!state_.inbox_messages || !state_.selected_message) return;
    auto& inbox = *state_.inbox_messages;
    auto& selected = *state_.selected_message;

    // Split view: message list on left, content on right
    ImGui::Columns(2, "MessageColumns", true);

    ImGui::Text("Inbox");
    if (ImGui::BeginChild("MessageList", ImVec2(-1, -1), ImGuiChildFlags_Borders)) {
        if (selected >= static_cast<int>(inbox.size())) {
            selected = static_cast<int>(inbox.size()) - 1;
        }
        if (inbox.empty()) {
            ImGui::TextDisabled("No messages");
        }

        for (size_t i = 0; i < inbox.size(); ++i) {
            const auto& msg = inbox[i];
            bool sel = (selected == static_cast<int>(i));

            std::string label = msg.from_callsign + ": " + msg.subject;
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(label.c_str(), sel)) {
                selected = static_cast<int>(i);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::NextColumn();

    ImGui::Text("Message Content");
    if (ImGui::BeginChild("MessageContent", ImVec2(-1, -1), ImGuiChildFlags_Borders)) {
        if (selected >= 0 && selected < static_cast<int>(inbox.size())) {
            const auto& msg = inbox[selected];
            ImGui::Text("From: %s", msg.from_callsign.c_str());
            ImGui::Text("To: %s", msg.to_callsign.c_str());
            ImGui::Text("Subject: %s", msg.subject.c_str());
            ImGui::Separator();
            ImGui::TextWrapped("%s", msg.body.c_str());
            if (msg.id > 0 && ImGui::Button("Delete Message")) {
                const int64_t id_to_delete = msg.id;
                if (onDeleteMessage) {
                    onDeleteMessage(id_to_delete);
                    postStatus("Message deleted", StatusKind::Info);
                }
                selected = -1;
            }
        } else {
            ImGui::TextDisabled("Select a message to view");
        }
    }
    ImGui::EndChild();

    ImGui::Columns(1);
}

void MessageWindow::renderOutbox() {
    if (!state_.outbox_messages) return;
    auto& outbox = *state_.outbox_messages;

    ImGui::Columns(2, "MessageOutboxColumns", true);

    ImGui::Text("Outbox");
    if (ImGui::BeginChild("OutboxList", ImVec2(-1, -1), ImGuiChildFlags_Borders)) {
        if (selected_outbox_ >= static_cast<int>(outbox.size())) {
            selected_outbox_ = static_cast<int>(outbox.size()) - 1;
        }
        if (outbox.empty()) {
            ImGui::TextDisabled("No messages");
        }

        for (size_t i = 0; i < outbox.size(); ++i) {
            const auto& msg = outbox[i];
            bool sel = (selected_outbox_ == static_cast<int>(i));

            std::string label = msg.to_callsign + ": " + msg.subject;
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(label.c_str(), sel)) {
                selected_outbox_ = static_cast<int>(i);
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::NextColumn();

    ImGui::Text("Sent Message");
    if (ImGui::BeginChild("OutboxContent", ImVec2(-1, -1), ImGuiChildFlags_Borders)) {
        if (selected_outbox_ >= 0 && selected_outbox_ < static_cast<int>(outbox.size())) {
            const auto& msg = outbox[selected_outbox_];
            ImGui::Text("From: %s", msg.from_callsign.c_str());
            ImGui::Text("To: %s", msg.to_callsign.c_str());
            ImGui::Text("Subject: %s", msg.subject.c_str());
            ImGui::Separator();
            ImGui::TextWrapped("%s", msg.body.c_str());
            if (msg.id > 0 && ImGui::Button("Delete Sent Message")) {
                const int64_t id_to_delete = msg.id;
                if (onDeleteMessage) {
                    onDeleteMessage(id_to_delete);
                    postStatus("Message deleted", StatusKind::Info);
                }
                selected_outbox_ = -1;
            }
        } else {
            ImGui::TextDisabled("Select a sent message to view");
        }
    }
    ImGui::EndChild();

    ImGui::Columns(1);
}

void MessageWindow::renderEmailTab() {
    ImGui::Text("Email Gateway");
    ImGui::Separator();

    static char to_addr[256] = "";
    static char subject[256] = "";
    static char body[4096] = "";

    ImGui::InputText("To", to_addr, sizeof(to_addr));
    ImGui::InputText("Subject", subject, sizeof(subject));
    ImGui::InputTextMultiline("Body", body, sizeof(body),
                              ImVec2(ImGui::CalcItemWidth(), ImGui::GetTextLineHeight() * 13.0f));

    if (ImGui::Button("Send Email")) {
        const std::string to = gui::trimCopy(to_addr);
        const std::string from = state_.config ? state_.config->station.callsign : "";
        if (to.empty() || gui::trimCopy(subject).empty()) {
            postStatus("Fill in To and Subject", StatusKind::Error);
        } else if (!plausibleEmailAddress(to)) {
            postStatus("\"" + to + "\" is not an email address", StatusKind::Error);
        } else if (from.empty()) {
            postStatus("Set your callsign in Settings first", StatusKind::Error);
        } else if (onQueueEmail) {
            std::string error;
            if (onQueueEmail(to, subject, body, from, error)) {
                postStatus("Email to " + to + " queued for the internet gateway",
                           StatusKind::Success);
                to_addr[0] = '\0';
                subject[0] = '\0';
                body[0] = '\0';
            } else {
                // Keep the draft.
                postStatus("Email not queued: " + (error.empty() ? std::string("unknown error")
                                                                 : error),
                           StatusKind::Error);
            }
        } else {
            postStatus("Email gateway is not available", StatusKind::Error);
        }
    }
}

} // namespace skylink
