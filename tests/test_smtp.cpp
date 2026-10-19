// SMTP client tests - TLS policy (fail closed) and socket timeouts

#include "test_macros.hpp"
#include "skywave_link/smtp_client.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <thread>

using namespace skylink;

namespace {

// Minimal plaintext SMTP server on loopback. It never offers STARTTLS.
// With silent=true it accepts the connection and never sends a greeting.
struct FakeSmtp {
    int listen_fd = -1;
    uint16_t port = 0;
    std::thread th;
    std::vector<std::string> commands;
    std::vector<std::string> data_lines;

    bool start(bool silent) {
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) return false;
        int one = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        if (listen(listen_fd, 1) != 0) return false;
        socklen_t len = sizeof(addr);
        getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
        th = std::thread([this, silent]() {
            int fd = accept(listen_fd, nullptr, nullptr);
            if (fd < 0) return;
            auto say = [fd](const std::string& s) { send(fd, s.data(), s.size(), 0); };
            if (silent) {
                // Hold the connection open without speaking until the client
                // gives up (its recv timeout fires and it closes).
                char ch;
                while (recv(fd, &ch, 1, 0) > 0) {}
                close(fd);
                return;
            }
            say("220 fake ESMTP\r\n");
            std::string buf;
            bool in_data = false;
            char ch;
            while (recv(fd, &ch, 1, 0) == 1) {
                if (ch != '\n') { buf += ch; continue; }
                if (!buf.empty() && buf.back() == '\r') buf.pop_back();
                std::string cmd = buf;
                buf.clear();
                if (in_data) {
                    if (cmd == ".") { in_data = false; say("250 queued\r\n"); }
                    else data_lines.push_back(cmd);
                    continue;
                }
                commands.push_back(cmd);
                if (cmd.rfind("EHLO", 0) == 0) {
                    say("250-fake\r\n250-AUTH LOGIN\r\n250 8BITMIME\r\n");
                } else if (cmd == "AUTH LOGIN") {
                    say("334 VXNlcm5hbWU6\r\n");
                } else if (cmd.rfind("MAIL", 0) == 0 || cmd.rfind("RCPT", 0) == 0) {
                    say("250 ok\r\n");
                } else if (cmd == "DATA") {
                    in_data = true;
                    say("354 go\r\n");
                } else if (cmd == "QUIT") {
                    say("221 bye\r\n");
                    break;
                } else if (commands.size() >= 2 &&
                           commands[commands.size() - 2] == "AUTH LOGIN") {
                    say("334 UGFzc3dvcmQ6\r\n");
                } else if (commands.size() >= 3 &&
                           commands[commands.size() - 3] == "AUTH LOGIN") {
                    say("235 ok\r\n");
                } else {
                    say("502 unknown\r\n");
                }
            }
            close(fd);
        });
        return true;
    }

    void finish() {
        if (th.joinable()) th.join();
        if (listen_fd >= 0) close(listen_fd);
    }

    bool sawPrefix(const std::string& p) const {
        for (const auto& c : commands) if (c.rfind(p, 0) == 0) return true;
        return false;
    }
};

SMTPDeliveryRequest localRequest(uint16_t port, bool use_tls) {
    SMTPDeliveryRequest req;
    req.host = "127.0.0.1";
    req.port = port;
    req.use_tls = use_tls;
    req.username = "user";
    req.password = "secret";
    req.from = "a@example.com";
    req.to = "b@example.com";
    req.subject = "s";
    req.body = "b";
    req.timeout_sec = 1;
    return req;
}

} // namespace

TEST(smtp_tls_required_without_starttls_fails_closed) {
    FakeSmtp srv;
    ASSERT_TRUE(srv.start(false));
    std::string err;
    // Non-587 port with TLS requested must still require STARTTLS.
    bool ok = sendSMTPDelivery(localRequest(srv.port, true), &err);
    srv.finish();
    ASSERT_FALSE(ok);
    ASSERT_CONTAINS(err, "STARTTLS");
    ASSERT_FALSE(srv.sawPrefix("AUTH"));
    ASSERT_FALSE(srv.sawPrefix("MAIL"));
    PASS();
}

TEST(smtp_plain_when_tls_disabled) {
    FakeSmtp srv;
    ASSERT_TRUE(srv.start(false));
    std::string err;
    bool ok = sendSMTPDelivery(localRequest(srv.port, false), &err);
    srv.finish();
    ASSERT_TRUE(ok);
    ASSERT_TRUE(srv.sawPrefix("AUTH LOGIN"));
    ASSERT_TRUE(srv.sawPrefix("MAIL FROM"));
    PASS();
}

// No credentials configured: EHLO, no AUTH, and the mail still goes out
// (send() used to require a successful AUTH).
TEST(smtp_without_credentials_sends) {
    FakeSmtp srv;
    ASSERT_TRUE(srv.start(false));
    std::string err;
    auto req = localRequest(srv.port, false);
    req.username.clear();
    req.password.clear();
    bool ok = sendSMTPDelivery(req, &err);
    srv.finish();
    ASSERT_TRUE(ok);
    ASSERT_FALSE(srv.sawPrefix("AUTH"));
    ASSERT_TRUE(srv.sawPrefix("MAIL FROM"));
    PASS();
}

TEST(smtp_stalled_server_times_out) {
    FakeSmtp srv;
    ASSERT_TRUE(srv.start(true));
    auto t0 = std::chrono::steady_clock::now();
    std::string err;
    bool ok = sendSMTPDelivery(localRequest(srv.port, false), &err);
    auto elapsed = std::chrono::steady_clock::now() - t0;
    srv.finish();
    ASSERT_FALSE(ok);
    ASSERT_LT(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(), 10);
    PASS();
}
// K20: station.name is the From display name; the envelope sender stays
// the bare address.
TEST(smtp_from_display_name) {
    FakeSmtp srv;
    ASSERT_TRUE(srv.start(false));
    auto req = localRequest(srv.port, false);
    req.from_name = "Test \"Op\" Station";
    std::string err;
    ASSERT_TRUE(sendSMTPDelivery(req, &err));
    srv.finish();
    ASSERT_TRUE(srv.sawPrefix("MAIL FROM:<a@example.com>"));
    bool saw = false;
    for (const auto& l : srv.data_lines) {
        if (l == "From: \"Test \\\"Op\\\" Station\" <a@example.com>") saw = true;
    }
    ASSERT_TRUE(saw);
    PASS();
}
#endif

TEST(smtp_header_encoding) {
    ASSERT_EQ(formatFromHeader("", "a@example.com"), std::string("a@example.com"));
    ASSERT_EQ(formatFromHeader("  Op Name ", "a@example.com"),
              std::string("\"Op Name\" <a@example.com>"));
    // Non-ASCII: an RFC 2047 encoded word ("Jos\xC3\xA9" = "José").
    ASSERT_EQ(formatFromHeader("Jos\xC3\xA9", "a@example.com"),
              std::string("=?UTF-8?B?Sm9zw6k=?= <a@example.com>"));
    ASSERT_EQ(encodeHeaderText("plain"), std::string("plain"));
    ASSERT_EQ(encodeHeaderText("a\r\nBcc: x"), std::string("aBcc: x"));
    PASS();
}

TEST_MAIN()
