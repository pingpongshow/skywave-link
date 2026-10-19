// MessageWindow - Store-and-forward messages + email tab
#pragma once

#include "window_base.hpp"
#include <cstdint>
#include <functional>
#include <string>

namespace skylink {

class MessageWindow : public WindowBase {
public:
    explicit MessageWindow(SharedAppState& state);

    void render() override;
    const char* name() const override { return "Messages (Store & Forward)"; }

    // Action callbacks
    // Reloads inbox/outbox from the store (two queries): called when the
    // window opens, every few seconds while open, and on Refresh.
    std::function<void()> onRefreshMessages;
    // Queues internet email (to, subject, body, from). True only when the
    // message is stored in the gateway outbox; false with `error` set
    // (bad address, store closed, gateway stopped...).
    std::function<bool(const std::string&, const std::string&, const std::string&,
                       const std::string&, std::string& error)> onQueueEmail;
    std::function<void(int64_t)> onDeleteMessage;

private:
    void renderInbox();
    void renderOutbox();
    void renderEmailTab();
    void refreshIfDue(bool force);
    int selected_outbox_ = -1;
    double last_refresh_ = -1.0;
    int last_render_frame_ = -1;
};

} // namespace skylink
