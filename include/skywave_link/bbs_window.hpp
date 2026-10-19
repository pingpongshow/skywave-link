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
    // Offers the file to the linked station's BBS (K21); false, with the
    // reason in the log, when no transfer started.
    std::function<bool(const std::string&)> onUploadToRemote;
    std::function<void(const BBSFile&)> onDownload;
    std::function<void(const std::string&)> onDelete;
    // Native picker; unset (no picker on this platform) disables Browse.
    PathPicker onOpenFileDialog;

private:
    void renderSettings();
    void renderPathInput();           // file path + Browse
    void renderRemoteUploadButton();  // upload to the linked station's BBS
    // Runs `action` and shows the newest log line it produced (or
    // `fallback`) on the status line.
    template <typename Fn>
    void runAndReport(Fn&& action, const std::string& fallback);
};

} // namespace skylink
