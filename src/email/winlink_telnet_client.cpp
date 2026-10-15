// Winlink CMS Telnet client for internet email relay

#include "skywave_link/winlink_telnet_client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <netdb.h>
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

#ifdef SKYWAVE_LINK_HAS_OPENSSL
    #include <openssl/ssl.h>
    #include <openssl/err.h>
    #include <openssl/x509v3.h>
#endif

namespace skylink {
namespace {

std::string trim(const std::string& s) {
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string lowerAscii(const std::string& input) {
    std::string out = input;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string sanitizeSingleLine(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        if (c == '\r' || c == '\n' || c == '\0') {
            continue;
        }
        out.push_back(c);
    }
    return trim(out);
}

bool containsAnyCaseInsensitive(const std::string& text,
                                const std::vector<std::string>& needles) {
    const std::string hay = lowerAscii(text);
    for (const auto& needle : needles) {
        if (!needle.empty() && hay.find(lowerAscii(needle)) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool startsWithCI(const std::string& text, const std::string& prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

std::string normalizeBbsBody(const std::string& input) {
    std::istringstream lines(input);
    std::string line;
    std::string out;

    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        // /EX is an end-of-message marker in many BBS sessions.
        if (lowerAscii(line) == "/ex") {
            line.insert(line.begin(), ' ');
        }
        out += line;
        out += "\r\n";
    }

    if (out.empty()) {
        out = "\r\n";
    }
    return out;
}

enum class LoginMode {
    AutoInteractive,
    ChallengeAugmented,
    CmsCommand
};

const char* loginModeName(LoginMode mode) {
    switch (mode) {
        case LoginMode::AutoInteractive: return "auto-interactive";
        case LoginMode::ChallengeAugmented: return "challenge-augmented";
        case LoginMode::CmsCommand: return "cmstelnet-command";
    }
    return "unknown";
}

bool responseLooksLikeLoginFailure(const std::string& text) {
    return containsAnyCaseInsensitive(text, {
        "invalid",
        "unable to complete login",
        "locked out",
        "incorrect",
        "rejected",
        "denied",
        "disconnect"
    });
}

bool responseLooksLikePromptReady(const std::string& text) {
    return containsAnyCaseInsensitive(text, {
        "\n>", "\r>", "cmd:", "bbs>", "welcome", "connected"
    });
}

class TelnetSession {
public:
#ifdef _WIN32
    using socket_t = SOCKET;
    static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
    using socket_t = int;
    static constexpr socket_t kInvalidSocket = -1;
#endif

    TelnetSession() {
#ifdef SKYWAVE_LINK_HAS_OPENSSL
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        if (ssl_ctx_) {
            SSL_CTX_set_default_verify_paths(ssl_ctx_);
            SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_PEER, nullptr);
        }
#endif
    }

    ~TelnetSession() {
        disconnect();
#ifdef SKYWAVE_LINK_HAS_OPENSSL
        if (ssl_ctx_) {
            SSL_CTX_free(ssl_ctx_);
            ssl_ctx_ = nullptr;
        }
#endif
    }

    bool connectTo(const std::string& host, uint16_t port, bool use_tls, std::string* error) {
        struct addrinfo hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        struct addrinfo* result = nullptr;
        const std::string port_text = std::to_string(port);
        if (getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result) != 0) {
            if (error) *error = "DNS resolution failed for " + host;
            return false;
        }

        bool connected = false;
        for (auto* ai = result; ai != nullptr; ai = ai->ai_next) {
            socket_ = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (socket_ == kInvalidSocket) {
                continue;
            }
#ifdef SO_NOSIGPIPE
            const int no_sigpipe = 1;
            (void)setsockopt(socket_, SOL_SOCKET, SO_NOSIGPIPE,
                             &no_sigpipe, sizeof(no_sigpipe));
#endif
            if (::connect(socket_, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
                connected = true;
                break;
            }
            closeSocket();
        }
        freeaddrinfo(result);

        if (!connected) {
            if (error) *error = "Failed to connect to " + host + ":" + port_text;
            return false;
        }

        tls_requested_ = use_tls;
        if (!tls_requested_) {
            return true;
        }

#ifdef SKYWAVE_LINK_HAS_OPENSSL
        if (!ssl_ctx_) {
            if (error) *error = "TLS is unavailable (SSL context init failed)";
            disconnect();
            return false;
        }

        ssl_ = SSL_new(ssl_ctx_);
        if (!ssl_) {
            if (error) *error = "Failed to allocate TLS session";
            disconnect();
            return false;
        }

        SSL_set_fd(ssl_, static_cast<int>(socket_));
        SSL_set_tlsext_host_name(ssl_, host.c_str());
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
        if (SSL_set1_host(ssl_, host.c_str()) != 1) {
            if (error) *error = "Failed to configure TLS hostname verification";
            disconnect();
            return false;
        }
#else
        X509_VERIFY_PARAM* verify = SSL_get0_param(ssl_);
        if (verify == nullptr ||
            X509_VERIFY_PARAM_set1_host(verify, host.c_str(), 0) != 1) {
            if (error) *error = "Failed to configure TLS hostname verification";
            disconnect();
            return false;
        }
#endif
        if (SSL_connect(ssl_) != 1) {
            if (error) *error = "TLS handshake failed";
            disconnect();
            return false;
        }
        if (SSL_get_verify_result(ssl_) != X509_V_OK) {
            if (error) *error = "TLS certificate verification failed";
            disconnect();
            return false;
        }
        tls_active_ = true;
        return true;
#else
        if (error) *error = "TLS requested but OpenSSL is not available";
        disconnect();
        return false;
#endif
    }

    void disconnect() {
#ifdef SKYWAVE_LINK_HAS_OPENSSL
        if (ssl_) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
#endif
        closeSocket();
        tls_active_ = false;
        tls_requested_ = false;
    }

    bool sendLine(const std::string& line, std::string* error) {
        return sendRaw(line + "\r\n", error);
    }

    bool sendRaw(const std::string& data, std::string* error) {
        size_t offset = 0;
        while (offset < data.size()) {
            int written = 0;
#ifdef SKYWAVE_LINK_HAS_OPENSSL
            if (tls_active_ && ssl_) {
                written = SSL_write(ssl_,
                                    data.data() + offset,
                                    static_cast<int>(data.size() - offset));
            } else
#endif
            {
                int send_flags = 0;
#ifdef MSG_NOSIGNAL
                send_flags = MSG_NOSIGNAL;
#endif
                written = static_cast<int>(::send(socket_,
                                                  data.data() + offset,
                                                  static_cast<int>(data.size() - offset),
                                                  send_flags));
            }

            if (written <= 0) {
                if (error) *error = "Socket write failed";
                return false;
            }
            offset += static_cast<size_t>(written);
        }
        return true;
    }

    bool readUntilAny(const std::vector<std::string>& patterns,
                      int timeout_ms,
                      std::string* output,
                      std::string* error,
                      size_t max_capture = 256 * 1024) {
        if (output) {
            output->clear();
        }

        std::vector<std::string> patterns_lc;
        patterns_lc.reserve(patterns.size());
        for (const auto& p : patterns) {
            patterns_lc.push_back(lowerAscii(p));
        }

        std::string capture;
        capture.reserve(4096);
        std::string capture_lc;
        capture_lc.reserve(4096);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto now = std::chrono::steady_clock::now();
            const int remaining_ms = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
            if (remaining_ms <= 0) {
                break;
            }

            uint8_t b = 0;
            ReadStatus status = readByte(&b, std::min(remaining_ms, 800), error);
            if (status == ReadStatus::Timeout) {
                continue;
            }
            if (status == ReadStatus::Closed) {
                if (error && error->empty()) {
                    *error = "Remote server closed the connection";
                }
                if (output) *output = capture;
                return false;
            }
            if (status == ReadStatus::Error) {
                if (output) *output = capture;
                return false;
            }

            if (b == 255) {  // IAC
                if (!handleTelnetNegotiation(error)) {
                    if (output) *output = capture;
                    return false;
                }
                continue;
            }

            const char ch = static_cast<char>(b);
            if (ch == '\0') {
                continue;
            }
            if (ch == '\r' || ch == '\n' || ch == '\t' || (ch >= 32 && ch <= 126)) {
                if (capture.size() >= max_capture) {
                    if (error) *error = "Telnet response exceeded capture limit";
                    if (output) *output = capture;
                    return false;
                }
                capture.push_back(ch);
                capture_lc.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            }

            for (const auto& pattern : patterns_lc) {
                if (!pattern.empty() && capture_lc.find(pattern) != std::string::npos) {
                    if (output) *output = capture;
                    return true;
                }
            }
        }

        if (error) *error = "Timed out waiting for Telnet server response";
        if (output) *output = capture;
        return false;
    }

private:
    enum class ReadStatus {
        Ok,
        Timeout,
        Closed,
        Error
    };

    socket_t socket_ = kInvalidSocket;
    bool tls_requested_ = false;
    bool tls_active_ = false;

#ifdef SKYWAVE_LINK_HAS_OPENSSL
    SSL_CTX* ssl_ctx_ = nullptr;
    SSL* ssl_ = nullptr;
#endif

    void closeSocket() {
        if (socket_ == kInvalidSocket) {
            return;
        }
#ifdef _WIN32
        closesocket(socket_);
#else
        close(socket_);
#endif
        socket_ = kInvalidSocket;
    }

    ReadStatus readByte(uint8_t* out, int timeout_ms, std::string* error) {
#ifdef SKYWAVE_LINK_HAS_OPENSSL
        if (tls_active_ && ssl_ && SSL_pending(ssl_) > 0) {
            const int n = SSL_read(ssl_, out, 1);
            if (n == 1) {
                return ReadStatus::Ok;
            }
            if (error) *error = "TLS read failed";
            return ReadStatus::Error;
        }
#endif

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(socket_, &rfds);

        struct timeval tv {};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

#ifdef _WIN32
        const int ready = select(0, &rfds, nullptr, nullptr, &tv);
#else
        const int ready = select(socket_ + 1, &rfds, nullptr, nullptr, &tv);
#endif
        if (ready == 0) {
            return ReadStatus::Timeout;
        }
        if (ready < 0) {
            if (error) *error = "Socket read select() failed";
            return ReadStatus::Error;
        }

        int n = 0;
#ifdef SKYWAVE_LINK_HAS_OPENSSL
        if (tls_active_ && ssl_) {
            n = SSL_read(ssl_, out, 1);
        } else
#endif
        {
            n = static_cast<int>(recv(socket_, reinterpret_cast<char*>(out), 1, 0));
        }

        if (n == 0) {
            return ReadStatus::Closed;
        }
        if (n < 0) {
            if (error) *error = "Socket read failed";
            return ReadStatus::Error;
        }
        return ReadStatus::Ok;
    }

    bool handleTelnetNegotiation(std::string* error) {
        uint8_t cmd = 0;
        uint8_t opt = 0;
        ReadStatus s1 = readByte(&cmd, 1500, error);
        ReadStatus s2 = readByte(&opt, 1500, error);
        if (s1 != ReadStatus::Ok || s2 != ReadStatus::Ok) {
            if (error && error->empty()) {
                *error = "Malformed TELNET negotiation sequence";
            }
            return false;
        }

        // Minimal negotiation handling: decline all options.
        std::array<uint8_t, 3> reply {};
        reply[0] = 255;  // IAC
        if (cmd == 251 || cmd == 252) {          // WILL/WONT
            reply[1] = 254;                       // DONT
        } else if (cmd == 253 || cmd == 254) {   // DO/DONT
            reply[1] = 252;                       // WONT
        } else {
            return true;
        }
        reply[2] = opt;
        std::string wire;
        wire.push_back(static_cast<char>(reply[0]));
        wire.push_back(static_cast<char>(reply[1]));
        wire.push_back(static_cast<char>(reply[2]));
        return sendRaw(wire, error);
    }
};

std::string normalizeCallsignForAuth(const std::string& input) {
    std::string cs = sanitizeSingleLine(input);
    std::transform(cs.begin(), cs.end(), cs.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return cs;
}

bool loginWithMode(TelnetSession& session,
                   const std::string& callsign,
                   const std::string& password,
                   int timeout_ms,
                   LoginMode mode,
                   std::string* error) {
    auto absorbTrailingPrompt = [&](std::string* response_text) {
        if (!response_text) {
            return;
        }
        if (containsAnyCaseInsensitive(*response_text, {"\n>", "\r>", "cmd:", "bbs>"})) {
            return;
        }
        if (!containsAnyCaseInsensitive(*response_text, {"welcome", "connected"})) {
            return;
        }

        std::string tail;
        std::string tail_error;
        const int tail_timeout_ms = std::max(400, std::min(timeout_ms, 2000));
        if (session.readUntilAny({"\n>", "\r>", "cmd:", "bbs>", "disconnect"},
                                 tail_timeout_ms,
                                 &tail,
                                 &tail_error,
                                 64 * 1024)) {
            response_text->append(tail);
        }
    };

    std::string response;
    if (!session.readUntilAny({"callsign", "call sign", "login", "password", "passcode",
                               "challenge", ">", "cmd", "disconnect", "invalid"},
                              timeout_ms,
                              &response,
                              error)) {
        return false;
    }

    if (responseLooksLikeLoginFailure(response)) {
        if (error) *error = "Winlink login rejected: " + trim(response);
        return false;
    }
    if (responseLooksLikePromptReady(response) &&
        !containsAnyCaseInsensitive(response, {"password", "passcode", "callsign", "login"})) {
        absorbTrailingPrompt(&response);
        return true;
    }

    if (mode == LoginMode::CmsCommand) {
        if (!session.sendLine("CMSTELNET " + callsign + " " + password, error)) {
            return false;
        }
        if (!session.readUntilAny({"password", "passcode", "welcome", "connected", ">", "cmd",
                                   "disconnect", "invalid", "incorrect"},
                                  timeout_ms,
                                  &response,
                                  error)) {
            return false;
        }
        if (containsAnyCaseInsensitive(response, {"password", "passcode"})) {
            if (!session.sendLine(password, error)) {
                return false;
            }
            if (!session.readUntilAny({"welcome", "connected", ">", "cmd",
                                       "disconnect", "invalid", "incorrect"},
                                      timeout_ms,
                                      &response,
                                      error)) {
                return false;
            }
        }
        absorbTrailingPrompt(&response);
        if (responseLooksLikeLoginFailure(response)) {
            if (error) *error = "Winlink command login rejected: " + trim(response);
            return false;
        }
        return responseLooksLikePromptReady(response);
    }

    if (!session.sendLine(callsign, error)) {
        return false;
    }
    if (!session.readUntilAny({"password", "passcode", "challenge", "welcome", "connected",
                               ">", "cmd", "disconnect", "invalid", "incorrect"},
                              timeout_ms,
                              &response,
                              error)) {
        return false;
    }
    if (responseLooksLikeLoginFailure(response)) {
        if (error) *error = "Winlink login rejected after callsign: " + trim(response);
        return false;
    }

    if (containsAnyCaseInsensitive(response, {"password", "passcode", "challenge"})) {
        const std::string challenge = extractWinlinkChallengeToken(response);
        std::string auth = password;
        if (mode == LoginMode::ChallengeAugmented) {
            auth = buildWinlinkChallengeResponse(password, challenge);
        }
        if (!session.sendLine(auth, error)) {
            return false;
        }
        if (!session.readUntilAny({"welcome", "connected", ">", "cmd",
                                   "disconnect", "invalid", "incorrect"},
                                  timeout_ms,
                                  &response,
                                  error)) {
            return false;
        }
    }

    absorbTrailingPrompt(&response);
    if (responseLooksLikeLoginFailure(response)) {
        if (error) *error = "Winlink login rejected after password: " + trim(response);
        return false;
    }
    if (!responseLooksLikePromptReady(response)) {
        if (error) *error = "Winlink login did not reach command prompt";
        return false;
    }
    return true;
}

bool tryAddressingCommand(TelnetSession& session,
                          const std::vector<std::string>& commands,
                          int timeout_ms,
                          std::string* error) {
    for (const auto& cmd : commands) {
        std::string response;
        if (!session.sendLine(cmd, error)) {
            return false;
        }
        if (!session.readUntilAny({"subject", "subj", "message", "text",
                                   "unknown", "invalid", "error", "reject", "disconnect", ">", "cmd"},
                                  timeout_ms,
                                  &response,
                                  error)) {
            return false;
        }
        if (containsAnyCaseInsensitive(response, {"disconnect"})) {
            if (error) *error = "Winlink server disconnected while setting recipient";
            return false;
        }
        if (containsAnyCaseInsensitive(response, {"subject", "subj"})) {
            return true;
        }
        if (containsAnyCaseInsensitive(response, {"unknown", "invalid", "error", "reject"})) {
            continue;
        }
        // Some servers may move directly to body prompt.
        if (containsAnyCaseInsensitive(response, {"message", "text", "/ex"})) {
            return true;
        }
    }
    if (error) *error = "Winlink server rejected recipient command variants";
    return false;
}

bool sendInteractiveBbsMail(TelnetSession& session,
                            const WinlinkTelnetRequest& req,
                            std::string* error) {
    const std::string to = sanitizeSingleLine(req.to);
    if (to.empty()) {
        if (error) *error = "Winlink destination is empty";
        return false;
    }

    if (!tryAddressingCommand(session,
                              {"SP " + to, "SM " + to, "SEND " + to},
                              req.timeout_ms,
                              error)) {
        return false;
    }

    const std::string subject = sanitizeSingleLine(req.subject).empty()
        ? std::string("(no subject)")
        : sanitizeSingleLine(req.subject);
    if (!session.sendLine(subject, error)) {
        return false;
    }

    std::string response;
    if (!session.readUntilAny({"enter", "message", "text", "/ex", "ctrl-z",
                               "error", "invalid", "reject", "disconnect", ">", "cmd"},
                              req.timeout_ms,
                              &response,
                              error)) {
        return false;
    }
    if (containsAnyCaseInsensitive(response, {"disconnect", "error", "invalid", "reject"})) {
        if (error) *error = "Winlink server rejected message body start: " + trim(response);
        return false;
    }

    if (!session.sendRaw(normalizeBbsBody(req.body), error)) {
        return false;
    }
    if (!session.sendLine("/EX", error)) {
        return false;
    }

    if (!session.readUntilAny({"queued", "posted", "accepted", "saved", "ok", ">", "cmd",
                               "error", "invalid", "reject", "disconnect", "failed"},
                              req.timeout_ms,
                              &response,
                              error)) {
        return false;
    }
    if (containsAnyCaseInsensitive(response, {"error", "invalid", "reject", "disconnect", "failed"})) {
        if (error) *error = "Winlink server did not accept message: " + trim(response);
        return false;
    }

    (void)session.sendLine("BYE", nullptr);
    return true;
}

std::vector<int> parseWinlinkListIds(const std::string& response) {
    std::set<int> ids;
    std::regex line_with_id(R"((?:^|\n)\s*([0-9]{1,6})\s+)");
    for (auto it = std::sregex_iterator(response.begin(), response.end(), line_with_id);
         it != std::sregex_iterator(); ++it) {
        int id = std::atoi((*it)[1].str().c_str());
        if (id > 0) {
            ids.insert(id);
        }
    }
    return std::vector<int>(ids.begin(), ids.end());
}

bool listWinlinkMessages(TelnetSession& session,
                         int timeout_ms,
                         std::vector<int>* ids,
                         std::string* error) {
    ids->clear();
    const std::vector<std::string> commands {"LM", "L", "LIST"};
    std::string last_response;
    for (const auto& cmd : commands) {
        if (!session.sendLine(cmd, error)) {
            return false;
        }
        std::string response;
        if (!session.readUntilAny({"no messages", "msg", "subject", "list", ">", "cmd", "disconnect"},
                                  timeout_ms,
                                  &response,
                                  error)) {
            return false;
        }
        last_response = response;
        if (containsAnyCaseInsensitive(response, {"disconnect"})) {
            if (error) *error = "Winlink server disconnected during list command";
            return false;
        }

        auto parsed = parseWinlinkListIds(response);
        if (!parsed.empty()) {
            *ids = std::move(parsed);
            return true;
        }
        if (containsAnyCaseInsensitive(response, {"no messages"})) {
            return true;
        }
    }
    if (containsAnyCaseInsensitive(last_response, {"no messages"})) {
        return true;
    }
    if (error) *error = "Unable to parse Winlink list output";
    return false;
}

bool readWinlinkMessageById(TelnetSession& session,
                            int id,
                            int timeout_ms,
                            std::string* raw,
                            std::string* error) {
    raw->clear();
    const std::vector<std::string> commands {
        "R " + std::to_string(id),
        "RM " + std::to_string(id),
        "READ " + std::to_string(id)
    };

    for (const auto& cmd : commands) {
        if (!session.sendLine(cmd, error)) {
            return false;
        }
        std::string response;
        if (!session.readUntilAny({"from:", "subject:", "to:",
                                   "message", "no such", "unknown", "invalid",
                                   ">", "cmd", "disconnect"},
                                  timeout_ms,
                                  &response,
                                  error)) {
            return false;
        }
        if (containsAnyCaseInsensitive(response, {"disconnect"})) {
            if (error) *error = "Winlink server disconnected while reading message";
            return false;
        }
        if (containsAnyCaseInsensitive(response, {"no such", "unknown", "invalid"})) {
            continue;
        }
        if (containsAnyCaseInsensitive(response, {"from:", "subject:", "to:", "\n\n"})) {
            if (!containsAnyCaseInsensitive(response, {"\n>", "\r>", "cmd"})) {
                std::string tail;
                std::string tail_error;
                if (session.readUntilAny({"\n>", "\r>", "cmd", "disconnect"},
                                         timeout_ms,
                                         &tail,
                                         &tail_error,
                                         512 * 1024)) {
                    response += tail;
                }
            }
            *raw = response;
            return true;
        }
    }
    if (error) *error = "Unable to fetch Winlink message id " + std::to_string(id);
    return false;
}

void deleteWinlinkMessageById(TelnetSession& session, int id) {
    const std::vector<std::string> commands {
        "K " + std::to_string(id),
        "KM " + std::to_string(id),
        "DEL " + std::to_string(id),
        "DELETE " + std::to_string(id)
    };
    for (const auto& cmd : commands) {
        std::string ignored_error;
        std::string ignored_response;
        if (!session.sendLine(cmd, &ignored_error)) {
            continue;
        }
        (void)session.readUntilAny({"deleted", "killed", "ok", ">", "cmd", "unknown", "invalid", "disconnect"},
                                   5000,
                                   &ignored_response,
                                   &ignored_error);
        if (containsAnyCaseInsensitive(ignored_response, {"deleted", "killed", "ok"})) {
            return;
        }
    }
}

template <typename RequestLike, typename SuccessFn>
bool runWithLoginFallbacks(const RequestLike& req,
                           SuccessFn&& fn,
                           std::string* error,
                           std::string* login_mode_used,
                           int* login_attempts) {
    const std::string host = sanitizeSingleLine(req.host);
    const std::string callsign = normalizeCallsignForAuth(req.callsign);
    const std::string password = sanitizeSingleLine(req.password);
    const uint16_t port = req.port == 0 ? static_cast<uint16_t>(8772) : req.port;
    const int timeout_ms = req.timeout_ms > 0 ? req.timeout_ms : 15000;

    if (host.empty()) {
        if (error) *error = "Winlink host is not configured";
        return false;
    }
    if (callsign.empty()) {
        if (error) *error = "Winlink callsign is not configured";
        return false;
    }
    if (password.empty()) {
        if (error) *error = "Winlink password is not configured";
        return false;
    }

    const std::vector<LoginMode> modes {
        LoginMode::AutoInteractive,
        LoginMode::ChallengeAugmented,
        LoginMode::CmsCommand
    };

    std::string last_error = "Winlink login attempts exhausted";
    int attempts = 0;
    for (LoginMode mode : modes) {
        attempts++;
        TelnetSession session;
        std::string mode_error;
        if (!session.connectTo(host, port, req.use_tls, &mode_error)) {
            last_error = mode_error;
            continue;
        }
        if (!loginWithMode(session, callsign, password, timeout_ms, mode, &mode_error)) {
            last_error = std::string("Mode ") + loginModeName(mode) + ": " + mode_error;
            session.disconnect();
            continue;
        }
        if (!fn(session, timeout_ms, mode_error)) {
            last_error = std::string("Mode ") + loginModeName(mode) + ": " + mode_error;
            session.disconnect();
            continue;
        }

        if (login_mode_used) *login_mode_used = loginModeName(mode);
        if (login_attempts) *login_attempts = attempts;
        session.disconnect();
        return true;
    }

    if (login_mode_used) *login_mode_used = "";
    if (login_attempts) *login_attempts = attempts;
    if (error) *error = last_error;
    return false;
}

}  // namespace

std::string extractWinlinkChallengeToken(const std::string& prompt) {
    // Common forms:
    //   "Password (challenge: ABC123):"
    //   "Password [ABCD1234]:"
    //   "Challenge XYZ987"
    static const std::regex kBracketPattern(R"(\[([A-Za-z0-9]{3,32})\])");
    static const std::regex kNamedPattern(R"((challenge|chal)\s*[:=]?\s*([A-Za-z0-9]{3,32}))",
                                          std::regex_constants::icase);

    std::smatch m;
    if (std::regex_search(prompt, m, kBracketPattern) && m.size() >= 2) {
        return m[1].str();
    }
    if (std::regex_search(prompt, m, kNamedPattern) && m.size() >= 3) {
        return m[2].str();
    }
    return "";
}

std::string buildWinlinkChallengeResponse(const std::string& password,
                                          const std::string& challenge_token) {
    const std::string pass = sanitizeSingleLine(password);
    const std::string token = sanitizeSingleLine(challenge_token);
    if (token.empty()) {
        return pass;
    }
    return pass + " " + token;
}

WinlinkTelnetInboundMessage parseWinlinkInboundMessage(const std::string& raw) {
    WinlinkTelnetInboundMessage msg;
    msg.raw = raw;

    // Normalize CRLF and lone-CR to LF without introducing duplicate blank lines.
    std::string normalized;
    normalized.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '\r') {
            normalized.push_back('\n');
            if (i + 1 < raw.size() && raw[i + 1] == '\n') {
                ++i;
            }
        } else {
            normalized.push_back(c);
        }
    }

    const auto lines = splitLines(normalized);
    bool in_body = false;
    bool saw_header = false;
    std::string body;
    for (const auto& line_raw : lines) {
        const std::string line = trim(line_raw);
        if (!in_body) {
            if (line.empty()) {
                if (saw_header) {
                    in_body = true;
                }
                continue;
            }
            if (!saw_header &&
                (line == ">" || startsWithCI(line, "cmd:") || startsWithCI(line, "bbs>"))) {
                continue;
            }
            if (startsWithCI(line, "from:")) {
                msg.from = trim(line.substr(5));
                saw_header = true;
                continue;
            }
            if (startsWithCI(line, "to:")) {
                msg.to = trim(line.substr(3));
                saw_header = true;
                continue;
            }
            if (startsWithCI(line, "subject:")) {
                msg.subject = trim(line.substr(8));
                saw_header = true;
                continue;
            }
            if (startsWithCI(line, "msgid:") || startsWithCI(line, "mid:") || startsWithCI(line, "id:")) {
                const size_t colon = line.find(':');
                if (colon != std::string::npos) {
                    msg.message_id = trim(line.substr(colon + 1));
                }
                saw_header = true;
                continue;
            }
            if (!saw_header) {
                continue;
            }
            // Some servers omit the blank separator line and start body immediately.
            in_body = true;
            if (line == ">" || startsWithCI(line, "cmd:") || startsWithCI(line, "bbs>")) {
                continue;
            }
            body += line_raw;
            body += "\n";
        } else {
            if (line == ">" || startsWithCI(line, "cmd:") || startsWithCI(line, "bbs>")) {
                continue;
            }
            body += line_raw;
            body += "\n";
        }
    }

    if (body.empty()) {
        // No structured headers detected; keep raw payload as body for resilience.
        if (!saw_header) {
            body = normalized;
        }
    }
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) {
        body.pop_back();
    }
    msg.body = body;
    return msg;
}

bool sendWinlinkTelnetDelivery(const WinlinkTelnetRequest& req,
                               std::string* error,
                               std::string* login_mode_used,
                               int* login_attempts) {
    const std::string to = sanitizeSingleLine(req.to);
    if (to.empty()) {
        if (error) *error = "Winlink destination is empty";
        return false;
    }

#ifdef _WIN32
    struct WinSockGuard {
        bool ok = false;
        WSADATA data {};
        WinSockGuard() { ok = WSAStartup(MAKEWORD(2, 2), &data) == 0; }
        ~WinSockGuard() { if (ok) WSACleanup(); }
    } win_sock;
    if (!win_sock.ok) {
        if (error) *error = "WSAStartup failed";
        return false;
    }
#endif

    return runWithLoginFallbacks(req,
                                 [&](TelnetSession& session, int timeout_ms, std::string& fn_error) {
                                     WinlinkTelnetRequest effective = req;
                                     effective.timeout_ms = timeout_ms;
                                     return sendInteractiveBbsMail(session, effective, &fn_error);
                                 },
                                 error,
                                 login_mode_used,
                                 login_attempts);
}

WinlinkTelnetFetchResult fetchWinlinkTelnetMessages(const WinlinkTelnetFetchRequest& req) {
    WinlinkTelnetFetchResult result;

    if (req.max_messages <= 0) {
        result.error = "Winlink max_messages must be > 0";
        return result;
    }

#ifdef _WIN32
    struct WinSockGuard {
        bool ok = false;
        WSADATA data {};
        WinSockGuard() { ok = WSAStartup(MAKEWORD(2, 2), &data) == 0; }
        ~WinSockGuard() { if (ok) WSACleanup(); }
    } win_sock;
    if (!win_sock.ok) {
        result.error = "WSAStartup failed";
        return result;
    }
#endif

    std::string run_error;
    const bool ok = runWithLoginFallbacks(req,
                                          [&](TelnetSession& session, int timeout_ms, std::string& fn_error) {
                                              std::vector<int> ids;
                                              if (!listWinlinkMessages(session, timeout_ms, &ids, &fn_error)) {
                                                  return false;
                                              }

                                              int fetched = 0;
                                              for (int id : ids) {
                                                if (fetched >= req.max_messages) {
                                                    break;
                                                }
                                                std::string raw_msg;
                                                if (!readWinlinkMessageById(session, id, timeout_ms, &raw_msg, &fn_error)) {
                                                    continue;
                                                }
                                                auto parsed = parseWinlinkInboundMessage(raw_msg);
                                                if (parsed.message_id.empty()) {
                                                    parsed.message_id = std::to_string(id);
                                                }
                                                result.messages.push_back(std::move(parsed));
                                                fetched++;
                                                if (req.delete_after_fetch) {
                                                    deleteWinlinkMessageById(session, id);
                                                }
                                              }

                                              (void)session.sendLine("BYE", nullptr);
                                              return true;
                                          },
                                          &run_error,
                                          &result.login_mode_used,
                                          &result.login_attempts);

    result.success = ok;
    if (!ok) {
        result.error = run_error;
    }
    return result;
}

}  // namespace skylink
