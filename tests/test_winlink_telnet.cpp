// Winlink Telnet helper tests - parsing, validation, and mocked transcript flows

#include "test_macros.hpp"
#include "skywave_link/winlink_telnet_client.hpp"

#include <algorithm>
#include <future>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace skylink;

namespace {

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

#ifndef _WIN32
bool sendAll(int sock, const std::string& data) {
    size_t offset = 0;
    while (offset < data.size()) {
        int send_flags = 0;
#ifdef MSG_NOSIGNAL
        send_flags = MSG_NOSIGNAL;
#endif
        int written = static_cast<int>(::send(sock,
                                              data.data() + offset,
                                              static_cast<int>(data.size() - offset),
                                              send_flags));
        if (written <= 0) {
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool sendLine(int sock, const std::string& line) {
    return sendAll(sock, line + "\r\n");
}

bool recvLine(int sock, std::string* out) {
    out->clear();
    while (out->size() < 4096) {
        char c = '\0';
        int n = static_cast<int>(::recv(sock, &c, 1, 0));
        if (n <= 0) {
            return false;
        }
        if (c == '\n') {
            return true;
        }
        if (c != '\r') {
            out->push_back(c);
        }
    }
    return false;
}

template <typename ScriptFn>
struct MockServerRun {
    uint16_t port = 0;
    std::vector<std::string> lines;
    std::string error;
    std::thread thread;

    MockServerRun() = default;
    MockServerRun(const MockServerRun&) = delete;
    MockServerRun& operator=(const MockServerRun&) = delete;
    MockServerRun(MockServerRun&& other) noexcept
        : port(other.port),
          lines(std::move(other.lines)),
          error(std::move(other.error)),
          thread(std::move(other.thread)) {}
    MockServerRun& operator=(MockServerRun&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        if (thread.joinable()) {
            thread.join();
        }
        port = other.port;
        lines = std::move(other.lines);
        error = std::move(other.error);
        thread = std::move(other.thread);
        return *this;
    }

    ~MockServerRun() {
        if (thread.joinable()) {
            thread.join();
        }
    }
};

template <typename ScriptFn>
MockServerRun<ScriptFn> startMockServer(ScriptFn script) {
    MockServerRun<ScriptFn> run;
    std::promise<uint16_t> port_promise;
    std::future<uint16_t> port_future = port_promise.get_future();

    run.thread = std::thread([&run, script, p = std::move(port_promise)]() mutable {
        int listen_sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_sock < 0) {
            run.error = "listen socket create failed";
            p.set_value(0);
            return;
        }

        int opt = 1;
        (void)setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            run.error = "bind failed";
            ::close(listen_sock);
            p.set_value(0);
            return;
        }
        if (::listen(listen_sock, 1) != 0) {
            run.error = "listen failed";
            ::close(listen_sock);
            p.set_value(0);
            return;
        }

        socklen_t len = sizeof(addr);
        if (::getsockname(listen_sock, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            run.error = "getsockname failed";
            ::close(listen_sock);
            p.set_value(0);
            return;
        }
        p.set_value(ntohs(addr.sin_port));

        int client = ::accept(listen_sock, nullptr, nullptr);
        ::close(listen_sock);
        if (client < 0) {
            if (run.error.empty()) run.error = "accept failed";
            return;
        }

        timeval tv {};
        tv.tv_sec = 8;
        tv.tv_usec = 0;
        (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
        const int no_sigpipe = 1;
        (void)setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif

        script(client, run.lines, run.error);
        ::close(client);
    });

    run.port = port_future.get();
    return run;
}
#endif

}  // namespace

TEST(extract_challenge_bracket_format) {
    const std::string token = extractWinlinkChallengeToken("Password [ABCD1234]:");
    ASSERT_EQ(token, std::string("ABCD1234"));
    PASS();
}

TEST(extract_challenge_named_format) {
    const std::string token = extractWinlinkChallengeToken("Password (challenge: ZXCV99):");
    ASSERT_EQ(token, std::string("ZXCV99"));
    PASS();
}

TEST(challenge_response_with_token) {
    const std::string response = buildWinlinkChallengeResponse("secret", "TOKEN7");
    ASSERT_EQ(response, std::string("secret TOKEN7"));
    PASS();
}

TEST(challenge_response_without_token) {
    const std::string response = buildWinlinkChallengeResponse("secret", "");
    ASSERT_EQ(response, std::string("secret"));
    PASS();
}

TEST(parse_inbound_message_headers_and_body) {
    std::string raw =
        "From: N0CALL\r\n"
        "To: K1ABC@radio\r\n"
        "Subject: Test inbound\r\n"
        "MsgId: MID-123\r\n"
        "\r\n"
        "Line one\r\n"
        "Line two\r\n";

    auto msg = parseWinlinkInboundMessage(raw);
    ASSERT_EQ(msg.from, std::string("N0CALL"));
    ASSERT_EQ(msg.to, std::string("K1ABC@radio"));
    ASSERT_EQ(msg.subject, std::string("Test inbound"));
    ASSERT_EQ(msg.message_id, std::string("MID-123"));
    ASSERT_TRUE(msg.body.find("Line one") != std::string::npos);
    ASSERT_TRUE(msg.body.find("Line two") != std::string::npos);
    PASS();
}

TEST(parse_inbound_message_body_fallback) {
    const std::string raw = "No structured headers in this payload";
    auto msg = parseWinlinkInboundMessage(raw);
    ASSERT_EQ(msg.body, raw);
    PASS();
}

TEST(parse_inbound_message_without_blank_separator_keeps_first_body_line) {
    std::string raw =
        "From: N0CALL\r\n"
        "To: K1ABC@radio\r\n"
        "Subject: Missing blank line\r\n"
        "MsgId: MID-900\r\n"
        "Body line one\r\n"
        "Body line two\r\n";

    auto msg = parseWinlinkInboundMessage(raw);
    ASSERT_EQ(msg.from, std::string("N0CALL"));
    ASSERT_EQ(msg.subject, std::string("Missing blank line"));
    ASSERT_EQ(msg.message_id, std::string("MID-900"));
    ASSERT_TRUE(msg.body.find("Body line one") == 0);
    ASSERT_TRUE(msg.body.find("Body line two") != std::string::npos);
    PASS();
}

TEST(send_delivery_empty_destination_fails_fast) {
    WinlinkTelnetRequest req;
    req.to = "";
    std::string error;
    const bool ok = sendWinlinkTelnetDelivery(req, &error);
    ASSERT_FALSE(ok);
    ASSERT_TRUE(error.find("destination") != std::string::npos);
    PASS();
}

TEST(fetch_invalid_max_messages_fails_fast) {
    WinlinkTelnetFetchRequest req;
    req.max_messages = 0;
    auto result = fetchWinlinkTelnetMessages(req);
    ASSERT_FALSE(result.success);
    ASSERT_TRUE(result.error.find("max_messages") != std::string::npos);
    PASS();
}

TEST(mock_transcript_send_delivery) {
#ifdef _WIN32
    PASS();
#else
    auto run = startMockServer([](int client, std::vector<std::string>& lines, std::string& error) {
        if (!sendLine(client, "Callsign:")) {
            error = "failed to send callsign prompt";
            return;
        }
        std::string line;
        if (!recvLine(client, &line)) { error = "missing callsign"; return; }
        lines.push_back(line);

        if (!sendLine(client, "Password:")) {
            error = "failed to send password prompt";
            return;
        }
        if (!recvLine(client, &line)) { error = "missing password"; return; }
        lines.push_back(line);

        if (!sendAll(client, "Welcome\r\n>\r\n")) { error = "failed to send welcome"; return; }

        bool have_subject = false;
        while (!have_subject) {
            if (!recvLine(client, &line)) { error = "missing address/subject line"; return; }
            lines.push_back(line);
            if (startsWith(line, "SP ") || startsWith(line, "SM ") || startsWith(line, "SEND ")) {
                if (!sendLine(client, "Subject:")) { error = "failed to send subject prompt"; return; }
                continue;
            }
            have_subject = true;
        }

        if (!sendLine(client, "Enter message, end with /EX")) { error = "failed to send body prompt"; return; }
        while (true) {
            if (!recvLine(client, &line)) { error = "missing body/ex"; return; }
            lines.push_back(line);
            if (line == "/EX") {
                break;
            }
        }

        (void)sendAll(client, "Queued\r\n>\r\n");
        (void)recvLine(client, &line);  // Optional BYE
    });

    ASSERT_GT(static_cast<int>(run.port), 0);

    WinlinkTelnetRequest req;
    req.host = "127.0.0.1";
    req.port = run.port;
    req.use_tls = false;
    req.timeout_ms = 8000;
    req.callsign = "N0CALL";
    req.password = "secret";
    req.to = "K1ABC";
    req.subject = "Integration Test";
    req.body = "Line A\nLine B";

    std::string error;
    std::string login_mode;
    int attempts = 0;
    const bool ok = sendWinlinkTelnetDelivery(req, &error, &login_mode, &attempts);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(error.empty());
    ASSERT_EQ(login_mode, std::string("auto-interactive"));
    ASSERT_EQ(attempts, 1);
    ASSERT_TRUE(run.error.empty());
    ASSERT_TRUE(run.lines.size() >= 5);
    ASSERT_EQ(run.lines[0], std::string("N0CALL"));
    ASSERT_EQ(run.lines[1], std::string("secret"));
    ASSERT_TRUE(std::any_of(run.lines.begin(), run.lines.end(), [](const std::string& line) {
        return startsWith(line, "SP ") || startsWith(line, "SM ") || startsWith(line, "SEND ");
    }));
    ASSERT_TRUE(std::any_of(run.lines.begin(), run.lines.end(), [](const std::string& line) {
        return line == "Integration Test";
    }));
    ASSERT_TRUE(std::any_of(run.lines.begin(), run.lines.end(), [](const std::string& line) {
        return line == "/EX";
    }));
    PASS();
#endif
}

TEST(mock_transcript_fetch_messages) {
#ifdef _WIN32
    PASS();
#else
    auto run = startMockServer([](int client, std::vector<std::string>& lines, std::string& error) {
        if (!sendLine(client, "Callsign:")) { error = "failed callsign prompt"; return; }
        std::string line;
        if (!recvLine(client, &line)) { error = "missing callsign"; return; }
        lines.push_back(line);
        if (!sendLine(client, "Password:")) { error = "failed password prompt"; return; }
        if (!recvLine(client, &line)) { error = "missing password"; return; }
        lines.push_back(line);
        if (!sendAll(client, "Welcome\r\n>\r\n")) { error = "failed welcome"; return; }

        bool saw_read = false;
        bool done = false;
        while (!done) {
            if (!recvLine(client, &line)) {
                // Client may close immediately after processing fetched/deleted mail.
                if (saw_read) {
                    break;
                }
                error = "missing client command";
                return;
            }
            lines.push_back(line);
            if (startsWith(line, "LM") || startsWith(line, "L ") || line == "L" || startsWith(line, "LIST")) {
                if (!sendAll(client, "1 TEST Inbox item\r\n>\r\n")) {
                    error = "failed list response";
                    return;
                }
                continue;
            }
            if (startsWith(line, "R ") || startsWith(line, "RM ") || startsWith(line, "READ ")) {
                saw_read = true;
                if (!sendAll(client,
                             "From: TEST\r\n"
                             "To: K1ABC@radio\r\n"
                             "Subject: Mock Fetch\r\n"
                             "MsgId: MID-42\r\n"
                             "\r\n"
                             "Body A\r\n"
                             "Body B\r\n"
                             ">\r\n")) {
                    error = "failed read response";
                    return;
                }
                continue;
            }
            if (startsWith(line, "K ") || startsWith(line, "KM ") ||
                startsWith(line, "DEL ") || startsWith(line, "DELETE ")) {
                if (!sendAll(client, "Deleted\r\n>\r\n")) {
                    error = "failed delete ack";
                    return;
                }
                continue;
            }
            if (startsWith(line, "BYE")) {
                done = true;
                break;
            }
        }
        if (!saw_read) {
            error = "read command never received";
        }
    });

    ASSERT_GT(static_cast<int>(run.port), 0);

    WinlinkTelnetFetchRequest req;
    req.host = "127.0.0.1";
    req.port = run.port;
    req.use_tls = false;
    req.timeout_ms = 8000;
    req.callsign = "N0CALL";
    req.password = "secret";
    req.max_messages = 3;
    req.delete_after_fetch = true;

    WinlinkTelnetFetchResult result = fetchWinlinkTelnetMessages(req);

    ASSERT_TRUE(result.success);
    ASSERT_TRUE(result.error.empty());
    ASSERT_EQ(result.login_mode_used, std::string("auto-interactive"));
    ASSERT_EQ(result.login_attempts, 1);
    ASSERT_EQ(result.messages.size(), static_cast<size_t>(1));
    ASSERT_TRUE(result.messages[0].message_id == "MID-42" ||
                result.messages[0].message_id == "1");
    ASSERT_EQ(result.messages[0].from, std::string("TEST"));
    ASSERT_EQ(result.messages[0].to, std::string("K1ABC@radio"));
    ASSERT_EQ(result.messages[0].subject, std::string("Mock Fetch"));
    ASSERT_TRUE(result.messages[0].body.find("Body A") != std::string::npos);
    ASSERT_TRUE(result.messages[0].body.find("Body B") != std::string::npos);
    ASSERT_TRUE(run.error.empty());
    PASS();
#endif
}

TEST_MAIN()
