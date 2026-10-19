// WindowBase - Abstract base class for modular GUI windows
#pragma once

#include "shared_state.hpp"
#include <functional>
#include <string>

namespace skylink {

// Native file/folder pickers run without blocking the GUI: the window asks
// for a path and the App calls `done` on the GUI thread once the user has
// chosen (an empty string means cancelled).
using PathPickedCallback = std::function<void(const std::string& path)>;
using PathPicker = std::function<void(PathPickedCallback done)>;

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

protected:
    SharedAppState& state_;
};

} // namespace skylink
