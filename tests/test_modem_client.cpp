// ModemClient tests against a fake modem on loopback: command/response
// correlation, session de-framing reset, callback re-entrancy, whole-frame
// sends and multi-address connect.

#include "test_macros.hpp"
#include "skywave_link/modem_client.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

using namespace skylink;

namespace {

int makeListener(uint16_t* port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    *port = ntohs(addr.sin_port);
    return fd;
}

void sendAll(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = send(fd, s.data() + off, s.size() - off, 0);
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

std::string frame(const std::string& msg) {
    std::string f;
    f += static_cast<char>(0xA5);
    f += static_cast<char>(0x5A);
    f += static_cast<char>((msg.size() >> 8) & 0xFF);
    f += static_cast<char>(msg.size() & 0xFF);
    return f + msg;
}

// Fake modem: three listeners; a responder thread answers control commands
// using `replies` (verb -> {delay_ms, line}); unknown verbs echo the verb.
struct FakeModem {
    int lcmd = -1, ldata = -1, lvoice = -1;
    uint16_t pcmd = 0, pdata = 0, pvoice = 0;
    int cmd = -1, data = -1, voice = -1;
    std::thread responder;
    std::atomic<bool> stop{false};
    std::mutex mu;
    std::map<std::string, std::pair<int, std::string>> replies;

    bool listen() {
        lcmd = makeListener(&pcmd);
        ldata = makeListener(&pdata);
        lvoice = makeListener(&pvoice);
        return lcmd >= 0 && ldata >= 0 && lvoice >= 0;
    }
    void acceptAll() {
        cmd = accept(lcmd, nullptr, nullptr);
        data = accept(ldata, nullptr, nullptr);
        voice = accept(lvoice, nullptr, nullptr);
        timeval tv{0, 50000};
        setsockopt(cmd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        responder = std::thread([this]() {
            std::string buf;
            char ch;
            while (!stop) {
                ssize_t n = recv(cmd, &ch, 1, 0);
                if (n == 0) break;
                if (n < 0) continue;
                if (ch != '\n') { buf += ch; continue; }
                std::string verb = buf.substr(0, buf.find(' '));
                buf.clear();
                std::pair<int, std::string> r{0, verb};
                {
                    std::lock_guard<std::mutex> lock(mu);
                    auto it = replies.find(verb);
                    if (it != replies.end()) r = it->second;
                }
                if (r.first > 0) std::this_thread::sleep_for(std::chrono::milliseconds(r.first));
                sendAll(cmd, r.second + "\r");
            }
        });
    }
    void reply(const std::string& verb, int delay_ms, const std::string& line) {
        std::lock_guard<std::mutex> lock(mu);
        replies[verb] = {delay_ms, line};
    }
    void event(const std::string& line) { sendAll(cmd, line + "\r"); }
    ~FakeModem() {
        stop = true;
        if (responder.joinable()) responder.join();
        for (int fd : {cmd, data, voice, lcmd, ldata, lvoice}) if (fd >= 0) close(fd);
    }
};

void pump(ModemClient& c, int ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        c.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace

TEST(stale_responses_do_not_shift_results) {
    FakeModem m;
    ASSERT_TRUE(m.listen());
    ModemClient c;
    ASSERT_TRUE(c.connect("127.0.0.1", m.pcmd, m.pdata, m.pvoice));
    m.acceptAll();
    m.reply("VERSION", 0, "VERSION TEST-1.0");
    m.reply("STATUS", 0, "STATUS STATE=LISTENING CALL=N0CALL REMOTE=-");
    m.reply("NOISEFLOOR", 700, "NOISEFLOOR -90.0 BUSY=0");  // answers too late

    // Fire-and-forget command whose answer nobody reads.
    ASSERT_TRUE(c.sendCommand("STATUS"));
    pump(c, 100);
    ASSERT_EQ(c.getVersion(), std::string("TEST-1.0"));

    // A command that times out; its late answer must not be taken as the
    // next command's result.
    ASSERT_FALSE(c.getNoiseFloor().has_value());
    pump(c, 400);  // late NOISEFLOOR arrives now
    ASSERT_EQ(c.getVersion(), std::string("TEST-1.0"));
    PASS();
}

TEST(session_end_resets_deframer) {
    FakeModem m;
    ASSERT_TRUE(m.listen());
    ModemClient c;
    std::vector<std::string> got;
    c.onDataReceived([&](const std::vector<uint8_t>& d) { got.emplace_back(d.begin(), d.end()); });
    ASSERT_TRUE(c.connect("127.0.0.1", m.pcmd, m.pdata, m.pvoice));
    m.acceptAll();

    m.event("EVT:CONNECTED W1AW WIDE M8");
    pump(c, 100);
    // Partial frame (header says 100 bytes, only 5 arrive), then session ends.
    std::string partial = frame(std::string(100, 'x')).substr(0, 9);
    sendAll(m.data, partial);
    pump(c, 100);
    m.event("EVT:DISCONNECTED TIMEOUT");
    pump(c, 100);

    // Unconnected payload must be delivered as-is, not swallowed.
    sendAll(m.data, "hello broadcast");
    pump(c, 100);
    ASSERT_EQ(got.size(), size_t(1));
    ASSERT_EQ(got[0], std::string("hello broadcast"));
    PASS();
}

TEST(data_callback_reentrancy) {
    FakeModem m;
    ASSERT_TRUE(m.listen());
    ModemClient c;
    std::vector<std::string> got;
    std::string versions;
    c.onDataReceived([&](const std::vector<uint8_t>& d) {
        got.emplace_back(d.begin(), d.end());
        // Re-enters poll() through getResponse() while frames are pending.
        versions += c.getVersion() + ";";
    });
    ASSERT_TRUE(c.connect("127.0.0.1", m.pcmd, m.pdata, m.pvoice));
    m.acceptAll();
    m.reply("VERSION", 0, "VERSION V");
    m.event("EVT:CONNECTED W1AW WIDE M8");
    pump(c, 100);

    std::string burst;
    for (int i = 0; i < 5; ++i) burst += frame("msg" + std::to_string(i));
    sendAll(m.data, burst);
    sendAll(m.data, frame("tail1"));
    pump(c, 300);
    sendAll(m.data, frame("tail2"));
    pump(c, 300);

    ASSERT_EQ(got.size(), size_t(7));
    for (int i = 0; i < 5; ++i) ASSERT_EQ(got[i], "msg" + std::to_string(i));
    ASSERT_EQ(got[5], std::string("tail1"));
    ASSERT_EQ(got[6], std::string("tail2"));
    ASSERT_EQ(versions, std::string("V;V;V;V;V;V;V;"));
    PASS();
}

TEST(send_data_never_truncates_frames) {
    FakeModem m;
    ASSERT_TRUE(m.listen());
    ModemClient c;
    ASSERT_TRUE(c.connect("127.0.0.1", m.pcmd, m.pdata, m.pvoice));
    m.acceptAll();
    m.event("EVT:CONNECTED W1AW WIDE M8");
    pump(c, 100);

    // The fake modem does not read the data port yet, so the socket fills.
    const size_t kMsg = 60000;
    const int kCount = 40;  // ~2.4 MB, well past socket buffers
    for (int i = 0; i < kCount; ++i) {
        std::vector<uint8_t> msg(kMsg, static_cast<uint8_t>('A' + (i % 26)));
        ASSERT_TRUE(c.sendData(msg));
    }
    ASSERT_GT(c.pendingDataBytes(), size_t(0));

    // Now drain on the modem side while the client keeps polling.
    std::string rx;
    std::atomic<bool> done{false};
    std::thread reader([&]() {
        timeval tv{0, 100000};
        setsockopt(m.data, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char buf[65536];
        const size_t want = kCount * (kMsg + 4);
        while (rx.size() < want) {
            ssize_t n = recv(m.data, buf, sizeof(buf), 0);
            if (n > 0) rx.append(buf, static_cast<size_t>(n));
            else if (n == 0) break;
        }
        done = true;
    });
    for (int i = 0; i < 1000 && !done; ++i) {
        c.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    reader.join();
    ASSERT_EQ(c.pendingDataBytes(), size_t(0));
    ASSERT_EQ(rx.size(), kCount * (kMsg + 4));
    size_t pos = 0;
    for (int i = 0; i < kCount; ++i) {
        ASSERT_EQ(static_cast<uint8_t>(rx[pos]), uint8_t(0xA5));
        ASSERT_EQ(static_cast<uint8_t>(rx[pos + 1]), uint8_t(0x5A));
        size_t len = (static_cast<uint8_t>(rx[pos + 2]) << 8) | static_cast<uint8_t>(rx[pos + 3]);
        ASSERT_EQ(len, kMsg);
        ASSERT_EQ(rx[pos + 4], static_cast<char>('A' + (i % 26)));
        ASSERT_EQ(rx[pos + 4 + kMsg - 1], static_cast<char>('A' + (i % 26)));
        pos += 4 + len;
    }
    PASS();
}

TEST(connect_tries_all_addresses) {
    // Listeners are IPv4-only; "localhost" commonly resolves to ::1 first.
    FakeModem m;
    ASSERT_TRUE(m.listen());
    ModemClient c;
    ASSERT_TRUE(c.connect("localhost", m.pcmd, m.pdata, m.pvoice));
    m.acceptAll();
    ASSERT_TRUE(c.isConnected());
    PASS();
}
#endif

TEST_MAIN()
