// RadioController dry-run tests

#include "test_macros.hpp"
#include "skywave_link/radio_controller.hpp"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
using socket_handle_t = SOCKET;
constexpr socket_handle_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_handle_t = int;
constexpr socket_handle_t kInvalidSocket = -1;
#endif

void close_socket(socket_handle_t fd) {
#ifdef _WIN32
    if (fd != INVALID_SOCKET) closesocket(fd);
#else
    if (fd >= 0) close(fd);
#endif
}

bool ensure_socket_runtime() {
#ifdef _WIN32
    static bool ready = false;
    if (!ready) {
        WSADATA wsa_data;
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            return false;
        }
        ready = true;
    }
#endif
    return true;
}

std::string recv_line(socket_handle_t fd) {
    std::string out;
    char ch = '\0';
    while (true) {
#ifdef _WIN32
        const int rc = recv(fd, &ch, 1, 0);
#else
        const ssize_t rc = recv(fd, &ch, 1, 0);
#endif
        if (rc <= 0) {
            break;
        }
        if (ch == '\n') {
            break;
        }
        if (ch != '\r') {
            out.push_back(ch);
        }
    }
    return out;
}

bool send_line(socket_handle_t fd, const std::string& line) {
    const char* data = line.c_str();
    size_t remaining = line.size();
    while (remaining > 0) {
#ifdef _WIN32
        const int sent = send(fd, data, static_cast<int>(remaining), 0);
#else
        const ssize_t sent = send(fd, data, remaining, 0);
#endif
        if (sent <= 0) {
            return false;
        }
        data += sent;
        remaining -= static_cast<size_t>(sent);
    }
    return true;
}

socket_handle_t connect_local(uint16_t port) {
    if (!ensure_socket_runtime()) {
        return kInvalidSocket;
    }

    socket_handle_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSocket) {
        return kInvalidSocket;
    }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_socket(fd);
        return kInvalidSocket;
    }
    return fd;
}

} // namespace

TEST(radio_controller_dry_run_frequency_and_ptt) {
    skylink::RadioController radio;
    skylink::RadioConfig cfg;
    cfg.enabled = true;
    cfg.hamlib_model = 0;
    cfg.ptt_server_enabled = false;

    radio.applyConfig(cfg);
    ASSERT_TRUE(radio.connect());
    ASSERT_TRUE(radio.isConnected());
    ASSERT_TRUE(radio.isDryRun());

    ASSERT_TRUE(radio.setFrequency(14105000));
    const auto freq = radio.getFrequency();
    ASSERT_TRUE(freq.has_value());
    ASSERT_EQ(*freq, (uint64_t)14105000);

    ASSERT_TRUE(radio.setMode("USB"));
    const auto mode = radio.getMode();
    ASSERT_TRUE(mode.has_value());
    ASSERT_EQ(*mode, std::string("USB"));

    ASSERT_TRUE(radio.setPtt(true));
    ASSERT_TRUE(radio.getPtt());
    ASSERT_TRUE(radio.setPtt(false));
    ASSERT_FALSE(radio.getPtt());
    PASS();
}

TEST(radio_controller_ptt_server_roundtrip) {
    skylink::RadioController radio;
    skylink::RadioConfig cfg;
    cfg.enabled = true;
    cfg.hamlib_model = 0;
    cfg.ptt_server_enabled = true;
    cfg.ptt_server_bind = "127.0.0.1";
    cfg.ptt_server_port = 46331;

    radio.applyConfig(cfg);
    ASSERT_TRUE(radio.connect());
    ASSERT_TRUE(radio.startPttServer());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    socket_handle_t fd = connect_local(cfg.ptt_server_port);
    ASSERT_TRUE(fd != kInvalidSocket);

    ASSERT_TRUE(send_line(fd, "STATUS\n"));
    ASSERT_EQ(recv_line(fd), std::string("PTT:OFF"));

    ASSERT_TRUE(send_line(fd, "KEY\n"));
    ASSERT_EQ(recv_line(fd), std::string("OK"));
    ASSERT_TRUE(send_line(fd, "STATUS\n"));
    ASSERT_EQ(recv_line(fd), std::string("PTT:ON"));

    ASSERT_TRUE(send_line(fd, "UNKEY\n"));
    ASSERT_EQ(recv_line(fd), std::string("OK"));
    ASSERT_TRUE(send_line(fd, "STATUS\n"));
    ASSERT_EQ(recv_line(fd), std::string("PTT:OFF"));

    close_socket(fd);
    radio.stopPttServer();
    PASS();
}

namespace {
skylink::RadioConfig pttServerConfig(uint16_t port) {
    skylink::RadioConfig cfg;
    cfg.enabled = true;
    cfg.hamlib_model = 0;
    cfg.ptt_server_enabled = true;
    cfg.ptt_server_bind = "127.0.0.1";
    cfg.ptt_server_port = port;
    return cfg;
}
} // namespace

TEST(radio_controller_ptt_client_drop_unkeys) {
    skylink::RadioController radio;
    const auto cfg = pttServerConfig(46332);
    radio.applyConfig(cfg);
    ASSERT_TRUE(radio.connect());
    ASSERT_TRUE(radio.startPttServer());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    socket_handle_t fd = connect_local(cfg.ptt_server_port);
    ASSERT_TRUE(fd != kInvalidSocket);
    ASSERT_TRUE(send_line(fd, "KEY\n"));
    ASSERT_EQ(recv_line(fd), std::string("OK"));
    ASSERT_TRUE(radio.getPtt());

    close_socket(fd);  // drop without UNKEY
    bool unkeyed = false;
    for (int i = 0; i < 50 && !unkeyed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        unkeyed = !radio.getPtt();
    }
    ASSERT_TRUE(unkeyed);
    radio.stopPttServer();
    PASS();
}

TEST(radio_controller_ptt_stop_with_idle_keyed_client) {
    skylink::RadioController radio;
    const auto cfg = pttServerConfig(46333);
    radio.applyConfig(cfg);
    ASSERT_TRUE(radio.connect());
    ASSERT_TRUE(radio.startPttServer());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    socket_handle_t fd = connect_local(cfg.ptt_server_port);
    ASSERT_TRUE(fd != kInvalidSocket);
    ASSERT_TRUE(send_line(fd, "KEY\n"));
    ASSERT_EQ(recv_line(fd), std::string("OK"));

    // Client stays connected and silent (blocked in the server's recv).
    // stopPttServer must wake it, unkey and return promptly.
    const auto t0 = std::chrono::steady_clock::now();
    radio.stopPttServer();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    ASSERT_LT(ms, 2000);
    ASSERT_FALSE(radio.getPtt());
    ASSERT_FALSE(radio.getStatus().ptt_server_running);
    close_socket(fd);
    PASS();
}

TEST_MAIN()