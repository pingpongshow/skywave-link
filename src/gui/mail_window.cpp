#include "skywave_link/mail_window.hpp"
#include "skywave_link/gui_util.hpp"
#include "skywave_link/modern_style.hpp"
#include "imgui.h"
#include <cstring>
#include <ctime>
#include <algorithm>

namespace skylink {

namespace {

std::string escapeTemplateField(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (unsigned char c : input) {
        if (c == '\\' || c == '|') {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

const char* mailStatusLabel(MessageStatus status) {
    switch (status) {
        case MessageStatus::Draft: return "Draft";
        case MessageStatus::Pending: return "Pending";
        case MessageStatus::Sent: return "Sent";
        case MessageStatus::Delivered: return "Delivered";
        case MessageStatus::Failed: return "Failed";
        case MessageStatus::Expired: return "Expired";
        default: return "Unknown";
    }
}

const char* mailPriorityLabel(MailPriority priority) {
    switch (priority) {
        case MailPriority::Emergency: return "Emergency";
        case MailPriority::Priority: return "Priority";
        case MailPriority::Routine:
        default: return "Routine";
    }
}

}  // namespace

MailWindow::MailWindow(SharedAppState& state)
    : WindowBase(state) {}

void MailWindow::render() {
    ImGui::Text("Radio Mail");
    ImGui::Separator();
    ui::StatusLine(status_);

    // Compose section
    if (ImGui::CollapsingHeader("Compose Mail", ImGuiTreeNodeFlags_DefaultOpen)) {
        static int template_mode = 0;
        static char ics_to[64] = "";
        static char ics_from[64] = "";
        static char ics_incident[128] = "";
        static char ics_priority[32] = "Routine";
        static char rg_precedence[16] = "R";
        static char rg_handling[32] = "";
        static char rg_origin[64] = "";
        static char rg_check[16] = "";
        static char rg_place[64] = "";
        static char rg_time[32] = "";
        static char rg_date[32] = "";
        static char rg_to[128] = "";

        const char* template_items[] = {"Free Text", "ICS-213", "Radiogram"};
        ImGui::SetNextItemWidth(200);
        ImGui::Combo("Template", &template_mode, template_items, IM_ARRAYSIZE(template_items));

        ImGui::InputText("To", state_.mail_to_input, state_.mail_to_input_size);
        ImGui::SameLine();
        ImGui::TextDisabled("(leave empty for broadcast)");

        ImGui::InputText("Subject", state_.mail_subject_input, state_.mail_subject_input_size);
        if (state_.mail_priority_input) {
            int priority = *state_.mail_priority_input;
            const char* priority_items[] = {"Routine", "Priority", "Emergency"};
            if (ImGui::Combo("Priority", &priority, priority_items, IM_ARRAYSIZE(priority_items))) {
                *state_.mail_priority_input = std::clamp(priority, 0, 2);
            }
        }
        if (template_mode == 1) {
            ImGui::InputText("ICS To", ics_to, sizeof(ics_to));
            ImGui::InputText("ICS From", ics_from, sizeof(ics_from));
            ImGui::InputText("Incident", ics_incident, sizeof(ics_incident));
            ImGui::InputText("Priority", ics_priority, sizeof(ics_priority));
            ImGui::InputTextMultiline("Message", state_.mail_body_input, state_.mail_body_input_size,
                                      ImVec2(ImGui::CalcItemWidth(), ImGui::GetTextLineHeight() * 7.0f));
        } else if (template_mode == 2) {
            ImGui::InputText("Precedence", rg_precedence, sizeof(rg_precedence));
            ImGui::InputText("Handling", rg_handling, sizeof(rg_handling));
            ImGui::InputText("Origin", rg_origin, sizeof(rg_origin));
            ImGui::InputText("Check", rg_check, sizeof(rg_check));
            ImGui::InputText("Place", rg_place, sizeof(rg_place));
            ImGui::InputText("Time", rg_time, sizeof(rg_time));
            ImGui::InputText("Date", rg_date, sizeof(rg_date));
            ImGui::InputText("Radiogram To", rg_to, sizeof(rg_to));
            ImGui::InputTextMultiline("Text", state_.mail_body_input, state_.mail_body_input_size,
                                      ImVec2(ImGui::CalcItemWidth(), ImGui::GetTextLineHeight() * 7.0f));
        } else {
            ImGui::InputTextMultiline("Body", state_.mail_body_input, state_.mail_body_input_size,
                                      ImVec2(ImGui::CalcItemWidth(), ImGui::GetTextLineHeight() * 7.0f));
        }

        if (ImGui::Button("Send Mail")) {
            std::string subject = state_.mail_subject_input;
            std::string body = state_.mail_body_input;

            if (template_mode == 1) {
                if (subject.empty()) {
                    subject = "ICS-213";
                }
                body = "~TPL|I|" + escapeTemplateField(ics_to) + "|" +
                       escapeTemplateField(ics_from) + "|" +
                       escapeTemplateField(ics_incident) + "|" +
                       escapeTemplateField(ics_priority) + "|" +
                       escapeTemplateField(state_.mail_body_input);
            } else if (template_mode == 2) {
                if (subject.empty()) {
                    subject = "Radiogram";
                }
                body = "~TPL|R|" + escapeTemplateField(rg_precedence) + "|" +
                       escapeTemplateField(rg_handling) + "|" +
                       escapeTemplateField(rg_origin) + "|" +
                       escapeTemplateField(rg_check) + "|" +
                       escapeTemplateField(rg_place) + "|" +
                       escapeTemplateField(rg_time) + "|" +
                       escapeTemplateField(rg_date) + "|" +
                       escapeTemplateField(rg_to) + "|" +
                       escapeTemplateField(state_.mail_body_input);
            }

            const std::string to = gui::trimCopy(state_.mail_to_input);
            const bool body_empty = gui::trimCopy(state_.mail_body_input).empty();
            if (template_mode == 0 && body_empty && gui::trimCopy(subject).empty()) {
                postStatus("Nothing to send: write a subject or a message", StatusKind::Error);
            } else {
                if (gui::trimCopy(subject).empty()) {
                    subject = "(no subject)";
                }
                const bool can_send = to.empty() ? static_cast<bool>(onSendBroadcastMail)
                                                 : static_cast<bool>(onSendMail);
                if (!can_send) {
                    postStatus("Mail sending is not available", StatusKind::Error);
                } else {
                    // App appends the mail to the outbox when it is sent or
                    // queued and logs which; nothing appended = refused.
                    const size_t outbox_before = state_.mail_outbox ? state_.mail_outbox->size() : 0;
                    const size_t log_mark = state_.chatHistorySize();
                    if (to.empty()) {
                        onSendBroadcastMail(subject, body);
                    } else {
                        onSendMail(to, subject, body);
                    }
                    const size_t outbox_after = state_.mail_outbox ? state_.mail_outbox->size() : 0;
                    const std::string logged = state_.systemMessageSince(log_mark);
                    if (outbox_after > outbox_before) {
                        postStatus(logged.empty() ? std::string("Mail queued") : logged,
                                   StatusKind::Success);
                        state_.mail_to_input[0] = '\0';
                        state_.mail_subject_input[0] = '\0';
                        state_.mail_body_input[0] = '\0';
                    } else {
                        // Keep the draft so it can be fixed and resent.
                        postStatus(logged.empty() ? std::string("Mail not sent (is the modem "
                                                                "connected?)")
                                                  : logged,
                                   StatusKind::Error);
                    }
                }
            }
        }
    }

    ImGui::Separator();

    // Inbox/Outbox tabs
    if (ImGui::BeginTabBar("MailTabs")) {
        if (ImGui::BeginTabItem("Inbox")) {
            if (ImGui::BeginChild("MailInbox", ImVec2(-1, -1), true)) {
                if (state_.mail_inbox->empty()) {
                    ImGui::TextDisabled("No mail received");
                } else {
                    int delete_inbox_index = -1;
                    for (size_t i = 0; i < state_.mail_inbox->size(); ++i) {
                        auto& mail = (*state_.mail_inbox)[i];
                        std::string label = mail.from + " - " + mail.subject;
                        if (!mail.read) {
                            label = "* " + label;
                        }
                        if (mail.is_broadcast) {
                            label += " [BC]";
                        }
                        if (!mail.template_name.empty()) {
                            label += " [" + mail.template_name + "]";
                        }
                        if (mail.priority != MailPriority::Routine) {
                            label += std::string(" [") + mailPriorityLabel(mail.priority) + "]";
                        }

                        ImGui::PushID(static_cast<int>(i));
                        if (ImGui::Selectable(label.c_str(), *state_.selected_mail == static_cast<int>(i))) {
                            *state_.selected_mail = static_cast<int>(i);
                            (*state_.mail_inbox)[i].read = true;
                            if (onMarkMailRead && !mail.message_id.empty()) {
                                onMarkMailRead(mail.message_id, true);
                            }
                        }

                        if (*state_.selected_mail == static_cast<int>(i)) {
                            ImGui::Indent();
                            const std::string& body = mail.display_body.empty() ? mail.body : mail.display_body;
                            if (!mail.via.empty()) {
                                ImGui::TextColored(ui::palette().warning,
                                                   "Relayed by %s; sender %s is not verified",
                                                   mail.via.c_str(), mail.from.c_str());
                            }
                            ImGui::TextWrapped("%s", body.c_str());
                            if (!mail.message_id.empty()) {
                                ImGui::TextDisabled("MID: %s", mail.message_id.c_str());
                            }

                            auto time_t_val = static_cast<time_t>(mail.timestamp);
                            char time_str[64];
                            std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M",
                                         std::localtime(&time_t_val));
                            ImGui::TextDisabled("Received: %s", time_str);
                            if (ImGui::SmallButton("Delete##InboxMail")) {
                                delete_inbox_index = static_cast<int>(i);
                            }
                            ImGui::Unindent();
                        }
                        ImGui::PopID();
                    }
                    if (delete_inbox_index >= 0 &&
                        delete_inbox_index < static_cast<int>(state_.mail_inbox->size())) {
                        const auto& mail = (*state_.mail_inbox)[delete_inbox_index];
                        const int64_t store_id = mail.store_id;
                        const std::string message_id = mail.message_id;
                        if (onDeleteMail) {
                            onDeleteMail(store_id, message_id, false);
                        }
                        if (*state_.selected_mail == delete_inbox_index) {
                            *state_.selected_mail = -1;
                        } else if (*state_.selected_mail > delete_inbox_index) {
                            --(*state_.selected_mail);
                        }
                    }
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Sent")) {
            if (ImGui::BeginChild("MailOutbox", ImVec2(-1, -1), true)) {
                if (state_.mail_outbox->empty()) {
                    ImGui::TextDisabled("No mail sent");
                } else {
                    int delete_outbox_index = -1;
                    for (size_t i = 0; i < state_.mail_outbox->size(); ++i) {
                        const auto& mail = (*state_.mail_outbox)[i];
                        std::string label = "To: " + mail.to + " - " + mail.subject;
                        if (mail.is_broadcast) {
                            label += " [BC]";
                        }
                        if (!mail.template_name.empty()) {
                            label += " [" + mail.template_name + "]";
                        }
                        if (mail.priority != MailPriority::Routine) {
                            label += std::string(" [") + mailPriorityLabel(mail.priority) + "]";
                        }
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::Text("%s", label.c_str());
                        ImGui::TextDisabled("Status: %s  Retries: %d", mailStatusLabel(mail.status), mail.retry_count);
                        if (!mail.message_id.empty()) {
                            ImGui::TextDisabled("MID: %s", mail.message_id.c_str());
                        }

                        auto time_t_val = static_cast<time_t>(mail.timestamp);
                        char time_str[64];
                        std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M",
                                     std::localtime(&time_t_val));
                        ImGui::TextDisabled("Sent: %s", time_str);
                        if (ImGui::SmallButton("Delete##SentMail")) {
                            delete_outbox_index = static_cast<int>(i);
                        }
                        ImGui::PopID();
                        ImGui::Separator();
                    }
                    if (delete_outbox_index >= 0 &&
                        delete_outbox_index < static_cast<int>(state_.mail_outbox->size())) {
                        const auto& mail = (*state_.mail_outbox)[delete_outbox_index];
                        const int64_t store_id = mail.store_id;
                        const std::string message_id = mail.message_id;
                        if (onDeleteMail) {
                            onDeleteMail(store_id, message_id, true);
                        }
                    }
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

} // namespace skylink
