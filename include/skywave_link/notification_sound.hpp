// Notification sound for incoming chat/mail (ui.sound_notifications).
//
// Plays the platform's system alert sound without blocking the caller:
// macOS runs `afplay` on a system sound in a child process, Windows uses
// MessageBeep, other systems try `canberra-gtk-play`. Repeats within
// kMinInterval are dropped so a burst of messages makes one sound.

#pragma once

#include <chrono>
#include <functional>

namespace skylink {

class NotificationSound {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kMinInterval{2000};

    NotificationSound();

    // Plays the sound unless one played less than kMinInterval ago, or the
    // setting is off. Returns whether a sound was started.
    bool notify(bool enabled, Clock::time_point now = Clock::now());

    // Replaces the platform player (tests; returns whether it played).
    void setPlayer(std::function<bool()> player) { player_ = std::move(player); }

    // The platform player itself: starts the system sound, never blocks.
    static bool playSystemSound();

private:
    std::function<bool()> player_;
    bool played_ = false;
    Clock::time_point last_{};
};

} // namespace skylink
