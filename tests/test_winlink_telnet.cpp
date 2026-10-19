// Winlink CMS client tests: secure login vectors, B2F parsing, and full
// B2F exchanges against a local fake CMS (no real servers or accounts).

#include "test_macros.hpp"
#include "skywave_link/b2f.hpp"
#include "skywave_link/lzh.hpp"
#include "skywave_link/winlink_telnet_client.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace skylink;

namespace {

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

// Test-only account data (fake CMS, never a real account).
constexpr const char* kCall = "K1AAA";
constexpr const char* kPassword = "FooBar";
constexpr const char* kChallenge = "23753528";
// wl2k-go fbb/secure_test.go: secureLoginResponse("23753528", "FooBar").
constexpr const char* kExpectedResponse = "95074758";

std::vector<uint8_t> inboundB2F(const std::string& mid, const std::string& subject,
                                const std::string& body) {
    B2FHeader h;
    h.message_id = mid;
    h.date_time = "2026/01/15 12:00";
    h.from = "SMTP:sender@example.com";
    h.to = {kCall};
    h.subject = subject;
    h.mbo = "SMTP";
    h.extra = {{"Content-Type", "text/plain; charset=ISO-8859-1"}};
    return B2FCodec().encode(h, body, {});
}

#ifndef _WIN32

struct FakeCmsConfig {
    std::string password = kPassword;   // account password the CMS checks
    bool send_challenge = true;
    std::string answer = "+";           // FS answer for the client's proposals
    std::vector<std::pair<std::string, std::vector<uint8_t>>> inbound;  // mid, B2F message
    bool corrupt_block_checksum = false;
    bool corrupt_lzh = false;
};

struct FakeCmsLog {
    std::mutex mu;
    std::string received;               // every byte from the client
    std::string callsign;
    std::string telnet_password;
    std::string fw, sid, pr;
    std::vector<std::string> proposals;
    std::vector<std::vector<uint8_t>> messages;  // decompressed client messages
    std::string inbound_answer;
    std::string after_transfer;         // client's line after the inbound transfer
    std::atomic<bool> acked{false};     // client took its turn after receiving
    std::string error;

    void fail(const std::string& e) {
        std::lock_guard<std::mutex> lock(mu);
        if (error.empty()) error = e;
    }
};

class ServerConn {
public:
    ServerConn(int sock, FakeCmsLog& log) : sock_(sock), log_(log) {}

