// RadioController tests: dry run (model 0), Hamlib's dummy rig, the PTT
// server and backend resolution.

#include "test_macros.hpp"
#include "skywave_link/radio_controller.hpp"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif
#include <hamlib/rig.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <atomic>
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

// Hamlib's in-memory dummy rig: a real backend that needs no hardware.
constexpr int kDummyRigModel = 1;  // RIG_MODEL_DUMMY

skylink::RadioConfig rigConfig(int model, uint16_t ptt_port = 0) {
    skylink::RadioConfig cfg;
    cfg.enabled = true;
    cfg.hamlib_model = model;
    cfg.ptt_server_enabled = ptt_port != 0;
    cfg.ptt_server_bind = "127.0.0.1";
    cfg.ptt_server_port = ptt_port;
    return cfg;
}

} // namespace

// K12: model 0 controls nothing, so nothing may report success.
TEST(radio_controller_dry_run_refuses_control) {
    skylink::RadioController radio;
    radio.applyConfig(rigConfig(0));
    ASSERT_TRUE(radio.connect());
    ASSERT_TRUE(radio.isDryRun());
    ASSERT_TRUE(radio.getStatus().dry_run);

    ASSERT_FALSE(radio.setFrequency(14105000));
    ASSERT_CONTAINS(radio.getStatus().last_error, "model 0");
    ASSERT_FALSE(radio.getFrequency().has_value());
    ASSERT_FALSE(radio.setMode("USB"));
    ASSERT_FALSE(radio.getMode().has_value());
    ASSERT_FALSE(radio.setPtt(true));
    ASSERT_FALSE(radio.getPtt());
    ASSERT_TRUE(radio.setPtt(false));  // unkeying nothing is not an error
    PASS();
}

// K12 + K5: in dry run the PTT server answers KEY with an error, but still
// reports that the modem is keying.
TEST(radio_controller_dry_run_ptt_server_reports_error) {
    skylink::RadioController radio;
    const auto cfg = rigConfig(0, 46334);
    radio.applyConfig(cfg);
    std::atomic<int> keyed{-1};
    radio.onPttRequest([&keyed](bool k) { keyed = k ? 1 : 0; });
    ASSERT_TRUE(radio.connect());
    ASSERT_TRUE(radio.startPttServer());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    socket_handle_t fd = connect_local(cfg.ptt_server_port);
    ASSERT_TRUE(fd != kInvalidSocket);
    ASSERT_TRUE(send_line(fd, "KEY\n"));
    const std::string reply = recv_line(fd);
    ASSERT_CONTAINS(reply, "ERROR");
    ASSERT_EQ(keyed.load(), 1);
    ASSERT_TRUE(send_line(fd, "UNKEY\n"));
    ASSERT_EQ(recv_line(fd), std::string("OK"));
    ASSERT_EQ(keyed.load(), 0);
    close_socket(fd);
    radio.stopPttServer();
    PASS();
}

TEST(radio_controller_dummy_rig_frequency_and_ptt) {
    skylink::RadioController radio;
    radio.applyConfig(rigConfig(kDummyRigModel));
    ASSERT_TRUE(radio.connect());
    ASSERT_FALSE(radio.isDryRun());

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
    const auto cfg = rigConfig(kDummyRigModel, 46331);
    radio.applyConfig(cfg);
    std::atomic<int> requests{0};
    std::atomic<int> keyed{-1};
    radio.onPttRequest([&](bool k) { ++requests; keyed = k ? 1 : 0; });
    ASSERT_TRUE(radio.connect());
    ASSERT_TRUE(radio.startPttServer());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    socket_handle_t fd = connect_local(cfg.ptt_server_port);
    ASSERT_TRUE(fd != kInvalidSocket);

    ASSERT_TRUE(send_line(fd, "STATUS\n"));
    ASSERT_EQ(recv_line(fd), std::string("PTT:OFF"));

    ASSERT_TRUE(send_line(fd, "KEY\n"));
    ASSERT_EQ(recv_line(fd), std::string("OK"));
    ASSERT_EQ(keyed.load(), 1);
    ASSERT_TRUE(send_line(fd, "STATUS\n"));
    ASSERT_EQ(recv_line(fd), std::string("PTT:ON"));

    ASSERT_TRUE(send_line(fd, "UNKEY\n"));
    ASSERT_EQ(recv_line(fd), std::string("OK"));
    ASSERT_EQ(keyed.load(), 0);
    ASSERT_EQ(requests.load(), 2);
    ASSERT_TRUE(send_line(fd, "STATUS\n"));
    ASSERT_EQ(recv_line(fd), std::string("PTT:OFF"));

    close_socket(fd);
    radio.stopPttServer();
    PASS();
}

TEST(radio_controller_ptt_client_drop_unkeys) {
    skylink::RadioController radio;
    const auto cfg = rigConfig(kDummyRigModel, 46332);
    radio.applyConfig(cfg);
    std::atomic<int> keyed{-1};
    radio.onPttRequest([&keyed](bool k) { keyed = k ? 1 : 0; });
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
        unkeyed = !radio.getPtt() && keyed.load() == 0;
    }
    ASSERT_TRUE(unkeyed);
    radio.stopPttServer();
    PASS();
}

TEST(radio_controller_ptt_stop_with_idle_keyed_client) {
    skylink::RadioController radio;
    const auto cfg = rigConfig(kDummyRigModel, 46333);
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

// K6: the PTT server defaults to the modem's PTTPORT default.
TEST(radio_controller_default_ptt_port_matches_modem) {
    ASSERT_EQ(skylink::kDefaultPttServerPort, (uint16_t)4532);
    skylink::RadioConfig defaults;
    ASSERT_EQ(defaults.ptt_server_port, (uint16_t)4532);
    skylink::RadioController radio;
    radio.applyConfig(rigConfig(0, 0));
    ASSERT_EQ(radio.getStatus().ptt_port, (uint16_t)4532);
    PASS();
}

// K13: the Kenwood TCP backend uses a Kenwood model, not the FlexRadio one.
TEST(resolve_radio_config_backends) {
    skylink::RadioConfig src;
    src.backend = "Kenwood TCP";
    src.kenwood_host = " 192.0.2.10 ";
    src.kenwood_port = 60000;
    src.hamlib_model = 0;
    auto cfg = skylink::resolveRadioConfig(src);
    ASSERT_TRUE(cfg.enabled);
    ASSERT_EQ(cfg.backend, std::string("kenwood_tcp"));
    ASSERT_EQ(cfg.hamlib_model, (int)RIG_MODEL_TS890S);
    ASSERT_TRUE(cfg.hamlib_model != (int)RIG_MODEL_F6K);
    ASSERT_EQ(cfg.device, std::string("192.0.2.10:60000"));

    src.hamlib_model = RIG_MODEL_TS990S;  // a Kenwood model the user chose
    ASSERT_EQ(skylink::resolveRadioConfig(src).hamlib_model, (int)RIG_MODEL_TS990S);
    src.hamlib_model = kDummyRigModel;    // not a Kenwood model
    ASSERT_EQ(skylink::resolveRadioConfig(src).hamlib_model, (int)RIG_MODEL_TS890S);

    src.backend = "hamlib";
    src.hamlib_model = 1234;
    cfg = skylink::resolveRadioConfig(src);
    ASSERT_TRUE(cfg.enabled);
    ASSERT_EQ(cfg.hamlib_model, 1234);

    src.backend = "none";
    cfg = skylink::resolveRadioConfig(src);
    ASSERT_FALSE(cfg.enabled);
    ASSERT_EQ(cfg.hamlib_model, 0);
    PASS();
}

TEST_MAIN()
