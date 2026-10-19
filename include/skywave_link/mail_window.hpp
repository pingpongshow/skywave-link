#pragma once
#include "window_base.hpp"
#include <cstdint>
#include <functional>
#include <string>

namespace skylink {

class MailWindow : public WindowBase {
public:
    explicit MailWindow(SharedAppState& state);
    void render() override;
    const char* name() const override { return "Mail"; }

    std::function<void(const std::string&, const std::string&, const std::string&)> onSendMail;
    std::function<void(const std::string&, const std::string&)> onSendBroadcastMail;
    std::function<void(const std::string&, bool)> onMarkMailRead;
    std::function<void(int64_t, const std::string&, bool)> onDeleteMail;

    enum class Tab { None, Inbox, Sent, Compose };
    // Brings a tab to the front on the next frame (the main panel MAIL
    // button opens the Inbox).
    void selectTab(Tab tab) { select_tab_ = tab; }
    int unreadCount() const;

private:
    void renderInbox();
    void renderSent();
    void renderCompose();
    Tab select_tab_ = Tab::None;
};

} // namespace skylink
