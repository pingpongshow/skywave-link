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
    std::vector<std::string> received;  // command lines, in arrival order

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
                std::pair<int, std::string> r{0, verb};
                {
                    std::lock_guard<std::mutex> lock(mu);
                    received.push_back(buf);
                    // A reply keyed by the whole line wins over one keyed by verb.
                    auto it = replies.find(buf);
                    if (it == replies.end()) it = replies.find(verb);
                    if (it != replies.end()) r = it->second;
                }
                buf.clear();
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
    std::vector<std::string> commands() {
        std::lock_guard<std::mutex> lock(mu);
        return received;
    }
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

// Voice commands hand the modem's answer to the caller (the app explains
// ERROR:STATE / ERROR:UNSUPPORTED instead of swallowing them), EVT:VOICE
// TALKING/STOPPED drive the PTT flag, and downlink audio nobody plays is
// counted and bounded.
TEST(voice_command_answers_and_events) {
    FakeModem m;
    ASSERT_TRUE(m.listen());
    ModemClient c;
    ASSERT_TRUE(c.connect("127.0.0.1", m.pcmd, m.pdata, m.pvoice));
    m.acceptAll();
    m.reply("VOICESTART", 0, "ERROR:UNSUPPORTED");
    bool called = false, ok = true;
    std::string answer;
    c.voiceStart([&](bool k, const std::string& r) { called = true; ok = k; answer = r; });
    ASSERT_TRUE(called);
    ASSERT_FALSE(ok);
    ASSERT_EQ(answer, std::string("ERROR:UNSUPPORTED"));
    m.reply("VOICEPTT", 0, "VOICEPTT ON");
    called = false;
    c.setVoicePtt(true, [&](bool k, const std::string&) { called = true; ok = k; });
    ASSERT_TRUE(called);
    ASSERT_TRUE(ok);

    m.event("EVT:VOICE OPEN");
    m.event("EVT:VOICE TALKING");
    pump(c, 80);
    ASSERT_TRUE(c.getVoiceStatus().ptt);
    ASSERT_TRUE(c.getVoiceStatus().state == VoiceState::TxActive);
    m.event("EVT:VOICE STOPPED");
    pump(c, 80);
    ASSERT_FALSE(c.getVoiceStatus().ptt);
    ASSERT_TRUE(c.getVoiceStatus().state == VoiceState::Ready);

    // 12 s of downlink, never played: counted, buffer capped at 10 s.
    std::string blob;
    std::vector<int16_t> pcm(8000, 100);
    for (uint32_t seq = 0; seq < 12; ++seq) {
        blob += char(1); blob += char(2);
        const size_t n = pcm.size() * 2;
        blob += char(n >> 8); blob += char(n & 0xFF);
        for (int k = 3; k >= 0; --k) blob += char((seq >> (8 * k)) & 0xFF);
        for (int k = 0; k < 8; ++k) blob += char(0);
        for (int16_t v : pcm) { blob += char(uint16_t(v) & 0xFF); blob += char(uint16_t(v) >> 8); }
    }
    sendAll(m.voice, blob);
    for (int i = 0; i < 100 && c.voiceSamplesReceived() < 96000; ++i) pump(c, 20);
    ASSERT_EQ(c.voiceSamplesReceived(), uint64_t(96000));
    ASSERT_EQ(c.voiceSamplesBuffered(), ModemClient::kMaxVoiceRxSamples);
    c.discardVoicePcm();
    ASSERT_EQ(c.voiceSamplesBuffered(), size_t(0));
    PASS();
}

