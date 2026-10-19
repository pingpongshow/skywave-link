// APRS-IS client tests - input sanitization, coordinate validation, EOF

#include "test_macros.hpp"
#include "skywave_link/aprs_client.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#endif

using namespace skylink;

TEST(sanitize_strips_crlf_and_controls) {
    ASSERT_EQ(APRSClient::sanitizeText("hi\r\nK1AAA>APRS:evil\x01\x7f!"),
              std::string("hiK1AAA>APRS:evil!"));
    ASSERT_EQ(APRSClient::sanitizeText("plain text 123"), std::string("plain text 123"));
    PASS();
}

TEST(callsign_validation) {
    ASSERT_TRUE(APRSClient::isValidCallsign("K1AAA"));
    ASSERT_TRUE(APRSClient::isValidCallsign("K1AAA-15"));
    ASSERT_FALSE(APRSClient::isValidCallsign(""));
    ASSERT_FALSE(APRSClient::isValidCallsign("K1AAA\r\n"));
    ASSERT_FALSE(APRSClient::isValidCallsign("A>B"));
    ASSERT_FALSE(APRSClient::isValidCallsign("K1AAAA-150"));  // 10 chars
    PASS();
}

TEST(coordinate_validation) {
    ASSERT_TRUE(APRSClient::isValidCoordinate(45.0, -122.0));
    ASSERT_TRUE(APRSClient::isValidCoordinate(-90.0, 180.0));
    ASSERT_FALSE(APRSClient::isValidCoordinate(std::nan(""), 0.0));
    ASSERT_FALSE(APRSClient::isValidCoordinate(0.0, std::numeric_limits<double>::infinity()));
    ASSERT_FALSE(APRSClient::isValidCoordinate(91.0, 0.0));
    ASSERT_FALSE(APRSClient::isValidCoordinate(0.0, 1e300));
    PASS();
}

TEST(format_position_rounding_and_comment) {
    APRSPosition pos;
    pos.latitude = 44.99999999;   // must not print 44 60.00
    pos.longitude = -122.5;
    pos.comment = "a\r\nb";
    std::string out = APRSClient::formatPosition(pos);
    ASSERT_EQ(out, std::string("=4500.00N/12230.00W- ab"));
    PASS();
}

#ifndef _WIN32
namespace {
struct FakeAprsIs {
    int listen_fd = -1;
    int client_fd = -1;
    uint16_t port = 0;

