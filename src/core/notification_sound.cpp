// Notification sound - non-blocking system alert sound.

#include "skywave_link/notification_sound.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>
extern char** environ;
#endif

namespace skylink {

NotificationSound::NotificationSound() : player_(&NotificationSound::playSystemSound) {}

bool NotificationSound::notify(bool enabled, Clock::time_point now) {
    if (!enabled || !player_) return false;
    if (played_ && now - last_ < kMinInterval) return false;
    if (!player_()) return false;
    played_ = true;
    last_ = now;
    return true;
}

bool NotificationSound::playSystemSound() {
#ifdef _WIN32
    return MessageBeep(MB_ICONINFORMATION) != 0;
#else
#if defined(__APPLE__)
    const char* program = "/usr/bin/afplay";
    const char* sound = "/System/Library/Sounds/Glass.aiff";
    if (access(program, X_OK) != 0 || access(sound, R_OK) != 0) return false;
    char* const argv[] = {const_cast<char*>(program), const_cast<char*>(sound), nullptr};
#else
    const char* program = "canberra-gtk-play";
    char* const argv[] = {const_cast<char*>(program), const_cast<char*>("-i"),
                          const_cast<char*>("message-new-instant"), nullptr};
#endif
    pid_t pid = 0;
#if defined(__APPLE__)
    const int rc = posix_spawn(&pid, program, nullptr, nullptr, argv, environ);
#else
    const int rc = posix_spawnp(&pid, program, nullptr, nullptr, argv, environ);
#endif
    if (rc != 0) return false;
    // Reap the player when it ends so it never lingers as a zombie.
    try {
        std::thread([pid]() {
            int status = 0;
            waitpid(pid, &status, 0);
        }).detach();
    } catch (...) {
    }
    return true;
#endif
}

} // namespace skylink
