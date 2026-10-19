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
    // Optional: cancel a running transfer by FileTransfer::transfer_id.
    // Unset, the window cancels through the FileTransferManager directly.
    std::function<void(const std::string&)> onCancelTransfer;
    // Native picker; unset (no picker on this platform) disables Browse.
    PathPicker onOpenFileDialog;

private:
    void sendSelectedFile();
    void cancelTransfer(const FileTransfer& transfer);
};

} // namespace skylink
