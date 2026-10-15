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
};

} // namespace skylink