// Non-blocking commands: replies are matched in order without the caller
// waiting, ERROR: fails the oldest pending command, and a command whose
// reply never comes is reported once the deadline passes.
TEST(async_commands_resolve_in_order) {
    FakeModem m;
    ASSERT_TRUE(m.listen());
    ModemClient c;
    ASSERT_TRUE(c.connect("127.0.0.1", m.pcmd, m.pdata, m.pvoice));
    m.acceptAll();
    m.reply("ABORT", 0, "ERROR:STATE");
    m.reply("CQ", 0, "");  // swallowed: no reply line

    std::vector<std::string> log;
    auto record = [&log](const char* name) {
        return [&log, name](bool ok, const std::string& resp) {
            log.push_back(std::string(name) + (ok ? ":ok:" : ":fail:") + resp);
        };
    };
    const auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(c.sendCommandAsync("MYCALL K1AAA", record("mycall")));
    ASSERT_TRUE(c.sendCommandAsync("ABORT", record("abort")));
    ASSERT_TRUE(c.sendCommandAsync("CQ", record("cq"), 200));
    const auto sent_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    ASSERT_TRUE(sent_ms < 50);  // nothing waited for the modem
    ASSERT_EQ(c.pendingAsyncCommands(), size_t(3));

    pump(c, 400);
    ASSERT_EQ(log.size(), size_t(3));
    ASSERT_EQ(log[0], std::string("mycall:ok:MYCALL"));
    ASSERT_EQ(log[1], std::string("abort:fail:ERROR:STATE"));
    ASSERT_EQ(log[2], std::string("cq:fail:"));
    ASSERT_EQ(c.pendingAsyncCommands(), size_t(0));

    // In non-blocking mode the high-level commands return at once.
    c.setNonBlockingCommands(true);
    m.reply("CONNECT", 300, "CONNECT K1BBB");
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(c.connectToStation("k1bbb"));
    ASSERT_TRUE(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(100));
    ASSERT_EQ(c.pendingAsyncCommands(), size_t(1));
    pump(c, 450);
    ASSERT_EQ(c.pendingAsyncCommands(), size_t(0));
    PASS();
}

// Background connect: returns at once, reports the result once from
// pollConnect(), and a refused port fails without hanging.
TEST(background_connect_reports_once) {
    FakeModem m;
    ASSERT_TRUE(m.listen());
    ModemClient c;
    ASSERT_TRUE(c.beginConnect("127.0.0.1", m.pcmd, m.pdata, m.pvoice));
    ASSERT_TRUE(c.isConnecting());
    ConnectProgress p = ConnectProgress::InProgress;
    for (int i = 0; i < 200 && p == ConnectProgress::InProgress; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        p = c.pollConnect();
    }
    ASSERT_TRUE(p == ConnectProgress::Connected);
    ASSERT_TRUE(c.isConnected());
    ASSERT_TRUE(c.pollConnect() == ConnectProgress::None);
    m.acceptAll();
    ASSERT_EQ(c.getVersion().empty(), false);
    c.disconnect();

    // Nothing listens on the freed port: the connect is refused.
    uint16_t dead_port = 0;
    int l = makeListener(&dead_port);
    close(l);
    ModemClient d;
    ASSERT_TRUE(d.beginConnect("127.0.0.1", dead_port, dead_port, dead_port));
    p = ConnectProgress::InProgress;
    for (int i = 0; i < 400 && p == ConnectProgress::InProgress; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        p = d.pollConnect();
    }
    ASSERT_TRUE(p == ConnectProgress::Failed);
    ASSERT_FALSE(d.isConnected());

    // Cancelling an in-flight connect leaves the client disconnected.
    ModemClient e;
    ASSERT_TRUE(e.beginConnect("127.0.0.1", m.pcmd, m.pdata, m.pvoice));
    e.disconnect();
    ASSERT_FALSE(e.isConnecting());
    ASSERT_FALSE(e.isConnected());
    PASS();
}

