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
    // Send until the client is actually holding bytes back: macOS loopback
    // auto-tunes its buffers, and a fixed 2.4 MB sometimes fit entirely.
    const size_t kMsg = 60000;
    int kCount = 0;
    while (kCount < 40 || (c.pendingDataBytes() == 0 && kCount < 400)) {
        std::vector<uint8_t> msg(kMsg, static_cast<uint8_t>('A' + (kCount % 26)));
        ASSERT_TRUE(c.sendData(msg));
        ++kCount;
    }
    ASSERT_GT(c.pendingDataBytes(), size_t(0));

    // Now drain on the modem side while the client keeps polling.
    std::string rx;
    std::atomic<bool> done{false};
    std::thread reader([&]() {
        timeval tv{0, 100000};
        setsockopt(m.data, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char buf[65536];
        const size_t want = size_t(kCount) * (kMsg + 4);
        while (rx.size() < want) {
            ssize_t n = recv(m.data, buf, sizeof(buf), 0);
            if (n > 0) rx.append(buf, static_cast<size_t>(n));
            else if (n == 0) break;
        }
        done = true;
    });
    for (int i = 0; i < 6000 && !done; ++i) {  // <= 30 s
        c.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    reader.join();
    ASSERT_EQ(c.pendingDataBytes(), size_t(0));
    ASSERT_EQ(rx.size(), size_t(kCount) * (kMsg + 4));
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
// Voice media on the voice port (INTERFACES.md records): nothing tested
// the app's half of it. Uplink PCM must leave as framed uplink records;
// downlink records - split across socket reads - must come back as PCM;
// a wrong-direction record is ignored.
TEST(voice_port_records_both_ways) {
    FakeModem m;
    ASSERT_TRUE(m.listen());
    ModemClient c;
    ASSERT_TRUE(c.connect("127.0.0.1", m.pcmd, m.pdata, m.pvoice));
    m.acceptAll();
    pump(c, 50);

    std::vector<int16_t> up(320);
    for (size_t i = 0; i < up.size(); ++i) up[i] = static_cast<int16_t>(int(i) * 97 - 15000);
    ASSERT_TRUE(c.sendVoicePcm(up.data(), up.size()));
    ASSERT_TRUE(c.sendVoicePcm(up.data(), 160));
    std::vector<uint8_t> rx;
    timeval tv{0, 200000};
    setsockopt(m.voice, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    for (int i = 0; i < 20 && rx.size() < 16 + 640 + 16 + 320; ++i) {
        uint8_t b[4096];
        ssize_t n = recv(m.voice, b, sizeof(b), 0);
        if (n > 0) rx.insert(rx.end(), b, b + n);
    }
    ASSERT_EQ(rx.size(), size_t(16 + 640 + 16 + 320));
    ASSERT_EQ(rx[0], uint8_t(1));                       // version
    ASSERT_EQ(rx[1], uint8_t(1));                       // uplink
    ASSERT_EQ((size_t(rx[2]) << 8) | rx[3], size_t(640));
    ASSERT_EQ(uint32_t(rx[4] << 24 | rx[5] << 16 | rx[6] << 8 | rx[7]), uint32_t(0));
    for (size_t i = 0; i < up.size(); ++i) {
        const int16_t v = int16_t(uint16_t(rx[16 + 2 * i]) | uint16_t(rx[17 + 2 * i]) << 8);
        ASSERT_EQ(v, up[i]);                            // PCM16LE
    }
    const size_t r2 = 16 + 640;
    ASSERT_EQ(uint32_t(rx[r2 + 7]), uint32_t(1));       // sequence advances
    ASSERT_EQ(uint32_t(rx[r2 + 14]), uint32_t(0x01));   // sample counter = 320
    ASSERT_EQ(uint32_t(rx[r2 + 15]), uint32_t(0x40));

    // Downlink: one record split across two sends, plus an uplink-typed
    // record from the modem, which is not playable.
    auto rec = [](uint8_t type, uint32_t seq, const std::vector<int16_t>& pcm) {
        std::string out;
        out += char(1); out += char(type);
        const size_t n = pcm.size() * 2;
        out += char(n >> 8); out += char(n & 0xFF);
        for (int k = 3; k >= 0; --k) out += char((seq >> (8 * k)) & 0xFF);
        for (int k = 0; k < 8; ++k) out += char(0);
        for (int16_t v : pcm) { out += char(uint16_t(v) & 0xFF); out += char(uint16_t(v) >> 8); }
        return out;
    };
    std::vector<int16_t> down(480);
    for (size_t i = 0; i < down.size(); ++i) down[i] = static_cast<int16_t>(int(i) * 31 - 7000);
    const std::string d = rec(2, 7, down) + rec(1, 8, {1, 2, 3});
    sendAll(m.voice, d.substr(0, 101));
    pump(c, 60);
    sendAll(m.voice, d.substr(101));
    pump(c, 120);
    auto got = c.receiveVoicePcm(4000);
    ASSERT_EQ(got.size(), down.size());
    ASSERT_TRUE(got == down);
    PASS();
}

// VOICESTATUS: the fields modem v1.2.7 adds (broadcast playout, unit
// length, real-time flag) reach the app, and reset when absent.
TEST(voice_status_fields_parse) {
    FakeModem m;
    ASSERT_TRUE(m.listen());
    ModemClient c;
    ASSERT_TRUE(c.connect("127.0.0.1", m.pcmd, m.pdata, m.pvoice));
    m.acceptAll();
    m.reply("VOICESTATUS", 0,
            "VOICESTATUS MODE=BROADCAST CODEC=AUTO(1300) STATE=OPEN PTT=OFF GRANTED=OFF "
            "TXQ=0 RXQ=0 TRANSPORT=M9 BCASTRX=ON UNIT=3.0s RT=NO");
    auto v = c.queryVoiceStatus();
    ASSERT_TRUE(v.has_value());
    ASSERT_TRUE(v->mode == VoiceMode::Broadcast);
    ASSERT_TRUE(v->broadcast_rx);
    ASSERT_FALSE(v->real_time);
    ASSERT_TRUE(v->unit_sec > 2.9 && v->unit_sec < 3.1);
    ASSERT_EQ(v->transport_mode, 9);
    m.reply("VOICESTATUS", 0,
            "VOICESTATUS MODE=CONNECTED CODEC=AUTO(1300) STATE=CLOSED PTT=OFF GRANTED=OFF "
            "TXQ=0 RXQ=0 TRANSPORT=M9");
    v = c.queryVoiceStatus();
    ASSERT_TRUE(v.has_value());
    ASSERT_FALSE(v->broadcast_rx);
    ASSERT_TRUE(v->real_time);
    PASS();
}

#endif

TEST_MAIN()
