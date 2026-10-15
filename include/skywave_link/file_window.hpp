// FileWindow - File transfer UI
#pragma once

#include "window_base.hpp"
#include <functional>
#include <string>

namespace skylink {

class FileWindow : public WindowBase {
public:
    explicit FileWindow(SharedAppState& state);

    void render() override;
    const char* name() const override { return "File Transfer"; }

    // Action callbacks
    std::function<void(const std::string&)> onSendFile;
    std::function<std::string()> onOpenFileDialog;
};

} // namespace skylink
