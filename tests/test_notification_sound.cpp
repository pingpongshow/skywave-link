// NotificationSound: setting gate and burst suppression (never plays a
// real sound: the player is replaced).

#include "test_macros.hpp"
#include "skywave_link/notification_sound.hpp"

using namespace skylink;

TEST(notification_sound_gated_and_rate_limited) {
    NotificationSound sound;
    int played = 0;
    sound.setPlayer([&played]() { ++played; return true; });
    const auto t0 = NotificationSound::Clock::time_point{} + std::chrono::hours(1);

    ASSERT_FALSE(sound.notify(false, t0));                       // setting off
    ASSERT_TRUE(sound.notify(true, t0));
    ASSERT_FALSE(sound.notify(true, t0 + std::chrono::milliseconds(500)));   // burst
    ASSERT_TRUE(sound.notify(true, t0 + NotificationSound::kMinInterval));
    ASSERT_EQ(played, 2);

    // A player that fails does not start the quiet interval.
    NotificationSound broken;
    int tries = 0;
    broken.setPlayer([&tries]() { ++tries; return false; });
    ASSERT_FALSE(broken.notify(true, t0));
    ASSERT_FALSE(broken.notify(true, t0 + std::chrono::milliseconds(10)));
    ASSERT_EQ(tries, 2);
    PASS();
}

TEST_MAIN()