    bool send(const std::string& data) {
        size_t off = 0;
        while (off < data.size()) {
            int flags = 0;
#ifdef MSG_NOSIGNAL
            flags = MSG_NOSIGNAL;
#endif
            const ssize_t n = ::send(sock_, data.data() + off, data.size() - off, flags);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }

    bool byte(uint8_t* b) {
        char c = 0;
        if (::recv(sock_, &c, 1, 0) != 1) return false;
        *b = static_cast<uint8_t>(c);
        std::lock_guard<std::mutex> lock(log_.mu);
        log_.received.push_back(c);
        return true;
    }

    // CR-terminated line (LF ignored).
    bool line(std::string* out) {
        out->clear();
        for (;;) {
            uint8_t b = 0;
            if (!byte(&b)) return false;
            if (b == '\r') {
                if (out->empty()) continue;
                return true;
            }
            if (b == '\n') continue;
            out->push_back(static_cast<char>(b));
            if (out->size() > 4096) return false;
        }
    }

private:
    int sock_;
    FakeCmsLog& log_;
};

uint8_t blockChecksum(const std::vector<std::string>& lines) {
    return winlinkProposalChecksum(lines);
}

std::string hex2(uint8_t v) {
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%02X", v);
    return buf;
}

// Sends one compressed message (SOH header, STX blocks, EOT checksum).
bool sendCompressed(ServerConn& c, const std::string& title, const std::vector<uint8_t>& data,
                    bool corrupt) {
    std::string out;
    out.push_back('\x01');
    out.push_back(static_cast<char>(title.size() + 1 + 2));
    out += title;
    out.push_back('\0');
    out += "0";
    out.push_back('\0');
    unsigned sum = 0;
    for (size_t p = 0; p < data.size(); p += 100) {
        const size_t n = std::min<size_t>(100, data.size() - p);
        out.push_back('\x02');
        out.push_back(static_cast<char>(n));
        for (size_t k = 0; k < n; ++k) {
            out.push_back(static_cast<char>(data[p + k]));
            sum += data[p + k];
        }
    }
    out.push_back('\x04');
    uint8_t cks = static_cast<uint8_t>((0x100 - (sum & 0xFF)) & 0xFF);
    if (corrupt) cks ^= 0x55;
    out.push_back(static_cast<char>(cks));
    return c.send(out);
}

bool receiveCompressed(ServerConn& c, std::vector<uint8_t>* data, std::string* err) {
    uint8_t b = 0;
    if (!c.byte(&b) || b != 0x01) { *err = "no SOH"; return false; }
    uint8_t hlen = 0;
    if (!c.byte(&hlen)) { *err = "no header length"; return false; }
    std::string header;
    for (int i = 0; i < hlen; ++i) {
        if (!c.byte(&b)) { *err = "short header"; return false; }
        header.push_back(static_cast<char>(b));
    }
    if (header.empty() || header.back() != '\0' || header.find('\0') + 3 != header.size() ||
        header[header.size() - 2] != '0') {
        *err = "bad SOH header";
        return false;
    }
    unsigned sum = 0;
    for (;;) {
        if (!c.byte(&b)) { *err = "short data"; return false; }
        if (b == 0x02) {
            uint8_t n = 0;
            if (!c.byte(&n)) { *err = "no block length"; return false; }
            const int len = n == 0 ? 256 : n;
            for (int i = 0; i < len; ++i) {
                if (!c.byte(&b)) { *err = "short block"; return false; }
                data->push_back(b);
                sum += b;
            }
        } else if (b == 0x04) {
            if (!c.byte(&b)) { *err = "no checksum"; return false; }
            if (((sum + b) & 0xFF) != 0) { *err = "bad block checksum"; return false; }
            return true;
        } else {
            *err = "unexpected byte";
            return false;
        }
    }
}

void fakeCms(int sock, const FakeCmsConfig& cfg, FakeCmsLog& log) {
    ServerConn c(sock, log);
    std::string line;

    c.send("Skywave test CMS\r\nCallsign :\r");
    if (!c.line(&line)) return log.fail("no callsign");
    log.callsign = line;
    c.send("Password :\r");
    if (!c.line(&line)) return log.fail("no telnet password");
    log.telnet_password = line;

    std::string hs = "[WL2K-5.0-B2FWIHJM$]\r";
    if (cfg.send_challenge) hs += std::string(";PQ: ") + kChallenge + "\r";
    hs += "CMS via TEST >\r";
    c.send(hs);

    // Client handshake, then its proposals or FF.
    std::vector<std::string> props;
    for (;;) {
        if (!c.line(&line)) return log.fail("client handshake incomplete");
        if (startsWith(line, ";FW:")) log.fw = line;
        else if (startsWith(line, ";PR:")) log.pr = line.substr(5);
        else if (startsWith(line, "[")) log.sid = line;
        else if (startsWith(line, ";")) continue;
        else if (startsWith(line, "FC ")) props.push_back(line);
        else if (line == "FF") break;
        else if (startsWith(line, "F> ")) {
            if (line.substr(3) != hex2(blockChecksum(props))) return log.fail("client proposal checksum");
            break;
        } else {
            return log.fail("unexpected client line: " + line);
        }
    }
    if (cfg.send_challenge &&
        log.pr != buildWinlinkChallengeResponse(cfg.password, kChallenge)) {
        c.send("*** Secure login failed - account password does not match. - Disconnecting\r");
        return;
    }
    log.proposals = props;

    if (!props.empty()) {
        std::string fs = "FS ";
        for (size_t i = 0; i < props.size(); ++i) fs += cfg.answer;
        c.send(fs + "\r");
        if (cfg.answer == "+") {
            for (size_t i = 0; i < props.size(); ++i) {
                std::vector<uint8_t> data;
                std::string err;
                if (!receiveCompressed(c, &data, &err)) return log.fail(err);
                std::vector<uint8_t> plain;
                if (!LZHCodec().decompress(data, plain, &err)) return log.fail("client LZH: " + err);
                log.messages.push_back(plain);
            }
        }
    }

    // Server turn.
    if (cfg.inbound.empty()) {
        c.send("FF\r");
        if (c.line(&line)) log.after_transfer = line;
        return;
    }
    std::vector<std::string> lines;
    std::vector<std::vector<uint8_t>> compressed;
    for (const auto& [mid, msg] : cfg.inbound) {
        auto z = LZHCodec().compress(msg);
        if (cfg.corrupt_lzh) z[z.size() / 2] ^= 0x10;
        lines.push_back("FC EM " + mid + " " + std::to_string(msg.size()) + " " +
                        std::to_string(z.size()) + " 0");
        compressed.push_back(std::move(z));
    }
    std::string block;
    for (const auto& l : lines) block += l + "\r";
    block += "F> " + hex2(blockChecksum(lines)) + "\r";
    c.send(";PM: K1AAA ABC 123 sender@example.com Pending\r" + block);
    if (!c.line(&line)) return log.fail("no FS from client");
    log.inbound_answer = line;
    if (line.size() != 3 + lines.size()) return log.fail("bad FS: " + line);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (line[3 + i] == '+') {
            sendCompressed(c, "Subject " + std::to_string(i), compressed[i], cfg.corrupt_block_checksum);
        }
    }
    // The client's next protocol line acknowledges the transfer.
    if (c.line(&line) && startsWith(line, "F")) {
        log.after_transfer = line;
        log.acked = true;
    }
}

struct FakeCms {
    uint16_t port = 0;
    std::shared_ptr<FakeCmsLog> log = std::make_shared<FakeCmsLog>();
    std::thread thread;

