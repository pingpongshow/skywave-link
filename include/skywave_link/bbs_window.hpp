#pragma once
#include "window_base.hpp"
#include <functional>
#include <string>

namespace skylink {

class BBSWindow : public WindowBase {
public:
    explicit BBSWindow(SharedAppState& state);
    void render() override;
    const char* name() const override { return "BBS File Sharing"; }

    std::function<void()> onRefreshFiles;
    std::function<void(const std::string&, const std::string&)> onUpload;
    std::function<void(const BBSFile&)> onDownload;
    std::function<void(const std::string&)> onDelete;
    std::function<std::string()> onOpenFileDialog;
};

} // namespace skylink
