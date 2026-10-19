// WindowBase - Abstract base class for modular GUI windows
#pragma once

#include "shared_state.hpp"
#include <chrono>
#include <functional>
#include <string>

namespace skylink {

// Native file/folder pickers run without blocking the GUI: the window asks
// for a path and the App calls `done` on the GUI thread once the user has
// chosen (an empty string means cancelled).
using PathPickedCallback = std::function<void(const std::string& path)>;
using PathPicker = std::function<void(PathPickedCallback done)>;

// One-line feedback shown inside a window (the result of the last action
// started there). The main Log panel only keeps 8 lines, so errors and
// results also land in the window that triggered them.
enum class StatusKind { Info, Success, Warning, Error };

struct WindowStatus {
    std::string text;
    StatusKind kind = StatusKind::Info;
    std::chrono::steady_clock::time_point posted_at{};
    bool empty() const { return text.empty(); }
};

class WindowBase {
public:
    explicit WindowBase(SharedAppState& state) : state_(state) {}
    virtual ~WindowBase() = default;

    // Non-copyable
    WindowBase(const WindowBase&) = delete;
    WindowBase& operator=(const WindowBase&) = delete;

    // Render the window content (called every frame when visible)
    virtual void render() = 0;

    // Update logic (called every frame regardless of visibility)
    virtual void update() {}

    // Window name for ImGui
    virtual const char* name() const = 0;

    // Status line: App may post results of asynchronous work (modem
    // command answers, send results) here too. Rendered by
    // ui::StatusLine() at the top of the window.
    void postStatus(std::string text, StatusKind kind = StatusKind::Info) {
        status_.text = std::move(text);
        status_.kind = kind;
        status_.posted_at = std::chrono::steady_clock::now();
    }
    void clearStatus() { status_ = WindowStatus{}; }
    const WindowStatus& status() const { return status_; }

protected:
    SharedAppState& state_;
    WindowStatus status_;
};

} // namespace skylink