    explicit FakeCms(FakeCmsConfig cfg) {
        std::promise<uint16_t> port_promise;
        auto port_future = port_promise.get_future();
        thread = std::thread([cfg, log = log, p = std::move(port_promise)]() mutable {
            const int ls = ::socket(AF_INET, SOCK_STREAM, 0);
            int opt = 1;
            (void)setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            socklen_t len = sizeof(addr);
            if (ls < 0 || ::bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
                ::listen(ls, 1) != 0 ||
                ::getsockname(ls, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
                if (ls >= 0) ::close(ls);
                p.set_value(0);
                return;
            }
            p.set_value(ntohs(addr.sin_port));
            const int s = ::accept(ls, nullptr, nullptr);
            ::close(ls);
            if (s < 0) return;
            timeval tv{};
            tv.tv_sec = 8;
            (void)setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            (void)setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
            const int one = 1;
            (void)setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            fakeCms(s, cfg, *log);
            ::close(s);
        });
        port = port_future.get();
    }
    ~FakeCms() {
        if (thread.joinable()) thread.join();
    }
    void join() {
        if (thread.joinable()) thread.join();
    }
};

WinlinkTelnetRequest sendRequest(uint16_t port) {
    WinlinkTelnetRequest req;
    req.host = "127.0.0.1";
    req.port = port;
    req.use_tls = false;
    req.timeout_ms = 5000;
    req.callsign = "k1aaa";
    req.password = kPassword;
    req.to = "user@example.com";
    req.subject = "Integration Test";
    req.body = "Line A\nLine B";
    return req;
}

WinlinkTelnetFetchRequest fetchRequest(uint16_t port) {
    WinlinkTelnetFetchRequest req;
    req.host = "127.0.0.1";
    req.port = port;
    req.use_tls = false;
    req.timeout_ms = 5000;
    req.callsign = kCall;
    req.password = kPassword;
    req.max_messages = 10;
    return req;
}

#endif  // !_WIN32

}  // namespace

// --- Secure login -----------------------------------------------------------

TEST(secure_login_reference_vectors) {
    // From wl2k-go fbb/secure_test.go and computed with its secureLoginResponse.
    ASSERT_EQ(buildWinlinkChallengeResponse("FOOBAR", "23753528"), std::string("72768415"));
    ASSERT_EQ(buildWinlinkChallengeResponse("FooBar", "23753528"), std::string("95074758"));
    ASSERT_EQ(buildWinlinkChallengeResponse("test-pass", "12345678"), std::string("49687343"));
    ASSERT_EQ(buildWinlinkChallengeResponse("Sk7waveL1nk", "98765432"), std::string("90814125"));
    PASS();
}

TEST(extract_pq_challenge) {
    ASSERT_EQ(extractWinlinkChallengeToken(";PQ: 23753528"), std::string("23753528"));
    ASSERT_EQ(extractWinlinkChallengeToken("[WL2K-5.0-B2FWIHJM$]\r;PQ: 12345678\rCMS >\r"),
              std::string("12345678"));
    ASSERT_EQ(extractWinlinkChallengeToken("Password :"), std::string(""));
    PASS();
}

TEST(proposal_checksum_reference_vector) {
    // wl2k-go fbb/wl2k_test.go: "FC EM TJKYEIMMHSRB 527 123 0" -> "F> 3b".
    ASSERT_EQ(static_cast<int>(winlinkProposalChecksum({"FC EM TJKYEIMMHSRB 527 123 0"})), 0x3B);
    ASSERT_EQ(static_cast<int>(winlinkProposalChecksum({})), 0);
    PASS();
}

// --- Parsing ----------------------------------------------------------------

TEST(parse_inbound_b2f_message) {
    // Latin-1 body and an RFC 2047 subject, as the CMS delivers them.
    const std::string raw =
        "Mid: ABCDEF123456\r\n"
        "Date: 2026/01/15 12:34\r\n"
        "Type: Private\r\n"
        "From: SMTP:someone@example.com\r\n"
        "To: K1AAA\r\n"
        "To: SMTP:other@example.com\r\n"
        "Subject: =?ISO-8859-1?Q?Gr=FC=DFe?= aus Berlin\r\n"
        "Mbo: SMTP\r\n"
        "Content-Type: text/plain; charset=ISO-8859-1\r\n"
        "Body: 16\r\n"
        "File: 3 a.bin\r\n"
        "\r\n"
        "Caf\xE9\r\nLine two\r\n"
        "\r\n"
        "xyz\r\n";
    const auto msg = parseWinlinkInboundMessage(raw);
    ASSERT_EQ(msg.message_id, std::string("ABCDEF123456"));
    ASSERT_EQ(msg.from, std::string("someone@example.com"));
    ASSERT_EQ(msg.to, std::string("K1AAA, other@example.com"));
    ASSERT_EQ(msg.subject, std::string("Gr\xC3\xBC\xC3\x9F" "e aus Berlin"));
    ASSERT_EQ(msg.body, std::string("Caf\xC3\xA9\nLine two"));
    ASSERT_EQ(msg.date, std::string("2026/01/15 12:34"));
    ASSERT_EQ(msg.attachments.size(), size_t(1));
    ASSERT_EQ(msg.attachments[0].name, std::string("a.bin"));
    ASSERT_EQ(msg.raw, raw);
    PASS();
}

TEST(parse_inbound_message_fallback) {
    const std::string raw = "From: N0CALL\r\nSubject: Test inbound\r\nMsgId: MID-123\r\n\r\nLine one\r\nLine two\r\n";
    auto msg = parseWinlinkInboundMessage(raw);
    ASSERT_EQ(msg.from, std::string("N0CALL"));
    ASSERT_EQ(msg.subject, std::string("Test inbound"));
    ASSERT_EQ(msg.message_id, std::string("MID-123"));
    ASSERT_EQ(msg.body, std::string("Line one\nLine two"));

    const std::string plain = "No structured headers in this payload";
    ASSERT_EQ(parseWinlinkInboundMessage(plain).body, plain);
    PASS();
}

// --- Validation -------------------------------------------------------------

TEST(send_delivery_empty_destination_fails_fast) {
    WinlinkTelnetRequest req;
    req.callsign = kCall;
    req.password = kPassword;
    req.to = "";
    std::string error;
    ASSERT_FALSE(sendWinlinkTelnetDelivery(req, &error));
    ASSERT_CONTAINS(error, "destination");
    PASS();
}

TEST(fetch_invalid_max_messages_fails_fast) {
    WinlinkTelnetFetchRequest req;
    req.max_messages = 0;
    auto result = fetchWinlinkTelnetMessages(req);
    ASSERT_FALSE(result.success);
    ASSERT_CONTAINS(result.error, "max_messages");
    PASS();
}

TEST(defaults_use_cms_tls_port) {
    WinlinkTelnetRequest send;
    WinlinkTelnetFetchRequest fetch;
    ASSERT_TRUE(send.use_tls);
    ASSERT_EQ(send.port, uint16_t(8773));
    ASSERT_TRUE(fetch.use_tls);
    ASSERT_EQ(fetch.port, uint16_t(8773));
    PASS();
}

#ifndef _WIN32

// --- Outbound ---------------------------------------------------------------

TEST(send_delivers_b2f_message_with_secure_login) {
    FakeCms cms{FakeCmsConfig{}};
    ASSERT_GT(static_cast<int>(cms.port), 0);
    auto req = sendRequest(cms.port);
    req.subject = "Integration Test \xC3\xA9";  // non-ASCII subject (UTF-8)
    std::string error, mode;
    int attempts = 0;
    const bool ok = sendWinlinkTelnetDelivery(req, &error, &mode, &attempts);
    cms.join();
    FakeCmsLog& log = *cms.log;
    ASSERT_EQ(log.error, std::string(""));
    ASSERT_TRUE(ok);
    ASSERT_TRUE(error.empty());
    ASSERT_EQ(mode, std::string("b2f-secure-login"));
    ASSERT_EQ(attempts, 1);

    // Login: callsign, the fixed telnet token, then the B2F handshake.
    ASSERT_EQ(log.callsign, std::string("K1AAA"));
    ASSERT_EQ(log.telnet_password, std::string("CMSTelnet"));
    ASSERT_EQ(log.fw, std::string(";FW: K1AAA"));
    ASSERT_TRUE(startsWith(log.sid, "[SkywaveLink-") && log.sid.find("B2F") != std::string::npos);
    ASSERT_EQ(log.pr, std::string(kExpectedResponse));
    // The account password itself never crosses the wire.
    ASSERT_TRUE(log.received.find(kPassword) == std::string::npos);

    // One FC EM proposal, transferred and LZH-decodable into valid B2F.
    ASSERT_EQ(log.proposals.size(), size_t(1));
    ASSERT_TRUE(startsWith(log.proposals[0], "FC EM "));
    ASSERT_EQ(log.messages.size(), size_t(1));
    B2FHeader h;
    std::string body;
    B2FAttachments att;
    std::string err;
    ASSERT_TRUE(B2FCodec().decode(log.messages[0], h, body, att, &err));
    // "FC EM <mid> <uncompressed size> <compressed size> 0"
    std::vector<std::string> parts;
    {
        std::string cur;
        for (char ch : log.proposals[0] + " ") {
            if (ch == ' ') { parts.push_back(cur); cur.clear(); }
            else cur.push_back(ch);
        }
    }
    ASSERT_EQ(parts.size(), size_t(6));
    ASSERT_EQ(parts[2], h.message_id);
    ASSERT_EQ(parts[3], std::to_string(log.messages[0].size()));
    ASSERT_EQ(parts[5], std::string("0"));
    ASSERT_EQ(h.message_id.size(), size_t(12));
    ASSERT_EQ(h.from, std::string("K1AAA"));
    ASSERT_EQ(h.mbo, std::string("K1AAA"));
    ASSERT_EQ(h.to.size(), size_t(1));
    ASSERT_EQ(h.to[0], std::string("SMTP:user@example.com"));
    ASSERT_EQ(h.type_name, std::string("Private"));
    ASSERT_EQ(h.subject, std::string("=?UTF-8?Q?Integration_Test_=C3=A9?="));
    ASSERT_EQ(body, std::string("Line A\r\nLine B"));
    ASSERT_EQ(h.extraValue("Content-Type"), std::string("text/plain; charset=ISO-8859-1"));
    // The CMS took its turn (FF) and the client quit.
    ASSERT_EQ(log.after_transfer, std::string("FQ"));
    PASS();
}

TEST(send_with_wrong_password_fails) {
    FakeCmsConfig cfg;
    cfg.password = "Different";
    FakeCms cms{cfg};
    std::string error;
    const bool ok = sendWinlinkTelnetDelivery(sendRequest(cms.port), &error);
    cms.join();
    ASSERT_FALSE(ok);
    ASSERT_CONTAINS(error, "Secure login failed");
    ASSERT_TRUE(cms.log->received.find(kPassword) == std::string::npos);
    PASS();
}

TEST(send_deferred_by_cms_fails) {
    FakeCmsConfig cfg;
    cfg.answer = "=";
    FakeCms cms{cfg};
    std::string error;
    const bool ok = sendWinlinkTelnetDelivery(sendRequest(cms.port), &error);
    cms.join();
    ASSERT_FALSE(ok);
    ASSERT_CONTAINS(error, "deferred");
    PASS();
}

TEST(send_rejected_duplicate_counts_only_for_stable_id) {
    {
        FakeCmsConfig cfg;
        cfg.answer = "-";
        FakeCms cms{cfg};
        std::string error;
        ASSERT_FALSE(sendWinlinkTelnetDelivery(sendRequest(cms.port), &error));
        cms.join();
        ASSERT_CONTAINS(error, "rejected");
    }
    {
        FakeCmsConfig cfg;
        cfg.answer = "-";
        FakeCms cms{cfg};
        auto req = sendRequest(cms.port);
        req.message_id = "STABLE123456";
        std::string error;
        ASSERT_TRUE(sendWinlinkTelnetDelivery(req, &error));
        cms.join();
        ASSERT_EQ(cms.log->proposals.size(), size_t(1));
        ASSERT_TRUE(startsWith(cms.log->proposals[0], "FC EM STABLE123456 "));
    }
    PASS();
}

TEST(send_defers_inbound_offered_in_delivery_session) {
    FakeCmsConfig cfg;
    cfg.inbound.push_back({"INBOUND00001", inboundB2F("INBOUND00001", "Hi", "x")});
    FakeCms cms{cfg};
    std::string error;
    const bool ok = sendWinlinkTelnetDelivery(sendRequest(cms.port), &error);
    cms.join();
    ASSERT_TRUE(ok);
    ASSERT_EQ(cms.log->inbound_answer, std::string("FS ="));
    PASS();
}

TEST(send_without_challenge_sends_no_password) {
    FakeCmsConfig cfg;
    cfg.send_challenge = false;
    FakeCms cms{cfg};
    std::string error, mode;
    const bool ok = sendWinlinkTelnetDelivery(sendRequest(cms.port), &error, &mode);
    cms.join();
    ASSERT_TRUE(ok);
    ASSERT_EQ(mode, std::string("b2f"));
    ASSERT_TRUE(cms.log->pr.empty());
    ASSERT_TRUE(cms.log->received.find(kPassword) == std::string::npos);
    PASS();
}

// --- Inbound ----------------------------------------------------------------

TEST(fetch_receives_and_acknowledges_after_commit) {
    FakeCmsConfig cfg;
    cfg.inbound.push_back({"MSGONE000001", inboundB2F("MSGONE000001", "First", "Body one\r\n")});
    std::string long_body;
    for (int i = 0; i < 200; ++i) long_body += "Line " + std::to_string(i) + " of a longer message\r\n";
    cfg.inbound.push_back({"MSGTWO000002", inboundB2F("MSGTWO000002", "Second", long_body)});
    FakeCms cms{cfg};

    bool acked_before_commit = true;
    size_t seen = 0;
    auto result = fetchWinlinkTelnetMessages(fetchRequest(cms.port), [&](const WinlinkTelnetFetchResult& r) {
        acked_before_commit = cms.log->acked.load();
        seen = r.messages.size();
        return true;
    });
    cms.join();
    ASSERT_EQ(cms.log->error, std::string(""));
    ASSERT_TRUE(result.success);
    ASSERT_TRUE(result.error.empty());
    ASSERT_EQ(result.login_mode_used, std::string("b2f-secure-login"));
    ASSERT_EQ(seen, size_t(2));
    ASSERT_FALSE(acked_before_commit);
    ASSERT_TRUE(result.committed);
    ASSERT_TRUE(cms.log->acked.load());
    ASSERT_EQ(cms.log->inbound_answer, std::string("FS ++"));
    ASSERT_EQ(cms.log->after_transfer, std::string("FQ"));
    ASSERT_EQ(cms.log->pr, std::string(kExpectedResponse));
    ASSERT_TRUE(cms.log->received.find(kPassword) == std::string::npos);

    ASSERT_EQ(result.messages.size(), size_t(2));
    ASSERT_EQ(result.messages[0].message_id, std::string("MSGONE000001"));
    ASSERT_EQ(result.messages[0].from, std::string("sender@example.com"));
    ASSERT_EQ(result.messages[0].to, std::string("K1AAA"));
    ASSERT_EQ(result.messages[0].subject, std::string("First"));
    ASSERT_EQ(result.messages[0].body, std::string("Body one"));
    ASSERT_EQ(result.messages[1].subject, std::string("Second"));
    ASSERT_CONTAINS(result.messages[1].body, "Line 199 of a longer message");
    PASS();
}

TEST(fetch_refused_commit_leaves_messages_on_cms) {
    FakeCmsConfig cfg;
    cfg.inbound.push_back({"KEEPME000001", inboundB2F("KEEPME000001", "Keep", "Body")});
    FakeCms cms{cfg};
    size_t seen = 0;
    auto result = fetchWinlinkTelnetMessages(fetchRequest(cms.port), [&](const WinlinkTelnetFetchResult& r) {
        seen = r.messages.size();
        return false;  // not stored
    });
    cms.join();
    ASSERT_TRUE(result.success);
    ASSERT_EQ(seen, size_t(1));
    ASSERT_FALSE(result.committed);
    ASSERT_FALSE(cms.log->acked.load());
    ASSERT_TRUE(cms.log->after_transfer.empty());
    PASS();
}

TEST(fetch_without_delete_never_acknowledges) {
    FakeCmsConfig cfg;
    cfg.inbound.push_back({"NODELETE0001", inboundB2F("NODELETE0001", "Keep", "Body")});
    FakeCms cms{cfg};
    auto req = fetchRequest(cms.port);
    req.delete_after_fetch = false;
    auto result = fetchWinlinkTelnetMessages(req, [](const WinlinkTelnetFetchResult&) { return true; });
    cms.join();
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.messages.size(), size_t(1));
    ASSERT_FALSE(result.committed);
    ASSERT_FALSE(cms.log->acked.load());
    PASS();
}

