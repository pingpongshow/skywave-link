// APRS-IS client tests - input sanitization, coordinate validation, EOF

#include "test_macros.hpp"
#include "skywave_link/aprs_client.hpp"

#include <cmath>
#include <limits>

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
    ASSERT_EQ(APRSClient::sanitizeText("hi\r\nN0CALL>APRS:evil\x01\x7f!"),
              std::string("hiN0CALL>APRS:evil!"));
    ASSERT_EQ(APRSClient::sanitizeText("plain text 123"), std::string("plain text 123"));
    PASS();
}

TEST(callsign_validation) {
    ASSERT_TRUE(APRSClient::isValidCallsign("N0CALL"));
    ASSERT_TRUE(APRSClient::isValidCallsign("N0CALL-15"));
    ASSERT_FALSE(APRSClient::isValidCallsign(""));
    ASSERT_FALSE(APRSClient::isValidCallsign("N0CALL\r\n"));
    ASSERT_FALSE(APRSClient::isValidCallsign("A>B"));
    ASSERT_FALSE(APRSClient::isValidCallsign("N0CALL-150"));  // 10 chars
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
        const char* banner = "# logresp N0CALL verified\r\n";
        send(srv.client_fd, banner, strlen(banner), 0);
    });
    ASSERT_TRUE(client.connect("127.0.0.1", srv.port));
    acceptor.join();
    ASSERT_TRUE(client.authenticate("N0CALL", "12345"));
    ASSERT_TRUE(client.isConnected());
    std::string login = srv.readAvailable(200);
    ASSERT_EQ(countLines(login), size_t(1));

    // CR/LF in comment/grid must not create extra APRS-IS lines.
    ASSERT_TRUE(client.sendPositionFor("W1AW", 41.7, -72.7, "FN31\r\n#filter x",
                                       "hello\r\nW1AW>APRS,TCPIP*::BLN1     :spam"));
    // Bad callsign / coordinates are rejected outright.
    ASSERT_FALSE(client.sendPositionFor("W1AW\r\nX", 41.7, -72.7));
    ASSERT_FALSE(client.sendPositionFor("W1AW", std::nan(""), -72.7));
    ASSERT_FALSE(client.sendPositionFor("W1AW", 41.7, 1e300));
    std::string sent = srv.readAvailable(200);
    ASSERT_EQ(countLines(sent), size_t(1));
    ASSERT_CONTAINS(sent, "}W1AW>APRS,TCPIP,N0CALL*:=4142.00N/07242.00W&");
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
#endif

TEST_MAIN()
