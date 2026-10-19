// POP3 client tests - message parsing, dot-unstuffing, header folding

#include "test_macros.hpp"
#include "skywave_link/pop3_client.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#endif

using namespace skylink;

TEST(parse_simple_message) {
    std::string raw =
        "From: user@example.com\r\n"
        "To: dest@example.com\r\n"
        "Subject: Test\r\n"
        "\r\n"
        "Hello, world!";

    auto msg = parseEmailMessage(raw);
    ASSERT_EQ(msg.from, std::string("user@example.com"));
    ASSERT_EQ(msg.to, std::string("dest@example.com"));
    ASSERT_EQ(msg.subject, std::string("Test"));
    ASSERT_EQ(msg.body, std::string("Hello, world!"));
    PASS();
}

TEST(parse_lf_only) {
    std::string raw =
        "From: alice@test.com\n"
        "To: bob@test.com\n"
        "Subject: LF test\n"
        "\n"
        "Line 1\nLine 2";

    auto msg = parseEmailMessage(raw);
    ASSERT_EQ(msg.from, std::string("alice@test.com"));
    ASSERT_EQ(msg.subject, std::string("LF test"));
    ASSERT_EQ(msg.body, std::string("Line 1\nLine 2"));
    PASS();
}

TEST(parse_folded_headers) {
    // RFC 5322 folded header: continuation line starts with whitespace
    std::string raw =
        "From: sender@example.com\r\n"
        "Subject: This is a very long\r\n"
        " subject line that was folded\r\n"
        "To: receiver@example.com\r\n"
        "\r\n"
        "Body here.";

    auto msg = parseEmailMessage(raw);
    ASSERT_EQ(msg.subject, std::string("This is a very long subject line that was folded"));
    ASSERT_EQ(msg.from, std::string("sender@example.com"));
    ASSERT_EQ(msg.to, std::string("receiver@example.com"));
    ASSERT_EQ(msg.body, std::string("Body here."));
    PASS();
}

TEST(parse_empty_body) {
    std::string raw =
        "From: a@b.com\r\n"
        "To: c@d.com\r\n"
        "Subject: No body\r\n"
        "\r\n";

    auto msg = parseEmailMessage(raw);
    ASSERT_EQ(msg.from, std::string("a@b.com"));
    ASSERT_EQ(msg.subject, std::string("No body"));
    ASSERT_TRUE(msg.body.empty());
    PASS();
}

TEST(parse_no_headers_match) {
    std::string raw =
        "X-Custom: value\r\n"
        "Date: Mon, 1 Jan 2024\r\n"
        "\r\n"
        "Just body.";

    auto msg = parseEmailMessage(raw);
    ASSERT_TRUE(msg.from.empty());
    ASSERT_TRUE(msg.to.empty());
    ASSERT_TRUE(msg.subject.empty());
    ASSERT_EQ(msg.body, std::string("Just body."));
    PASS();
}

TEST(parse_case_insensitive_headers) {
    std::string raw =
        "FROM: Upper@case.com\r\n"
        "TO: Another@case.com\r\n"
        "SUBJECT: Caps\r\n"
        "\r\n"
        "Case test.";

    auto msg = parseEmailMessage(raw);
    ASSERT_EQ(msg.from, std::string("Upper@case.com"));
    ASSERT_EQ(msg.to, std::string("Another@case.com"));
    ASSERT_EQ(msg.subject, std::string("Caps"));
    PASS();
}

TEST(parse_multipart_body) {
    // Body with MIME boundaries - we just return the raw body
    std::string raw =
        "From: test@test.com\r\n"
        "Subject: Multipart\r\n"
        "Content-Type: multipart/mixed; boundary=\"----=_Part_123\"\r\n"
        "\r\n"
        "------=_Part_123\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Text part\r\n"
        "------=_Part_123--";

    auto msg = parseEmailMessage(raw);
    ASSERT_EQ(msg.from, std::string("test@test.com"));
    // Body should contain the full MIME body
    ASSERT_TRUE(msg.body.find("Text part") != std::string::npos);
    PASS();
}

TEST(parse_malformed_no_blank_line) {
    // No blank line separating headers from body
    std::string raw = "From: broken@test.com\r\nSubject: No separator";

    auto msg = parseEmailMessage(raw);
    // Should treat entire thing as headers
    ASSERT_EQ(msg.from, std::string("broken@test.com"));
    ASSERT_EQ(msg.subject, std::string("No separator"));
    ASSERT_TRUE(msg.body.empty());
    PASS();
}

TEST(parse_raw_preserved) {
    std::string raw = "From: x@y.com\r\n\r\nHello";
    auto msg = parseEmailMessage(raw);
    ASSERT_EQ(msg.raw, raw);
    PASS();
}

TEST(fetch_request_defaults) {
    POP3FetchRequest req;
    ASSERT_EQ(req.port, uint16_t(995));
    ASSERT_TRUE(req.use_tls);
    ASSERT_FALSE(req.delete_after_fetch);
    ASSERT_EQ(req.max_messages, 50);
    PASS();
}