// ULTRA is opt-in on the modem: FAMILY ULTRA is refused until ULTRA ON,
// and ULTRA OFF is refused while ULTRA is the family. Choosing ULTRA sends
// ULTRA ON first; leaving it is a plain FAMILY change.
TEST(family_change_sequencing) {
    ASSERT_EQ(familyChangeCommands("ultra").size(), size_t(2));
    ASSERT_EQ(familyChangeCommands("ULTRA")[0], std::string("ULTRA ON"));
    ASSERT_EQ(familyChangeCommands("ULTRA")[1], std::string("FAMILY ULTRA"));
    ASSERT_EQ(familyChangeCommands(" wide ").size(), size_t(1));
    ASSERT_EQ(familyChangeCommands("WIDE")[0], std::string("FAMILY WIDE"));
    ASSERT_TRUE(familyChangeCommands("HUGE").empty());

    FakeModem m;
    ASSERT_TRUE(m.listen());
    ModemClient c;
    ASSERT_TRUE(c.connect("127.0.0.1", m.pcmd, m.pdata, m.pvoice));
    m.acceptAll();
    c.setNonBlockingCommands(true);
    m.reply("ULTRA ON", 0, "ULTRA ON");
    m.reply("FAMILY ULTRA", 0, "FAMILY ULTRA");

    std::vector<std::string> results;
    auto record = [&results](bool ok, const std::string& resp) {
        results.push_back((ok ? "ok:" : "fail:") + resp);
    };
    ASSERT_TRUE(c.setFamily("ULTRA", record));
    pump(c, 200);
    ASSERT_EQ(results.size(), size_t(1));
    ASSERT_EQ(results[0], std::string("ok:FAMILY ULTRA"));
    auto cmds = m.commands();
    ASSERT_EQ(cmds.size(), size_t(2));
    ASSERT_EQ(cmds[0], std::string("ULTRA ON"));
    ASSERT_EQ(cmds[1], std::string("FAMILY ULTRA"));

    // Leaving ULTRA: FAMILY only, never ULTRA OFF.
    m.reply("FAMILY MID", 0, "FAMILY MID");
    ASSERT_TRUE(c.setFamily("MID", record));
    pump(c, 200);
    ASSERT_EQ(results.back(), std::string("ok:FAMILY MID"));
    cmds = m.commands();
    ASSERT_EQ(cmds.size(), size_t(3));
    ASSERT_EQ(cmds[2], std::string("FAMILY MID"));

    // A refused ULTRA ON stops the chain: FAMILY ULTRA is never sent and
    // the caller sees the refusal.
    m.reply("ULTRA ON", 0, "ERROR:STATE");
    ASSERT_TRUE(c.setFamily("ULTRA", record));
    pump(c, 200);
    ASSERT_EQ(results.back(), std::string("fail:ERROR:STATE"));
    cmds = m.commands();
    ASSERT_EQ(cmds.size(), size_t(4));
    ASSERT_EQ(cmds[3], std::string("ULTRA ON"));
    ASSERT_CONTAINS(describeCommandError("FAMILY ULTRA", "ERROR:STATE"), "ULTRA");
    ASSERT_CONTAINS(describeCommandError("CONNECT K1BBB", "ERROR:STATE"), "busy");
    ASSERT_CONTAINS(describeCommandError("CQ", ""), "did not answer");
    PASS();
}

// Failed and refused calls: the reason reaches the app, the raw FSM state
// (CALLING) is visible, and the reason reads as plain words.
TEST(disconnect_reason_and_fsm_state) {
    FakeModem m;
    ASSERT_TRUE(m.listen());
    ModemClient c;
    std::vector<std::string> reasons;
    c.onDisconnected([&](const std::string& reason) { reasons.push_back(reason); });
    ASSERT_TRUE(c.connect("127.0.0.1", m.pcmd, m.pdata, m.pvoice));
    m.acceptAll();

    m.event("EVT:STATE CALLING");
    pump(c, 100);
    ASSERT_EQ(c.getStatus().fsm_state, std::string("CALLING"));
    ASSERT_TRUE(c.getStatus().link_state == LinkState::Connecting);
    m.event("EVT:DISCONNECTED NO_ANSWER");
    m.event("EVT:STATE IDLE");
    pump(c, 100);
    ASSERT_EQ(reasons.size(), size_t(1));
    ASSERT_EQ(reasons[0], std::string("NO_ANSWER"));
    ASSERT_EQ(c.getStatus().fsm_state, std::string("IDLE"));

    m.reply("STATUS", 0, "STATUS STATE=LISTENING CALL=K1AAA REMOTE=- FAMILY=MID MODE=AUTO(M6)");
    ASSERT_TRUE(c.refreshStatus());
    ASSERT_EQ(c.getStatus().fsm_state, std::string("LISTENING"));
    ASSERT_EQ(c.getStatus().family, std::string("MID"));
    ASSERT_EQ(c.getStatus().mode, std::string("AUTO(M6)"));

    ASSERT_CONTAINS(describeDisconnectReason("NO_ANSWER"), "No answer");
    ASSERT_CONTAINS(describeDisconnectReason("REFUSED"), "refused");
    ASSERT_CONTAINS(describeDisconnectReason("TIMEOUT"), "timed out");
    ASSERT_CONTAINS(describeDisconnectReason("LINK"), "Link lost");
    PASS();
}