TEST(fetch_bad_block_checksum_is_not_acknowledged) {
    FakeCmsConfig cfg;
    cfg.inbound.push_back({"BADSUM000001", inboundB2F("BADSUM000001", "Bad", "Body")});
    cfg.corrupt_block_checksum = true;
    FakeCms cms{cfg};
    bool gate_called = false;
    auto result = fetchWinlinkTelnetMessages(fetchRequest(cms.port), [&](const WinlinkTelnetFetchResult&) {
        gate_called = true;
        return true;
    });
    cms.join();
    ASSERT_FALSE(result.success);
    ASSERT_CONTAINS(result.error, "checksum");
    ASSERT_FALSE(gate_called);
    ASSERT_TRUE(result.messages.empty());
    ASSERT_FALSE(cms.log->acked.load());
    PASS();
}

TEST(fetch_corrupt_compressed_data_is_not_acknowledged) {
    FakeCmsConfig cfg;
    cfg.inbound.push_back({"BADLZH000001", inboundB2F("BADLZH000001", "Bad", "Body text here")});
    cfg.corrupt_lzh = true;
    FakeCms cms{cfg};
    bool gate_called = false;
    auto result = fetchWinlinkTelnetMessages(fetchRequest(cms.port), [&](const WinlinkTelnetFetchResult&) {
        gate_called = true;
        return true;
    });
    cms.join();
    ASSERT_FALSE(result.success);
    ASSERT_FALSE(gate_called);
    ASSERT_FALSE(cms.log->acked.load());
    PASS();
}