TEST(dot_unstuffing_basic) {
    // Lines starting with ".." should have leading dot removed
    std::string input = "..This line was dot-stuffed\r\nNormal line\r\n..Another stuffed";
    auto result = dotUnstuff(input);
    ASSERT_EQ(result, std::string(".This line was dot-stuffed\r\nNormal line\r\n.Another stuffed"));
    PASS();
}

TEST(dot_unstuffing_no_dots) {
    std::string input = "No dots here\r\nJust text";
    auto result = dotUnstuff(input);
    ASSERT_EQ(result, std::string("No dots here\r\nJust text"));
    PASS();
}

TEST(dot_unstuffing_empty) {
    ASSERT_EQ(dotUnstuff(""), std::string(""));
    PASS();
}

TEST(dot_unstuffing_multiple_consecutive) {
    // "..." at start of line → ".." after unstuffing
    std::string input = "...triple dots";
    auto result = dotUnstuff(input);
    ASSERT_EQ(result, std::string("..triple dots"));
    PASS();
}

TEST(dot_unstuffing_mixed_line_endings) {
    // Mixed CRLF and LF line endings with dot-stuffing
    std::string input = "..Line1\r\nNormal\n..Line3\r\n..Line4";
    auto result = dotUnstuff(input);
    ASSERT_EQ(result, std::string(".Line1\r\nNormal\n.Line3\r\n.Line4"));
    PASS();
}

TEST(dot_unstuffing_single_dot_line) {
    // A line with just ".." should become "."
    std::string input = "before\r\n..\r\nafter";
    auto result = dotUnstuff(input);
    ASSERT_EQ(result, std::string("before\r\n.\r\nafter"));
    PASS();
}

TEST(fetch_empty_host_fails) {
    POP3FetchRequest req;
    req.host = "";
    auto result = fetchPOP3Messages(req);
    ASSERT_FALSE(result.success);
    ASSERT_TRUE(result.error.find("not configured") != std::string::npos);
    PASS();
}

#ifndef _WIN32
// ---------------------------------------------------------------------------
// Fake POP3 server (plain TCP on loopback) for end-to-end fetch tests.
// The handler receives each command line and returns what to send back; an
// empty "close" reply closes the connection.
namespace {

struct FakePop3 {
    int listen_fd = -1;
    uint16_t port = 0;
    std::thread th;
    std::vector<std::string> commands;  // commands seen (after the session)

    // big_body: size in bytes of message 2 (0 -> normal); close_mid_retr2:
    // server drops the connection in the middle of RETR 2.
    bool start(bool close_mid_retr2, size_t big_body) {
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) return false;
        int one = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        if (listen(listen_fd, 1) != 0) return false;
        socklen_t len = sizeof(addr);
        getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
        th = std::thread([this, close_mid_retr2, big_body]() {
            int fd = accept(listen_fd, nullptr, nullptr);
            if (fd < 0) return;
            auto sendAll = [fd](const std::string& s) {
                size_t off = 0;
                while (off < s.size()) {
                    ssize_t n = send(fd, s.data() + off, s.size() - off, 0);
                    if (n <= 0) return;
                    off += static_cast<size_t>(n);
                }
            };
            sendAll("+OK ready\r\n");
            std::string buf;
            char ch;
            while (recv(fd, &ch, 1, 0) == 1) {
                if (ch != '\n') { buf += ch; continue; }
                if (!buf.empty() && buf.back() == '\r') buf.pop_back();
                std::string cmd = buf;
                buf.clear();
                commands.push_back(cmd);
                if (cmd.rfind("USER", 0) == 0 || cmd.rfind("PASS", 0) == 0) {
                    sendAll("+OK\r\n");
                } else if (cmd == "STAT") {
                    sendAll("+OK 3 300\r\n");
                } else if (cmd == "RETR 1" || cmd == "RETR 3") {
                    sendAll("+OK\r\nFrom: a@b\r\nSubject: one\r\n\r\nbody\r\n.\r\n");
                } else if (cmd == "RETR 2") {
                    if (close_mid_retr2) {
                        sendAll("+OK\r\nFrom: a@b\r\nSubject: two\r\n\r\npartial bo");
                        break;  // drop the connection mid-message
                    }
                    std::string msg = "+OK\r\nFrom: a@b\r\nSubject: big\r\n\r\n";
                    std::string line(1000, 'x');
                    for (size_t n = 0; n < big_body; n += line.size() + 2) {
                        msg += line + "\r\n";
                    }
                    msg += ".\r\n";
                    sendAll(msg);
                } else if (cmd.rfind("DELE", 0) == 0) {
                    sendAll("+OK\r\n");
                } else if (cmd == "QUIT") {
                    sendAll("+OK bye\r\n");
                    break;
                } else {
                    sendAll("-ERR\r\n");
                }
            }
            close(fd);
        });
        return true;
    }