// EVT:SNR carries both ends: `<local> <remote|->`.
TEST(snr_event_sets_local_and_remote) {
    FakeModem m;
    ASSERT_TRUE(m.listen());
    ModemClient c;
    ASSERT_TRUE(c.connect("127.0.0.1", m.pcmd, m.pdata, m.pvoice));
    m.acceptAll();
    m.event("EVT:CONNECTED K1BBB WIDE M8");
    m.event("EVT:SNR 12.5 -3.0");
    pump(c, 100);
    ModemStatus st = c.getStatus();
    ASSERT_TRUE(st.snr_db > 12.4f && st.snr_db < 12.6f);
    ASSERT_TRUE(st.remote_snr_valid);
    ASSERT_TRUE(st.remote_snr_db > -3.1f && st.remote_snr_db < -2.9f);
    m.event("EVT:SNR 9 -");
    pump(c, 100);
    st = c.getStatus();
    ASSERT_FALSE(st.remote_snr_valid);
    PASS();
}

// Outside a session, data-port bytes would wait in the modem's TX queue
// and go to whoever connects next: sendData refuses them.
TEST(send_data_refused_without_session) {
    FakeModem m;
    ASSERT_TRUE(m.listen());
    ModemClient c;
    ASSERT_TRUE(c.connect("127.0.0.1", m.pcmd, m.pdata, m.pvoice));
    m.acceptAll();
    pump(c, 50);
    ASSERT_FALSE(c.sendData({0xF4, 1, 2, 3}));
    ASSERT_FALSE(c.sendText("hello"));
    m.event("EVT:CONNECTED K1BBB WIDE M8");
    pump(c, 100);
    ASSERT_TRUE(c.sendData({0x01, 0x02}));
    m.event("EVT:DISCONNECTED NORMAL");
    pump(c, 100);
    ASSERT_FALSE(c.sendData({0x03}));

    timeval tv{0, 100000};
    setsockopt(m.data, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::string rx;
    char buf[256];
    ssize_t n;
    while ((n = recv(m.data, buf, sizeof(buf), 0)) > 0) rx.append(buf, static_cast<size_t>(n));
    ASSERT_EQ(rx, frame(std::string("\x01\x02")));  // only the in-session message
    PASS();
}

// K10: a blocking query that times out must not leave its late `ERROR:` to
// be taken as the answer of the next (async) command, and a pending async
// command must not steal a blocking query's reply.
TEST(blocking_query_and_async_commands_share_ordering) {
    FakeModem m;
    ASSERT_TRUE(m.listen());
    ModemClient c;
    ASSERT_TRUE(c.connect("127.0.0.1", m.pcmd, m.pdata, m.pvoice));
    m.acceptAll();
    c.setNonBlockingCommands(true);

    // STATUS answers late with an error; the query gives up first.
    m.reply("STATUS", 250, "ERROR:STATE");
    ASSERT_FALSE(c.refreshStatus(100));
    std::vector<std::string> log;
    ASSERT_TRUE(c.sendCommandAsync("LISTEN ON", [&](bool ok, const std::string& r) {
        log.push_back(std::string(ok ? "ok:" : "fail:") + r);
    }));
    pump(c, 500);
    ASSERT_EQ(log.size(), size_t(1));
    ASSERT_EQ(log[0], std::string("ok:LISTEN"));  // not the late STATUS error
    ASSERT_EQ(c.pendingAsyncCommands(), size_t(0));

    // An async command still pending when a blocking query goes out: each
    // gets its own reply.
    m.reply("CQ", 100, "CQ");
    m.reply("VERSION", 0, "VERSION TEST-2");
    log.clear();
    ASSERT_TRUE(c.sendCommandAsync("CQ", [&](bool ok, const std::string& r) {
        log.push_back(std::string(ok ? "ok:" : "fail:") + r);
    }));
    ASSERT_EQ(c.getVersion(), std::string("TEST-2"));
    pump(c, 50);
    ASSERT_EQ(log.size(), size_t(1));
    ASSERT_EQ(log[0], std::string("ok:CQ"));
    PASS();
}

// K5: the PTT state the owner reports survives STATUS refreshes.
TEST(ptt_active_reported_by_owner) {
    FakeModem m;
    ASSERT_TRUE(m.listen());
    ModemClient c;
    ASSERT_TRUE(c.connect("127.0.0.1", m.pcmd, m.pdata, m.pvoice));
    m.acceptAll();
    ASSERT_FALSE(c.getStatus().ptt_active);
    std::thread keyer([&c]() { c.setPttActive(true); });  // the PTT server's thread
    keyer.join();
    ASSERT_TRUE(c.getStatus().ptt_active);
    m.reply("STATUS", 0, "STATUS STATE=IDLE CALL=K1AAA REMOTE=- FAMILY=WIDE MODE=AUTO(M8)");
    ASSERT_TRUE(c.refreshStatus());
    ASSERT_TRUE(c.getStatus().ptt_active);
    c.setPttActive(false);
    ASSERT_FALSE(c.getStatus().ptt_active);
    PASS();
}

// K6: the modem is told to key through this app's PTT server.
TEST(use_ptt_server_sends_method_then_port) {
    FakeModem m;
    ASSERT_TRUE(m.listen());
    ModemClient c;
    ASSERT_TRUE(c.connect("127.0.0.1", m.pcmd, m.pdata, m.pvoice));
    m.acceptAll();
    m.reply("PTTMETHOD", 0, "PTTMETHOD TCP");
    m.reply("PTTPORT", 0, "PTTPORT 127.0.0.1:4532");
    int done = 0;
    bool ok = false;
    ASSERT_TRUE(c.usePttServer("127.0.0.1", 4532, [&](bool o, const std::string&) {
        ++done;
        ok = o;
    }));
    pump(c, 200);
    ASSERT_EQ(done, 1);
    ASSERT_TRUE(ok);
    auto cmds = m.commands();
    ASSERT_EQ(cmds.size(), size_t(2));
    ASSERT_EQ(cmds[0], std::string("PTTMETHOD TCP"));
    ASSERT_EQ(cmds[1], std::string("PTTPORT 127.0.0.1:4532"));
    ASSERT_FALSE(c.usePttServer("127.0.0.1", 0));
    PASS();
}

// K8: voice uplink never blocks the caller and never leaves a partial
// record on the socket: what the modem reads is a run of whole records with
// consecutive sequence numbers, one per accepted call.
TEST(voice_uplink_queues_whole_records_without_blocking) {
    FakeModem m;
    ASSERT_TRUE(m.listen());
    ModemClient c;
    ASSERT_TRUE(c.connect("127.0.0.1", m.pcmd, m.pdata, m.pvoice));
    m.acceptAll();
    pump(c, 20);

    std::vector<int16_t> pcm(320, 1234);
    size_t accepted = 0;
    bool refused = false;
    long long worst_us = 0;
    for (int i = 0; i < 20000 && !refused; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = c.sendVoicePcm(pcm.data(), pcm.size());
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count();
        worst_us = std::max(worst_us, static_cast<long long>(us));
        if (ok) ++accepted; else refused = true;
    }
    ASSERT_TRUE(refused);                    // the backlog bound was reached
    ASSERT_TRUE(worst_us < 20000);           // no call slept
    ASSERT_TRUE(c.pendingVoiceBytes() <= ModemClient::kMaxVoiceBacklogBytes);

    // Drain: the modem side reads while the client flushes from poll().
    std::vector<uint8_t> rx;
    const size_t want = accepted * (16 + 640);
    int fl = fcntl(m.voice, F_GETFL, 0);
    fcntl(m.voice, F_SETFL, fl | O_NONBLOCK);
    for (int i = 0; i < 4000 && rx.size() < want; ++i) {
        uint8_t b[65536];
        ssize_t n;
        while ((n = recv(m.voice, b, sizeof(b), 0)) > 0) rx.insert(rx.end(), b, b + n);
        c.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(rx.size(), want);
    for (size_t r = 0; r < accepted; ++r) {
        const uint8_t* rec = rx.data() + r * (16 + 640);
        ASSERT_EQ(rec[0], uint8_t(1));
        ASSERT_EQ(rec[1], uint8_t(1));
        ASSERT_EQ((size_t(rec[2]) << 8) | rec[3], size_t(640));
        const uint32_t seq = uint32_t(rec[4]) << 24 | uint32_t(rec[5]) << 16 |
                             uint32_t(rec[6]) << 8 | rec[7];
        ASSERT_EQ(seq, uint32_t(r));
    }
    ASSERT_EQ(c.pendingVoiceBytes(), size_t(0));
    PASS();
}

// K7: a voice socket the modem closes is reopened by poll().
TEST(voice_socket_reopened_after_close) {
    FakeModem m;
    ASSERT_TRUE(m.listen());
    ModemClient c;
    c.setVoiceReconnectInterval(std::chrono::milliseconds(50));
    ASSERT_TRUE(c.connect("127.0.0.1", m.pcmd, m.pdata, m.pvoice));
    m.acceptAll();
    pump(c, 20);
    ASSERT_TRUE(c.voiceMediaState() == VoiceMediaState::Connected);
    close(m.voice);
    m.voice = -1;
    pump(c, 30);
    ASSERT_FALSE(c.hasVoiceMedia());
    ASSERT_TRUE(c.voiceMediaState() == VoiceMediaState::Reconnecting);
    pump(c, 100);  // the reopen starts in the background
    m.voice = accept(m.lvoice, nullptr, nullptr);
    ASSERT_TRUE(m.voice >= 0);
    pump(c, 100);
    ASSERT_TRUE(c.hasVoiceMedia());
    std::vector<int16_t> pcm(160, 7);
    ASSERT_TRUE(c.sendVoicePcm(pcm.data(), pcm.size()));
    c.disconnect();
    ASSERT_TRUE(c.voiceMediaState() == VoiceMediaState::Disconnected);
    PASS();
}

// K9: voice events and the STATUS TURN=/VOICE= fields update the snapshot
// at once, without waiting for the next VOICESTATUS poll.
TEST(voice_events_and_status_turn_fields) {
    FakeModem m;
    ASSERT_TRUE(m.listen());
    ModemClient c;
    int callbacks = 0;
    c.onVoiceStatus([&](const VoiceStatus&) { ++callbacks; });
    ASSERT_TRUE(c.connect("127.0.0.1", m.pcmd, m.pdata, m.pvoice));
    m.acceptAll();
    m.reply("VOICESTATUS", 0,
            "VOICESTATUS MODE=CONNECTED CODEC=AUTO(2400) STATE=OPEN PTT=OFF GRANTED=OFF "
            "TXQ=0 RXQ=0 TRANSPORT=M9");
    ASSERT_TRUE(c.queryVoiceStatus().has_value());
    m.event("EVT:VOICE CODEC 1300");
    pump(c, 60);
    ASSERT_EQ(c.getVoiceStatus().codec, std::string("AUTO(1300)"));
    m.event("EVT:VOICE BCAST 3F2A");
    pump(c, 60);
    ASSERT_TRUE(c.getVoiceStatus().broadcast_rx);
    ASSERT_EQ(c.getVoiceStatus().broadcast_tag, std::string("3F2A"));
    m.event("EVT:VOICE BCAST END");
    pump(c, 60);
    ASSERT_FALSE(c.getVoiceStatus().broadcast_rx);
    ASSERT_TRUE(c.getVoiceStatus().broadcast_tag.empty());
    m.event("EVT:VOICE CLOSED");
    pump(c, 60);
    ASSERT_TRUE(c.getVoiceStatus().mode == VoiceMode::Connected);  // a setting, not state
    ASSERT_TRUE(c.getVoiceStatus().state == VoiceState::Idle);

    m.reply("STATUS", 0,
            "STATUS STATE=CONNECTED CALL=K1AAA REMOTE=K1BBB FAMILY=WIDE MODE=AUTO(M8) "
            "NET=1200.0 SNR=3.0 RSNR=- CFO=0.0 RETRY=0.0 BUSY=0 LISTEN=1 COMPRESS=1 "
            "CRYPTO=0 EPOCH=0 RESUME=1 MASK=0 VOICE=talking TXBUF=0 FOK=1 FFAIL=0 TURN=ISS");
    ASSERT_TRUE(c.refreshStatus());
    ModemStatus st = c.getStatus();
    ASSERT_EQ(st.turn, std::string("ISS"));
    ASSERT_EQ(st.voice_summary, std::string("talking"));
    ASSERT_TRUE(st.voice.state == VoiceState::TxActive);
    m.reply("STATUS", 0, "STATUS STATE=CONNECTED VOICE=bcastrx TURN=-");
    ASSERT_TRUE(c.refreshStatus());
    st = c.getStatus();
    ASSERT_EQ(st.turn, std::string("-"));
    ASSERT_TRUE(st.voice.broadcast_rx);
    ASSERT_TRUE(st.voice.state == VoiceState::Idle);
    ASSERT_TRUE(callbacks >= 5);
    PASS();
}

// K23 / K25: VOICECODEC values are validated; MAXMODE and CWID answers are
// kept in the status snapshot.
TEST(voice_codec_and_mode_settings) {
    FakeModem m;
    ASSERT_TRUE(m.listen());
    ModemClient c;
    ASSERT_TRUE(c.connect("127.0.0.1", m.pcmd, m.pdata, m.pvoice));
    m.acceptAll();
    ASSERT_EQ(normalizeVoiceCodec("700c"), std::string("700C"));
    ASSERT_EQ(normalizeVoiceCodec(" auto "), std::string("AUTO"));
    ASSERT_TRUE(normalizeVoiceCodec("1600").empty());
    ASSERT_FALSE(c.setVoiceCodec("1600"));
    m.reply("VOICECODEC", 0, "VOICECODEC 3200");
    ASSERT_TRUE(c.setVoiceCodec("3200"));
    m.reply("MAXMODE", 0, "MAXMODE M10");
    m.reply("CWID", 0, "CWID ON");
    c.requestModeSettings();
    pump(c, 100);
    ModemStatus st = c.getStatus();
    ASSERT_EQ(st.max_mode, std::string("M10"));
    ASSERT_TRUE(st.cw_id_known);
    ASSERT_TRUE(st.cw_id);
    auto cmds = m.commands();
    ASSERT_EQ(cmds[0], std::string("VOICECODEC 3200"));
    ASSERT_EQ(cmds[1], std::string("MAXMODE"));
    ASSERT_EQ(cmds[2], std::string("CWID"));
    PASS();
}

#endif

TEST_MAIN()
