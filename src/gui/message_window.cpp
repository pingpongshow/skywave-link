// MessageWindow - Store-and-forward messages + email tab

#include "skywave_link/message_window.hpp"
#include "skywave_link/message_store.hpp"
#include "imgui.h"
#include <cstring>
#include <chrono>

namespace skylink {

MessageWindow::MessageWindow(SharedAppState& state) : WindowBase(state) {}

void MessageWindow::render() {
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
    if (ImGui::BeginChild("MessageList", ImVec2(-1, -1), true)) {
        if (onRefreshMessages) onRefreshMessages();
        if (selected >= static_cast<int>(inbox.size())) {
            selected = static_cast<int>(inbox.size()) - 1;
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
    if (ImGui::BeginChild("MessageContent", ImVec2(-1, -1), true)) {
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
                }
                selected = -1;
            }
        } else {
            ImGui::Text("Select a message to view");
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
    if (ImGui::BeginChild("OutboxList", ImVec2(-1, -1), true)) {
        if (onRefreshMessages) onRefreshMessages();
        if (selected_outbox_ >= static_cast<int>(outbox.size())) {
            selected_outbox_ = static_cast<int>(outbox.size()) - 1;
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
    if (ImGui::BeginChild("OutboxContent", ImVec2(-1, -1), true)) {
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
                }
                selected_outbox_ = -1;
            }
        } else {
            ImGui::Text("Select a sent message to view");
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
        if (std::strlen(to_addr) > 0 && std::strlen(subject) > 0) {
            if (onSendEmail) {
                onSendEmail(to_addr, subject, body,
                            state_.config ? state_.config->station.callsign : "");
            }
            to_addr[0] = '\0';
            subject[0] = '\0';
            body[0] = '\0';
            ImGui::OpenPopup("Email Queued");
        } else {
            ImGui::OpenPopup("Email Error");
        }
    }

    if (ImGui::BeginPopupModal("Email Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Please fill in To and Subject fields.");
        if (ImGui::Button("OK")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Email Queued", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Email has been queued for delivery.");
        if (ImGui::Button("OK")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace skylink