    void finish() {
        if (th.joinable()) th.join();
        if (listen_fd >= 0) close(listen_fd);
        listen_fd = -1;
    }

    bool saw(const std::string& cmd) const {
        for (const auto& c : commands) if (c == cmd) return true;
        return false;
    }
};

POP3FetchRequest localRequest(uint16_t port) {
    POP3FetchRequest req;
    req.host = "127.0.0.1";
    req.port = port;
    req.use_tls = false;
    req.username = "u";
    req.password = "p";
    req.delete_after_fetch = true;
    req.max_messages = 10;
    return req;
}

} // namespace

TEST(fetch_server_close_mid_retr_terminates_and_skips_dele) {
    FakePop3 srv;
    ASSERT_TRUE(srv.start(/*close_mid_retr2=*/true, 0));
    std::atomic<bool> done{false};
    POP3FetchResult result;
    std::thread worker([&]() {
        result = fetchPOP3Messages(localRequest(srv.port));
        done = true;
    });
    for (int i = 0; i < 100 && !done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!done) {
        // Would spin forever before the fix; fail loudly rather than hang.
        std::cerr << "  FAIL fetch did not terminate after server EOF\n";
        std::_Exit(1);
    }
    worker.join();
    srv.finish();
    ASSERT_EQ(result.messages.size(), size_t(1));
    ASSERT_EQ(result.messages[0].subject, std::string("one"));
    ASSERT_FALSE(result.messages[0].truncated);
    ASSERT_TRUE(srv.saw("DELE 1"));
    ASSERT_FALSE(srv.saw("DELE 2"));
    ASSERT_FALSE(srv.saw("RETR 3"));
    ASSERT_CONTAINS(result.error, "msg 2");
    PASS();
}

TEST(fetch_oversized_message_not_deleted) {
    FakePop3 srv;
    ASSERT_TRUE(srv.start(false, 1100 * 1024));  // > 1 MB cap
    auto result = fetchPOP3Messages(localRequest(srv.port));
    srv.finish();
    ASSERT_EQ(result.messages.size(), size_t(3));
    ASSERT_FALSE(result.messages[0].truncated);
    ASSERT_TRUE(result.messages[1].truncated);
    ASSERT_FALSE(result.messages[2].truncated);
    ASSERT_TRUE(srv.saw("DELE 1"));
    ASSERT_FALSE(srv.saw("DELE 2"));
    ASSERT_TRUE(srv.saw("DELE 3"));  // protocol stayed in sync after the cap
    PASS();
}
// RFC 1939: DELE only marks; the server removes messages when QUIT moves the
// session to UPDATE. The commit gate sees the messages before QUIT, and a
// refused commit ends the session without QUIT so the server keeps them.
TEST(fetch_commit_gate_controls_quit) {
    {
        FakePop3 srv;
        ASSERT_TRUE(srv.start(false, 0));
        size_t messages_at_commit = 0;
        bool quit_before_commit = true;
        auto result = fetchPOP3Messages(localRequest(srv.port), [&](const POP3FetchResult& r) {
            messages_at_commit = r.messages.size();
            quit_before_commit = srv.saw("QUIT");
            return false;
        });
        srv.finish();
        ASSERT_EQ(messages_at_commit, size_t(3));
        ASSERT_FALSE(quit_before_commit);
        ASSERT_TRUE(srv.saw("DELE 1"));      // marked ...
        ASSERT_FALSE(srv.saw("QUIT"));       // ... but never committed
        ASSERT_FALSE(result.committed);
        ASSERT_EQ(result.messages.size(), size_t(3));
    }
    {
        FakePop3 srv;
        ASSERT_TRUE(srv.start(false, 0));
        auto result = fetchPOP3Messages(localRequest(srv.port),
                                        [](const POP3FetchResult&) { return true; });
        srv.finish();
        ASSERT_TRUE(srv.saw("QUIT"));
        ASSERT_TRUE(result.committed);
    }
    PASS();
}

// A server that accepts but never sends a greeting cannot hold the fetch.
TEST(fetch_silent_server_times_out) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(listen(lfd, 1), 0);
    socklen_t len = sizeof(addr);
    getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &len);
    POP3FetchRequest req = localRequest(ntohs(addr.sin_port));
    req.timeout_ms = 1000;
    const auto t0 = std::chrono::steady_clock::now();
    auto result = fetchPOP3Messages(req);  // connect completes via the backlog
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    close(lfd);
    ASSERT_FALSE(result.success);
    ASSERT_FALSE(result.error.empty());
    ASSERT_TRUE(elapsed < std::chrono::seconds(5));
    PASS();
}

TEST(parse_message_id_header) {
    auto msg = parseEmailMessage("From: a@b\r\nMessage-ID: <abc@example.com>\r\n\r\nbody");
    ASSERT_EQ(msg.message_id, std::string("<abc@example.com>"));
    PASS();
}
#endif

TEST_MAIN()
