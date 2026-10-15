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
    std::function<void()> onRefreshMessages;
    std::function<void(const std::string&, const std::string&, const std::string&, const std::string&)> onSendEmail;
    std::function<void(int64_t)> onDeleteMessage;

private:
    void renderInbox();
    void renderOutbox();
    void renderEmailTab();
    int selected_outbox_ = -1;
};

} // namespace skylink
