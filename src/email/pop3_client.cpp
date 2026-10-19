// POP3 Client - Email retrieval via POP3 with TLS support

#include "skywave_link/pop3_client.hpp"
#include "net_timeouts.hpp"

#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <unistd.h>
#endif

#ifdef SKYWAVE_LINK_HAS_OPENSSL
    #include <openssl/ssl.h>
    #include <openssl/err.h>
    #include <openssl/x509v3.h>
#endif

namespace skylink {

namespace {

// Trim leading/trailing whitespace
std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Check for POP3 "+OK" response with bounds safety
bool isOK(const std::string& resp) {
    return resp.size() >= 3 && resp.substr(0, 3) == "+OK";
}

// Case-insensitive starts-with
bool startsWithCI(const std::string& str, const std::string& prefix) {
    if (str.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(str[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    }
    return true;
}

// Unfold RFC 5322 folded headers (continuation lines start with whitespace)
std::string unfoldHeaders(const std::string& headers) {
    std::string result;
    result.reserve(headers.size());
    for (size_t i = 0; i < headers.size(); ++i) {
        if (headers[i] == '\r') continue;
        if (headers[i] == '\n' && i + 1 < headers.size() &&
            (headers[i + 1] == ' ' || headers[i + 1] == '\t')) {
            // Folded header continuation - replace newline+whitespace with single space
            result += ' ';
            ++i;  // Skip the whitespace char (loop will advance past newline)
            continue;
        }
        result += headers[i];
    }
    return result;
}

} // anonymous namespace

std::string dotUnstuff(const std::string& body) {
    std::string result;
    result.reserve(body.size());
    bool at_line_start = true;
    bool saw_dot = false;

    for (size_t i = 0; i < body.size(); ++i) {
        char c = body[i];
        if (at_line_start && c == '.' && !saw_dot) {
            saw_dot = true;
            continue;
        }
        if (saw_dot) {
            saw_dot = false;
            if (c == '.') {
                result += '.';
                at_line_start = false;
                continue;
            }
            result += '.';
        }
        result += c;
        at_line_start = (c == '\n');
    }
    if (saw_dot) {
        result += '.';
    }
    return result;
}

POP3RetrievedMessage parseEmailMessage(const std::string& raw) {
    POP3RetrievedMessage msg;
    msg.raw = raw;

    // Split headers from body at first blank line
    std::string sep_crlf = "\r\n\r\n";
    std::string sep_lf = "\n\n";
    size_t body_start = std::string::npos;

    size_t pos = raw.find(sep_crlf);
    if (pos != std::string::npos) {
        body_start = pos + 4;
    } else {
        pos = raw.find(sep_lf);
        if (pos != std::string::npos) {
            body_start = pos + 2;
        }
    }

    std::string headers_raw;
    if (body_start != std::string::npos) {
        headers_raw = raw.substr(0, pos);
        msg.body = raw.substr(body_start);
    } else {
        headers_raw = raw;
    }

    // Unfold folded headers
    std::string headers = unfoldHeaders(headers_raw);

    // Parse headers line by line
    std::istringstream hstream(headers);
    std::string line;
    while (std::getline(hstream, line)) {
        if (line.empty()) continue;

        if (startsWithCI(line, "from:")) {
            msg.from = trim(line.substr(5));
        } else if (startsWithCI(line, "to:")) {
            msg.to = trim(line.substr(3));
        } else if (startsWithCI(line, "subject:")) {
            msg.subject = trim(line.substr(8));
        } else if (startsWithCI(line, "message-id:")) {
            msg.message_id = trim(line.substr(11));
        }
    }

    // Strip trailing CRLF/LF from body
    while (!msg.body.empty() &&
           (msg.body.back() == '\r' || msg.body.back() == '\n')) {
        msg.body.pop_back();
    }

    return msg;
}

class POP3Client {
public:
    POP3Client(const POP3Client&) = delete;
    POP3Client& operator=(const POP3Client&) = delete;

    POP3Client() {
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

    ~POP3Client() {
        disconnect();
#ifdef SKYWAVE_LINK_HAS_OPENSSL
        if (ssl_ctx_) {
            SSL_CTX_free(ssl_ctx_);
        }
#endif
    }

    bool connect(const std::string& host, uint16_t port, bool use_tls,
                 int timeout_ms, std::string* error) {
#ifdef _WIN32
        WSADATA wsa_data;
        WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

        // Bounded connect (5 s) and 15 s read/write timeouts, which also
        // bound the TLS handshake (see net_timeouts.hpp).
        socket_ = netio::connectTcp(host, port, netio::kConnectTimeoutMs,
                                    timeout_ms > 0 ? timeout_ms : netio::kIoTimeoutMs, error);
        if (socket_ == INVALID_SOCK) {
            return false;
        }
        connected_ = true;
        connected_host_ = host;

        // Implicit TLS for port 995, or any non-standard port when TLS requested
        if (use_tls && port != 110) {
            if (!upgradeToTLS(error)) {
                disconnect();
                return false;
            }
        }

        // Read server greeting
        std::string greeting = readLine();
        if (!isOK(greeting)) {
            if (error) *error = "POP3 server rejected connection: " + greeting;
            disconnect();
            return false;
        }

        // STLS for port 110 with TLS
        if (use_tls && port == 110) {
            if (!sendLine("STLS")) {
                if (error) *error = "Failed to send STLS command";
                disconnect();
                return false;
            }
            std::string resp = readLine();
            if (!isOK(resp)) {
                if (error) *error = "STLS not supported: " + resp;
                disconnect();
                return false;
            }
            if (!upgradeToTLS(error)) {
                disconnect();
                return false;
            }
        }

        return true;
    }

    void disconnect() {
#ifdef SKYWAVE_LINK_HAS_OPENSSL
        if (ssl_) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
#endif
        if (socket_ != INVALID_SOCK) {
#ifdef _WIN32
            closesocket(socket_);
#else
            close(socket_);
#endif
            socket_ = INVALID_SOCK;
        }
        connected_ = false;
        tls_active_ = false;
        connected_host_.clear();
    }

    bool authenticate(const std::string& username, const std::string& password,
                      std::string* error) {
        if (!sendLine("USER " + username)) {
            if (error) *error = "Failed to send USER command";
            return false;
        }
        std::string resp = readLine();
        if (!isOK(resp)) {
            if (error) *error = "USER rejected: " + resp;
            return false;
        }

        if (!sendLine("PASS " + password)) {
            if (error) *error = "Failed to send PASS command";
            return false;
        }
        resp = readLine();
        if (!isOK(resp)) {
            if (error) *error = "Authentication failed: " + resp;
            return false;
        }

        return true;
    }

    int getMessageCount(std::string* error) {
        if (!sendLine("STAT")) {
            if (error) *error = "Failed to send STAT command";
            return -1;
        }
        std::string resp = readLine();
        if (!isOK(resp)) {
            if (error) *error = "STAT failed: " + resp;
            return -1;
        }

        int count = 0;
        if (sscanf(resp.c_str(), "+OK %d", &count) != 1 || count < 0) {
            if (error) *error = "Failed to parse STAT response";
            return -1;
        }
        return count;
    }

    // *complete is set true only when the whole message was read up to its
    // "." terminator without truncation or a read error/EOF. Callers must not
    // DELE a message that was not complete.
    std::string retrieveMessage(int msg_num, std::string* error, bool* complete) {
        if (complete) *complete = false;
        if (!sendLine("RETR " + std::to_string(msg_num))) {
            if (error) *error = "Failed to send RETR command";
            return "";
        }

        std::string first_line = readLine();
        if (!isOK(first_line)) {
            if (error) *error = "RETR failed: " + first_line;
            return "";
        }

        // Read multi-line response (dot-terminated)
        bool truncated = false;
        bool terminated = false;
        std::string body = readMultiLine(&truncated, &terminated);
        if (!terminated) {
            if (error) *error = "connection lost during RETR";
        } else if (truncated) {
            if (error) *error = "message exceeds size limit (truncated)";
        }
        if (complete) *complete = terminated && !truncated;
        return dotUnstuff(body);
    }

    bool deleteMessage(int msg_num) {
        if (!sendLine("DELE " + std::to_string(msg_num))) return false;
        std::string resp = readLine();
        return isOK(resp);
    }

    bool isConnected() const { return connected_ && !read_error_; }

    // Returns true when the server answered +OK, i.e. entered the UPDATE
    // state and committed any DELEs.
    bool quit() {
        if (!isConnected()) {
            disconnect();
            return false;
        }
        bool ok = sendLine("QUIT") && isOK(readLine());
        disconnect();
        return ok;
    }

private:
#ifdef _WIN32
    using socket_t = SOCKET;
    static constexpr socket_t INVALID_SOCK = INVALID_SOCKET;
#else
    using socket_t = int;
    static constexpr socket_t INVALID_SOCK = -1;
#endif

    socket_t socket_ = INVALID_SOCK;
    bool connected_ = false;
    bool tls_active_ = false;
    std::string connected_host_;

#ifdef SKYWAVE_LINK_HAS_OPENSSL
    SSL_CTX* ssl_ctx_ = nullptr;
    SSL* ssl_ = nullptr;
#endif

    std::string read_buffer_;
    bool read_error_ = false;

    static constexpr size_t MAX_LINE_LENGTH = 8192;
    static constexpr size_t MAX_MESSAGE_SIZE = 1024 * 1024;  // 1 MB max per message
    static constexpr size_t READ_CHUNK_SIZE = 1024;

    bool upgradeToTLS(std::string* error) {
#ifdef SKYWAVE_LINK_HAS_OPENSSL
        if (!ssl_ctx_) {
            if (error) *error = "SSL context not initialized";
            return false;
        }

        ssl_ = SSL_new(ssl_ctx_);
        if (!ssl_) {
            if (error) *error = "Failed to create SSL object";
            return false;
        }

        SSL_set_fd(ssl_, static_cast<int>(socket_));
        if (!connected_host_.empty()) {
            SSL_set_tlsext_host_name(ssl_, connected_host_.c_str());
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
            if (SSL_set1_host(ssl_, connected_host_.c_str()) != 1) {
                if (error) *error = "Failed to configure TLS hostname verification";
                SSL_free(ssl_);
                ssl_ = nullptr;
                return false;
            }
#else
            X509_VERIFY_PARAM* verify = SSL_get0_param(ssl_);
            if (verify == nullptr ||
                X509_VERIFY_PARAM_set1_host(verify, connected_host_.c_str(), 0) != 1) {
                if (error) *error = "Failed to configure TLS hostname verification";
                SSL_free(ssl_);
                ssl_ = nullptr;
                return false;
            }
#endif
        }

        if (SSL_connect(ssl_) != 1) {
            if (error) {
                unsigned long ssl_err = ERR_get_error();
                if (ssl_err != 0) {
                    char err_buf[256];
                    ERR_error_string_n(ssl_err, err_buf, sizeof(err_buf));
                    *error = std::string("TLS handshake failed: ") + err_buf;
                } else {
                    *error = "TLS handshake failed";
                }
            }
            SSL_free(ssl_);
            ssl_ = nullptr;
            return false;
        }
        if (SSL_get_verify_result(ssl_) != X509_V_OK) {
            if (error) *error = "TLS certificate verification failed";
            SSL_free(ssl_);
            ssl_ = nullptr;
            return false;
        }

        tls_active_ = true;
        return true;
#else
        if (error) *error = "TLS not available - OpenSSL not compiled in";
        return false;
#endif
    }

    bool sendLine(const std::string& line) {
        std::string data = line + "\r\n";
#ifdef SKYWAVE_LINK_HAS_OPENSSL
        if (tls_active_ && ssl_) {
            int written = SSL_write(ssl_, data.c_str(), static_cast<int>(data.size()));
            return written == static_cast<int>(data.size());
        }
#endif
        auto sent = ::send(socket_, data.c_str(), static_cast<int>(data.size()), 0);
        return sent == static_cast<int>(data.size());
    }

    std::string readLine() {
        // Check buffer for a complete line first
        while (true) {
            size_t newline_pos = read_buffer_.find('\n');
            if (newline_pos != std::string::npos) {
                // Extract line up to (not including) '\n'
                std::string line = read_buffer_.substr(0, newline_pos);
                read_buffer_.erase(0, newline_pos + 1);
                // Strip trailing '\r'
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (line.size() > MAX_LINE_LENGTH) {
                    line.resize(MAX_LINE_LENGTH);
                }
                return line;
            }

            // No complete line in buffer — read a chunk
            char chunk[READ_CHUNK_SIZE];
            int n;
#ifdef SKYWAVE_LINK_HAS_OPENSSL
            if (tls_active_ && ssl_) {
                n = SSL_read(ssl_, chunk, static_cast<int>(READ_CHUNK_SIZE));
            } else
#endif
            {
                n = recv(socket_, chunk, static_cast<int>(READ_CHUNK_SIZE), 0);
            }

            if (n <= 0) {
                // Read error, timeout or EOF (server closed the connection).
                // Either way the session is over: flag it so multi-line
                // readers terminate instead of spinning on empty lines. Any
                // partial line left in the buffer is incomplete and dropped.
                read_error_ = true;
                connected_ = false;
                read_buffer_.clear();
                return "";
            }

            read_buffer_.append(chunk, static_cast<size_t>(n));

            // Guard against unbounded buffer growth (no newline from server)
            if (read_buffer_.size() > MAX_LINE_LENGTH * 2) {
                read_error_ = true;
                read_buffer_.clear();
                return "";
            }
        }
    }

    // Read a dot-terminated multi-line response
    // Returns the body content (without the terminating ".\r\n").
    // *truncated: the size cap was hit (content beyond it was discarded).
    // *terminated: the "." terminator was seen (false on read error / EOF).
    std::string readMultiLine(bool* truncated_out = nullptr,
                              bool* terminated_out = nullptr) {
        std::string result;
        result.reserve(4096);
        bool truncated = false;
        bool terminated = false;

        while (result.size() < MAX_MESSAGE_SIZE) {
            std::string line = readLine();

            // Break on read error or EOF (readLine flags both)
            if (read_error_ || !connected_) break;

            // Terminator: a line containing just "."
            if (line == ".") {
                terminated = true;
                break;
            }

            if (!result.empty()) result += "\r\n";
            result += line;
        }

        // If we hit the size cap, drain remaining lines until the "."
        // terminator to keep the POP3 protocol in sync.
        if (!terminated && result.size() >= MAX_MESSAGE_SIZE) {
            truncated = true;
            while (!read_error_ && connected_) {
                std::string line = readLine();
                if (read_error_ || !connected_) break;
                if (line == ".") {
                    terminated = true;
                    break;
                }
            }
        }

        if (truncated_out) *truncated_out = truncated;
        if (terminated_out) *terminated_out = terminated;
        return result;
    }
};

POP3FetchResult fetchPOP3Messages(const POP3FetchRequest& req) {
    return fetchPOP3Messages(req, [](const POP3FetchResult&) { return true; });
}

POP3FetchResult fetchPOP3Messages(const POP3FetchRequest& req, const POP3CommitGate& commit) {
    POP3FetchResult result;

    if (req.host.empty()) {
        result.error = "POP3 host is not configured";
        return result;
    }
    if (req.max_messages <= 0) {
        result.error = "max_messages must be positive";
        return result;
    }

    POP3Client client;
    if (!client.connect(req.host, req.port, req.use_tls, req.timeout_ms, &result.error)) {
        return result;
    }

    if (!client.authenticate(req.username, req.password, &result.error)) {
        client.disconnect();
        return result;
    }

    int count = client.getMessageCount(&result.error);
    if (count < 0) {
        client.disconnect();
        return result;
    }

    int fetch_count = std::min(count, req.max_messages);
    std::string errors_accumulated;

    for (int i = 1; i <= fetch_count; ++i) {
        if (req.stop && req.stop->load()) {
            break;  // owner is shutting down: keep what was read, end cleanly
        }
        std::string msg_error;
        bool complete = false;
        std::string raw = client.retrieveMessage(i, &msg_error, &complete);
        if (!msg_error.empty()) {
            if (!errors_accumulated.empty()) errors_accumulated += "; ";
            errors_accumulated += "msg " + std::to_string(i) + ": " + msg_error;
        }
        if (!client.isConnected()) {
            // Connection lost mid-RETR: the partial message is discarded and
            // never DELE'd, so it stays on the server for the next poll.
            break;
        }
        if (raw.empty()) {
            continue;
        }

        result.messages.push_back(parseEmailMessage(raw));
        result.messages.back().truncated = !complete;

        // Never delete a message we did not receive in full.
        if (req.delete_after_fetch && complete) {
            if (!client.deleteMessage(i)) {
                if (!errors_accumulated.empty()) errors_accumulated += "; ";
                errors_accumulated += "msg " + std::to_string(i) + ": DELE failed";
            }
        }
    }

    if (!result.messages.empty() || count == 0) {
        result.success = true;
    }
    if (!errors_accumulated.empty()) {
        result.error = errors_accumulated;
    }

    // The DELEs above only mark messages; the server removes them when QUIT
    // moves the session to UPDATE. Commit only once the caller holds the
    // messages; otherwise drop the connection and the server keeps them.
    if (commit && commit(result)) {
        result.committed = client.quit();
    } else {
        client.disconnect();
    }

    return result;
}

} // namespace skylink