TEST(fetch_defers_beyond_max_messages) {
    FakeCmsConfig cfg;
    cfg.inbound.push_back({"LIMIT0000001", inboundB2F("LIMIT0000001", "One", "1")});
    cfg.inbound.push_back({"LIMIT0000002", inboundB2F("LIMIT0000002", "Two", "2")});
    FakeCms cms{cfg};
    auto req = fetchRequest(cms.port);
    req.max_messages = 1;
    auto result = fetchWinlinkTelnetMessages(req);
    cms.join();
    ASSERT_TRUE(result.success);
    ASSERT_EQ(result.messages.size(), size_t(1));
    ASSERT_EQ(result.deferred, 1);
    ASSERT_EQ(cms.log->inbound_answer, std::string("FS +="));
    ASSERT_TRUE(cms.log->acked.load());
    PASS();
}

TEST(fetch_stop_request_defers_everything) {
    FakeCmsConfig cfg;
    cfg.inbound.push_back({"STOP00000001", inboundB2F("STOP00000001", "One", "1")});
    FakeCms cms{cfg};
    std::atomic<bool> stop{true};
    auto req = fetchRequest(cms.port);
    req.stop = &stop;
    auto result = fetchWinlinkTelnetMessages(req);
    cms.join();
    ASSERT_TRUE(result.success);
    ASSERT_TRUE(result.messages.empty());
    ASSERT_EQ(cms.log->inbound_answer, std::string("FS ="));
    PASS();
}

