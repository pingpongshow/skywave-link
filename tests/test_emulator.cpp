// Link emulator end to end (K24): two ModemClients through the real
// emulator binary - BCAST answers and EVT:BROADCAST delivery, BEACON/CQ MON
// lines with modem-layout frames, NO_ANSWER for an absent station, session
// data only inside a session, and the placeholder callsign refused.

#include "test_macros.hpp"
#include "skywave_link/modem_client.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

using namespace skylink;

namespace {

uint16_t freePort() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    close(fd);
    return ntohs(addr.sin_port);
}

struct Emulator {
    pid_t pid = -1;
    uint16_t cmd = 0, data = 0;
    bool start() {
        cmd = freePort();
        data = freePort();
        const std::string c = std::to_string(cmd), d = std::to_string(data);
        const char* path = SKYWAVE_LINK_EMULATOR_PATH;
        char* const argv[] = {const_cast<char*>(path), const_cast<char*>("--host"),
                              const_cast<char*>("127.0.0.1"), const_cast<char*>("--cmd-port"),
                              const_cast<char*>(c.c_str()), const_cast<char*>("--data-port"),
                              const_cast<char*>(d.c_str()), nullptr};
        if (posix_spawn(&pid, path, nullptr, nullptr, argv, environ) != 0) return false;
        // Wait until it listens.
        for (int i = 0; i < 100; ++i) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(data);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            const bool up = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
            close(fd);
            if (up) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }
    ~Emulator() {
        if (pid > 0) {
            kill(pid, SIGTERM);
            int status = 0;
            waitpid(pid, &status, 0);
        }
    }
};

void pumpBoth(ModemClient& a, ModemClient& b, int ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        a.poll();
        b.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// A port nothing listens on, so the optional voice socket is absent.
uint16_t closedPort() { return freePort(); }

} // namespace

TEST(emulator_broadcast_monitor_and_session) {
    Emulator emu;
    ASSERT_TRUE(emu.start());
    ModemClient a, b;
    ASSERT_TRUE(a.connect("127.0.0.1", emu.cmd, emu.data, closedPort()));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // pair A's data socket first
    ASSERT_TRUE(b.connect("127.0.0.1", emu.cmd, emu.data, closedPort()));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // The placeholder callsign transmits nothing.
    bool refused = false;
    a.sendBroadcastData({0x01}, [&](bool ok, const std::string& r) {
        refused = !ok && r == "ERROR:STATE";
    });
    ASSERT_TRUE(refused);

    ASSERT_TRUE(a.setCallsign("K1AAA"));
    ASSERT_TRUE(b.setCallsign("K1BBB"));

    std::vector<std::pair<std::string, std::vector<uint8_t>>> got;
    b.onBroadcast([&](const std::string& src, const std::vector<uint8_t>& p) { got.emplace_back(src, p); });
    std::vector<MonitorFrame> heard;
    b.onMonitorFrame([&](const MonitorFrame& f) { heard.push_back(f); });
    std::vector<std::vector<uint8_t>> data_b;
    b.onDataReceived([&](const std::vector<uint8_t>& d) { data_b.push_back(d); });

    std::string answer;
    ASSERT_TRUE(a.sendBroadcastData({0xDE, 0xAD, 0xBE}, [&](bool ok, const std::string& r) {
        if (ok) answer = r;
    }));
    ASSERT_EQ(answer, std::string("BCAST 3"));  // the modem answers with the byte count
    pumpBoth(a, b, 150);
    ASSERT_EQ(got.size(), size_t(1));
    ASSERT_EQ(got[0].first, std::string("K1AAA"));
    ASSERT_TRUE(got[0].second == std::vector<uint8_t>({0xDE, 0xAD, 0xBE}));  // no 0xFA wrapping

    // The app's station beacon (0xFA | grid_len | grid | flags | "CALL text",
    // no SNR byte) travels as an opaque BCAST payload, byte for byte.
    const std::vector<uint8_t> app_beacon = {0xFA, 0x04, 'F', 'N', '4', '2', 0x00,
                                             'K', '1', 'A', 'A', 'A'};
    ASSERT_TRUE(a.sendBroadcastData(app_beacon));
    pumpBoth(a, b, 150);
    ASSERT_EQ(got.size(), size_t(2));
    ASSERT_TRUE(got[1].second == app_beacon);

    ASSERT_TRUE(a.sendBeacon());
    ASSERT_TRUE(a.sendCq());
    pumpBoth(a, b, 150);
    ASSERT_EQ(heard.size(), size_t(2));
    ASSERT_EQ(heard[0].frame_type, std::string("BEACON"));
    ASSERT_EQ(heard[0].src_callsign, std::string("K1AAA"));
    ASSERT_FALSE(heard[0].truncated);  // CRC checks
    ASSERT_EQ(heard[1].frame_type, std::string("CQ"));
    ASSERT_EQ(heard[1].src_callsign, std::string("K1AAA"));

    // Data outside a session goes nowhere.
    std::vector<std::string> reasons;
    a.onDisconnected([&](const std::string& r) { reasons.push_back(r); });
    ASSERT_TRUE(a.connectToStation("K1ZZZ"));
    pumpBoth(a, b, 150);
    ASSERT_EQ(reasons.size(), size_t(1));
    ASSERT_EQ(reasons[0], std::string("NO_ANSWER"));

    ASSERT_TRUE(a.connectToStation("K1BBB"));
    pumpBoth(a, b, 150);
    ASSERT_TRUE(a.getStatus().link_state == LinkState::Connected);
    ASSERT_TRUE(b.getStatus().link_state == LinkState::Connected);
    // App chat framing: 0xC0 | UTF-8 text, delivered unchanged.
    const std::vector<uint8_t> chat = {0xC0, 'h', 'e', 'l', 'l', 'o', ' ', 0xE2, 0x82, 0xAC};
    ASSERT_TRUE(a.sendData(chat));
    pumpBoth(a, b, 150);
    ASSERT_EQ(data_b.size(), size_t(1));
    ASSERT_TRUE(data_b[0] == chat);
    // Broadcasts keep arriving during a session.
    ASSERT_TRUE(a.sendBroadcastData({0x42}));
    pumpBoth(a, b, 150);
    ASSERT_EQ(got.size(), size_t(3));
    ASSERT_TRUE(a.disconnectStation());
    pumpBoth(a, b, 150);
    ASSERT_TRUE(b.getStatus().link_state == LinkState::Idle);
    PASS();
}
#endif

TEST_MAIN()