    bool start() {
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        if (listen(listen_fd, 1) != 0) return false;
        socklen_t len = sizeof(addr);
        getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
        return true;
    }
    void acceptClient() { client_fd = accept(listen_fd, nullptr, nullptr); }
    std::string readAvailable(int wait_ms) {
        std::string out;
        timeval tv{0, wait_ms * 1000};
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char buf[2048];
        while (true) {
            ssize_t n = recv(client_fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            out.append(buf, static_cast<size_t>(n));
        }
        return out;
    }
    void closeClient() { if (client_fd >= 0) close(client_fd); client_fd = -1; }
    ~FakeAprsIs() { closeClient(); if (listen_fd >= 0) close(listen_fd); }
};

size_t countLines(const std::string& s) {
    size_t n = 0;
    for (char c : s) if (c == '\n') ++n;
    return n;
}
} // namespace

TEST(relay_injection_blocked_and_eof_disconnects) {
    FakeAprsIs srv;
    ASSERT_TRUE(srv.start());
    APRSClient client;
    std::thread acceptor([&]() {
        srv.acceptClient();
        const char* banner = "# logresp K1AAA verified\r\n";
        send(srv.client_fd, banner, strlen(banner), 0);
    });
    ASSERT_TRUE(client.connect("127.0.0.1", srv.port));
    acceptor.join();
    ASSERT_TRUE(client.authenticate("K1AAA", "12345"));
    ASSERT_TRUE(client.isConnected());
    std::string login = srv.readAvailable(200);
    ASSERT_EQ(countLines(login), size_t(1));

    // CR/LF in comment/grid must not create extra APRS-IS lines.
    ASSERT_TRUE(client.sendPositionFor("K1BBB", 41.7, -72.7, "FN31\r\n#filter x",
                                       "hello\r\nK1BBB>APRS,TCPIP*::BLN1     :spam"));
    // Bad callsign / coordinates are rejected outright.
    ASSERT_FALSE(client.sendPositionFor("K1BBB\r\nX", 41.7, -72.7));
    ASSERT_FALSE(client.sendPositionFor("K1BBB", std::nan(""), -72.7));
    ASSERT_FALSE(client.sendPositionFor("K1BBB", 41.7, 1e300));
    std::string sent = srv.readAvailable(200);
    ASSERT_EQ(countLines(sent), size_t(1));
    ASSERT_CONTAINS(sent, "}K1BBB>APRS,TCPIP,K1AAA*:=4142.00N/07242.00W&");
    ASSERT_CONTAINS(sent, "QTH FN31#filter x");

    // Server closes: poll must notice EOF and mark the client disconnected.
    srv.closeClient();
    for (int i = 0; i < 20 && client.getState() == APRSState::Connected; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        client.poll();
    }
    ASSERT_EQ(static_cast<int>(client.getState()), static_cast<int>(APRSState::Disconnected));
    PASS();
}

// Background login: beginConnect() returns at once, pollConnect() adopts
// the session and sends a queued position; an unreachable server never
// blocks the caller and can be cancelled.
TEST(background_login_and_queued_position) {
    FakeAprsIs srv;
    ASSERT_TRUE(srv.start());
    std::thread acceptor([&]() {
        srv.acceptClient();
        const char* banner = "# logresp K1AAA verified\r\n";
        send(srv.client_fd, banner, strlen(banner), 0);
    });
    APRSClient client;
    client.setMyPosition(41.7, -72.7);
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(client.beginConnect("127.0.0.1", srv.port, "K1AAA", "12345"));
    ASSERT_TRUE(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(50));
    ASSERT_TRUE(client.isConnecting());
    client.sendPositionWhenConnected();
    APRSConnectProgress p = APRSConnectProgress::InProgress;
    for (int i = 0; i < 300 && p == APRSConnectProgress::InProgress; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        p = client.pollConnect();
    }
    acceptor.join();
    ASSERT_TRUE(p == APRSConnectProgress::Connected);
    ASSERT_TRUE(client.isConnected());
    ASSERT_TRUE(client.takeQueuedPositionSent());
    ASSERT_FALSE(client.takeQueuedPositionSent());
    const std::string sent = srv.readAvailable(200);
    ASSERT_CONTAINS(sent, "user K1AAA pass 12345");
    // K14: without a filter port 14580 sends nothing at all.
    ASSERT_CONTAINS(sent, " filter r/41.7/-72.7/100 b/K1AAA\r\n");
    ASSERT_CONTAINS(sent, "K1AAA>APRS,TCPIP*:");
    client.disconnect();

    // TEST-NET-1 (RFC 5737) is never routed: the connect cannot complete.
    APRSClient unreachable;
    const auto t1 = std::chrono::steady_clock::now();
    ASSERT_TRUE(unreachable.beginConnect("192.0.2.1", 14580, "K1AAA", "-1"));
    ASSERT_TRUE(std::chrono::steady_clock::now() - t1 < std::chrono::milliseconds(50));
    ASSERT_TRUE(unreachable.pollConnect() != APRSConnectProgress::Connected);
    unreachable.disconnect();  // cancels without waiting
    ASSERT_FALSE(unreachable.isConnecting());
    PASS();
}

// K14: the filter follows the station position and settings while
// connected; inbound messages to this station are delivered and acked;
// other traffic is not mistaken for positions or messages.
TEST(filter_updates_messages_and_acks) {
    FakeAprsIs srv;
    ASSERT_TRUE(srv.start());
    std::thread acceptor([&]() {
        srv.acceptClient();
        const char* banner = "# logresp K1AAA verified\r\n";
        send(srv.client_fd, banner, strlen(banner), 0);
    });
    APRSClient client;
    std::vector<APRSPosition> positions;
    std::vector<std::pair<std::string, std::string>> messages;
    client.onPositionReceived([&](const APRSPosition& p) { positions.push_back(p); });
    client.onMessageReceived([&](const std::string& f, const std::string& m) {
        messages.emplace_back(f, m);
    });
    ASSERT_TRUE(client.beginConnect("127.0.0.1", srv.port, "K1AAA", "12345"));
    APRSConnectProgress p = APRSConnectProgress::InProgress;
    for (int i = 0; i < 300 && p == APRSConnectProgress::InProgress; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        p = client.pollConnect();
    }
    acceptor.join();
    ASSERT_TRUE(p == APRSConnectProgress::Connected);
    std::string sent = srv.readAvailable(200);
    ASSERT_CONTAINS(sent, " filter b/K1AAA\r\n");  // no position yet

    ASSERT_FALSE(client.sendPositionNow());          // no fix: nothing beaconed
    client.setMyPosition(42.35, -71.06);
    sent = srv.readAvailable(200);
    ASSERT_EQ(sent, std::string("#filter r/42.4/-71.1/100 b/K1AAA\r\n"));
    client.setMyPosition(42.36, -71.06);              // same filter text: nothing sent
    ASSERT_EQ(srv.readAvailable(100), std::string());
    client.setFilter("r/40/-70/10");
    ASSERT_EQ(srv.readAvailable(200), std::string("#filter r/40/-70/10\r\n"));

    const std::string inbound =
        "K1BBB>APRS,TCPIP*,qAC,T2TEST::K1AAA    :Hello there{12\r\n"
        "K1BBB>APRS,TCPIP*::K1CCC    :not for us{3\r\n"
        "K1BBB>APRS,TCPIP*::K1AAA    :ack7\r\n"
        "K1CCC>APRS,TCPIP*:=4203.50N/07130.25W-QTH: home, 10:30\r\n"
        "K1DDD>APRS,TCPIP*:@092345z4903.50N/07201.75W>moving\r\n"
        "K1EEE>APRS,TCPIP*:>status text: no position\r\n";
    send(srv.client_fd, inbound.data(), inbound.size(), 0);
    for (int i = 0; i < 20; ++i) {
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(messages.size(), size_t(1));
    ASSERT_EQ(messages[0].first, std::string("K1BBB"));
    ASSERT_EQ(messages[0].second, std::string("Hello there"));
    ASSERT_EQ(srv.readAvailable(200), std::string("K1AAA>APRS,TCPIP*::K1BBB    :ack12\r\n"));
    ASSERT_EQ(positions.size(), size_t(2));
    ASSERT_EQ(positions[0].callsign, std::string("K1CCC"));
    ASSERT_TRUE(std::fabs(positions[0].latitude - (42.0 + 3.5 / 60.0)) < 1e-9);
    ASSERT_TRUE(std::fabs(positions[0].longitude + (71.0 + 30.25 / 60.0)) < 1e-9);
    ASSERT_EQ(positions[0].comment, std::string("QTH: home, 10:30"));
    ASSERT_EQ(positions[1].callsign, std::string("K1DDD"));
    ASSERT_TRUE(positions[1].symbol.symbol == '>');

    // Directed position query and numbered outgoing messages.
    ASSERT_TRUE(client.requestPosition("K1BBB"));
    ASSERT_TRUE(client.sendMessage("K1BBB", "hi {there}|~"));
    sent = srv.readAvailable(200);
    ASSERT_CONTAINS(sent, "K1AAA>APRS,TCPIP*::K1BBB    :?APRSP\r\n");
    ASSERT_CONTAINS(sent, "K1AAA>APRS,TCPIP*::K1BBB    :hi there}{1\r\n");
    ASSERT_FALSE(client.requestPosition("BAD>CALL"));
    client.disconnect();
    PASS();
}
#endif

TEST(default_filter_and_passcode_helpers) {
    ASSERT_EQ(APRSClient::defaultFilter("k1aaa-9", true, 51.5, -0.12, 75),
              std::string("r/51.5/-0.1/75 b/K1AAA-9"));
    ASSERT_EQ(APRSClient::defaultFilter("K1AAA", false, 0.0, 0.0, 75), std::string("b/K1AAA"));
    ASSERT_EQ(APRSClient::defaultFilter("bad>call", false, 0.0, 0.0, 75), std::string());
    ASSERT_EQ(APRSClient::effectivePasscode(" 12345 ", "K1AAA"), std::string("12345"));
    ASSERT_EQ(APRSClient::effectivePasscode("", ""), std::string("-1"));
    ASSERT_EQ(APRSClient::effectivePasscode("", "K1AAA"),
              std::to_string(APRSClient::calculatePasscode("K1AAA")));
    PASS();
}

TEST(position_parser_rejects_non_positions) {
    ASSERT_FALSE(APRSClient::parsePositionReport("K1AAA>APRS:>just status").has_value());
    ASSERT_FALSE(APRSClient::parsePositionReport("K1AAA>APRS:!/5L!!<*e7>7P[").has_value());  // compressed
    ASSERT_FALSE(APRSClient::parsePositionReport("K1AAA>APRS:!4960.00N/07201.75W-").has_value());
    ASSERT_FALSE(APRSClient::parsePositionReport("no header at all").has_value());
    auto ok = APRSClient::parsePositionReport("K1AAA>APRS,WIDE1-1:!4903.50S/07201.75E-");
    ASSERT_TRUE(ok.has_value());
    ASSERT_TRUE(ok->latitude < 0 && ok->longitude > 0);
    auto none = APRSClient::parseMessageFor("K1BBB>APRS::K1AAA    :rej5", "K1AAA");
    ASSERT_FALSE(none.has_value());
    PASS();
}

TEST_MAIN()