TEST(fetch_with_no_mail) {
    FakeCms cms{FakeCmsConfig{}};
    int gate_calls = 0;
    auto result = fetchWinlinkTelnetMessages(fetchRequest(cms.port), [&](const WinlinkTelnetFetchResult&) {
        ++gate_calls;
        return true;
    });
    cms.join();
    ASSERT_TRUE(result.success);
    ASSERT_TRUE(result.messages.empty());
    ASSERT_EQ(gate_calls, 1);
    ASSERT_TRUE(cms.log->proposals.empty());
    ASSERT_EQ(cms.log->after_transfer, std::string("FQ"));
    PASS();
}

TEST(fetch_wrong_password_fails_without_messages) {
    FakeCmsConfig cfg;
    cfg.password = "Different";
    cfg.inbound.push_back({"SECRET000001", inboundB2F("SECRET000001", "x", "y")});
    FakeCms cms{cfg};
    auto result = fetchWinlinkTelnetMessages(fetchRequest(cms.port));
    cms.join();
    ASSERT_FALSE(result.success);
    ASSERT_CONTAINS(result.error, "Secure login failed");
    ASSERT_TRUE(result.messages.empty());
    PASS();
}

// A server that accepts but never says anything cannot hold the fetch.
TEST(fetch_silent_server_times_out) {
    std::promise<uint16_t> port_promise;
    auto port_future = port_promise.get_future();
    std::thread server([p = std::move(port_promise)]() mutable {
        const int ls = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(addr);
        ::bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(ls, 1);
        ::getsockname(ls, reinterpret_cast<sockaddr*>(&addr), &len);
        p.set_value(ntohs(addr.sin_port));
        const int s = ::accept(ls, nullptr, nullptr);
        ::close(ls);
        char c;
        while (s >= 0 && ::recv(s, &c, 1, 0) > 0) {
        }
        if (s >= 0) ::close(s);
    });
    auto req = fetchRequest(port_future.get());
    req.timeout_ms = 1000;
    const auto t0 = std::chrono::steady_clock::now();
    auto result = fetchWinlinkTelnetMessages(req);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    server.join();
    ASSERT_FALSE(result.success);
    ASSERT_TRUE(elapsed < std::chrono::seconds(8));
    PASS();
}

#endif  // !_WIN32

TEST_MAIN()
