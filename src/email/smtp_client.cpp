// SMTP Client - Email sending via SMTP with TLS support

#include "skywave_link/smtp_client.hpp"

#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <ctime>
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

// Strip CR, LF, and null bytes from a string to prevent header injection
std::string sanitizeHeader(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        if (c != '\r' && c != '\n' && c != '\0') {
            out += c;
        }
    }
    return out;
}

// Sanitize MIME parameter value (strip quotes and control chars)
std::string sanitizeMimeParam(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        if (c != '\r' && c != '\n' && c != '\0' && c != '"' && c != '\\') {
            out += c;
        }
    }
    return out;
}

// Apply SMTP dot-stuffing: lines starting with '.' get an extra '.' prepended
std::string dotStuff(const std::string& body) {
    std::string result;
    result.reserve(body.size() + body.size() / 40);
    bool at_line_start = true;
    for (char c : body) {
        if (at_line_start && c == '.') {
            result += '.';
        }
        result += c;
        at_line_start = (c == '\n');
    }
    return result;
}

// Format RFC 5322 Date header
std::string formatDateHeader() {
    time_t now = time(nullptr);
    struct tm tm_buf;
#ifdef _WIN32
    gmtime_s(&tm_buf, &now);
#else
    gmtime_r(&now, &tm_buf);
#endif
    char buf[64];
    strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S +0000", &tm_buf);
    return std::string(buf);
}

} // namespace

struct EmailMessage {
    std::string from;
    std::string to;
    std::string subject;
    std::string body;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> attachments;
};

class SMTPClient {
public:
    SMTPClient() {
#ifdef SKYWAVE_LINK_HAS_OPENSSL
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        if (ssl_ctx_) {
            // Enable certificate verification with system CA store
            SSL_CTX_set_default_verify_paths(ssl_ctx_);
            SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_PEER, nullptr);
        }
#endif
    }

    ~SMTPClient() {
        disconnect();
#ifdef SKYWAVE_LINK_HAS_OPENSSL
        if (ssl_ctx_) {
            SSL_CTX_free(ssl_ctx_);
        }
#endif
    }

    // TLS policy:
    //   port 465 + use_tls  -> implicit TLS (handshake before the greeting)
    //   other port + use_tls -> STARTTLS is mandatory; fail closed (never fall
    //                          back to cleartext) if the server lacks it
    //   use_tls == false     -> plain connection
    bool connect(const std::string& host, uint16_t port, bool use_tls,
                 int timeout_sec, std::string* error) {
        struct addrinfo hints{}, *result = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        std::string port_str = std::to_string(port);
        if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result) != 0) {
            if (error) *error = "DNS resolution failed for " + host;
            return false;
        }

        bool connected = false;
        for (auto* ai = result; ai != nullptr; ai = ai->ai_next) {
            socket_ = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (socket_ == INVALID_SOCK) {
                continue;
            }
            if (::connect(socket_, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
                connected = true;
                break;
            }
#ifdef _WIN32
            closesocket(socket_);
#else
            close(socket_);
#endif
            socket_ = INVALID_SOCK;
        }
        freeaddrinfo(result);
        if (!connected) {
            if (error) *error = "Failed to connect to " + host + ":" + port_str;
            return false;
        }

        // Socket timeouts so a stalled server cannot hang the caller forever.
        // They also bound the TLS handshake and every SSL_read/SSL_write,
        // since OpenSSL runs over this blocking socket.
        if (timeout_sec <= 0) timeout_sec = 30;
#ifdef _WIN32
        DWORD timeout_ms = static_cast<DWORD>(timeout_sec) * 1000;
        if (setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms)) != 0 ||
            setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms)) != 0) {
#else
        struct timeval tv;
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;
        if (setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
            setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
#endif
            if (error) *error = "Failed to set socket timeouts";
            disconnect();
            return false;
        }

        connected_host_ = host;
        use_tls_ = use_tls;

        // Implicit TLS (SMTPS) on port 465: handshake before the greeting.
        if (use_tls && port == 465) {
            if (!tlsHandshake()) {
                if (error) *error = "TLS handshake failed";
                disconnect();
                return false;
            }
        }

        // Read server greeting
        std::string response = readLine();
        if (response.substr(0, 3) != "220") {
            if (error) *error = "SMTP server rejected connection: " + response;
            disconnect();
            return false;
        }

        connected_ = true;

        // Any other port with TLS requested: STARTTLS is mandatory.
        if (use_tls && !tls_active_) {
            if (!startTLS(error)) {
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

    bool startTLS(std::string* error) {
#ifdef SKYWAVE_LINK_HAS_OPENSSL
        // Send EHLO first
        if (!sendLine("EHLO localhost")) return false;
        std::string response = readLine();
        if (response.substr(0, 3) != "250") {
            if (error) *error = "EHLO rejected: " + response;
            return false;
        }

        // Read the multiline capability list, noting STARTTLS support
        bool has_starttls = false;
        while (true) {
            std::string cap = response.size() > 4 ? response.substr(4) : "";
            for (auto& ch : cap) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (cap.rfind("STARTTLS", 0) == 0) has_starttls = true;
            if (!(response.length() >= 4 && response[3] == '-')) break;
            response = readLine();
            if (response.substr(0, 3) != "250") break;
        }
        if (!has_starttls) {
            // Fail closed: never continue (and send AUTH) in cleartext.
            if (error) *error = "TLS requested but server does not offer STARTTLS";
            return false;
        }

        // Send STARTTLS
        if (!sendLine("STARTTLS")) return false;
        response = readLine();
        if (response.substr(0, 3) != "220") {
            if (error) *error = "STARTTLS rejected: " + response;
            return false;
        }

        if (!tlsHandshake()) {
            if (error) *error = "TLS handshake failed";
            return false;
        }
        return true;
#else
        std::cerr << "[SMTP] TLS not available - OpenSSL not compiled in\n";
        if (error) *error = "TLS not available - OpenSSL not compiled in";
        return false;
#endif
    }

    bool tlsHandshake() {
#ifdef SKYWAVE_LINK_HAS_OPENSSL
        if (!ssl_ctx_) return false;

        ssl_ = SSL_new(ssl_ctx_);
        if (!ssl_) return false;

        SSL_set_fd(ssl_, static_cast<int>(socket_));
        if (!connected_host_.empty()) {
            SSL_set_tlsext_host_name(ssl_, connected_host_.c_str());
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
            if (SSL_set1_host(ssl_, connected_host_.c_str()) != 1) {
                SSL_free(ssl_);
                ssl_ = nullptr;
                return false;
            }
#else
            X509_VERIFY_PARAM* verify = SSL_get0_param(ssl_);
            if (verify == nullptr ||
                X509_VERIFY_PARAM_set1_host(verify, connected_host_.c_str(), 0) != 1) {
                SSL_free(ssl_);
                ssl_ = nullptr;
                return false;
            }
#endif
        }

        if (SSL_connect(ssl_) != 1) {
            SSL_free(ssl_);
            ssl_ = nullptr;
            return false;
        }
        if (SSL_get_verify_result(ssl_) != X509_V_OK) {
            SSL_free(ssl_);
            ssl_ = nullptr;
            return false;
        }

        tls_active_ = true;
        return true;
#else
        return false;
#endif
    }

    bool authenticate(const std::string& username, const std::string& password) {
        if (!connected_) return false;
        // Never send credentials in cleartext when TLS was requested.
        if (use_tls_ && !tls_active_) return false;

        // Send EHLO (again after TLS)
        if (!sendLine("EHLO localhost")) return false;
        std::string response = readLine();
        if (response.substr(0, 3) != "250") {
            return false;
        }

        // Skip multiline response
        while (response.length() >= 4 && response[3] == '-') {
            response = readLine();
        }

        // AUTH LOGIN
        if (!sendLine("AUTH LOGIN")) return false;
        response = readLine();
        if (response.substr(0, 3) != "334") {
            return false;
        }

        // Send username (base64)
        if (!sendLine(base64Encode(username))) return false;
        response = readLine();
        if (response.substr(0, 3) != "334") {
            return false;
        }

        // Send password (base64)
        if (!sendLine(base64Encode(password))) return false;
        response = readLine();
        if (response.substr(0, 3) != "235") {
            return false;
        }

        authenticated_ = true;
        return true;
    }

    // Servers that relay without credentials (a LAN smarthost): EHLO
    // only. send() used to demand authenticate(), so such a server could
    // never be used.
    bool greet() {
        if (!connected_) return false;
        if (!sendLine("EHLO localhost")) return false;
        std::string response = readLine();
        if (response.substr(0, 3) != "250") return false;
        while (response.length() >= 4 && response[3] == '-') {
            response = readLine();
        }
        greeted_ = true;
        return true;
    }

    bool send(const EmailMessage& message) {
        if (!connected_ || !(authenticated_ || greeted_)) {
            return false;
        }

        // Sanitize all header-injectable fields
        std::string safe_from = sanitizeHeader(message.from);
        std::string safe_to = sanitizeHeader(message.to);
        std::string safe_subject = sanitizeHeader(message.subject);

        // MAIL FROM
        if (!sendLine("MAIL FROM:<" + safe_from + ">")) return false;
        std::string response = readLine();
        if (response.substr(0, 3) != "250") {
            return false;
        }

        // RCPT TO
        if (!sendLine("RCPT TO:<" + safe_to + ">")) return false;
        response = readLine();
        if (response.substr(0, 3) != "250") {
            return false;
        }

        // DATA
        if (!sendLine("DATA")) return false;
        response = readLine();
        if (response.substr(0, 3) != "354") {
            return false;
        }

        // Headers (sanitized against CRLF injection)
        sendLine("From: " + safe_from);
        sendLine("To: " + safe_to);
        sendLine("Subject: " + safe_subject);
        sendLine("Date: " + formatDateHeader());
        sendLine("MIME-Version: 1.0");

        if (message.attachments.empty()) {
            sendLine("Content-Type: text/plain; charset=UTF-8");
            sendLine("");
            sendLine(dotStuff(message.body));
        } else {
            // Multipart message with attachments
            std::string boundary = "----=_Skywave_Link_Boundary_" + std::to_string(time(nullptr));
            sendLine("Content-Type: multipart/mixed; boundary=\"" + boundary + "\"");
            sendLine("");

            // Text part
            sendLine("--" + boundary);
            sendLine("Content-Type: text/plain; charset=UTF-8");
            sendLine("Content-Transfer-Encoding: 8bit");
            sendLine("");
            sendLine(dotStuff(message.body));
            sendLine("");

            // Attachments
            for (const auto& [filename, data] : message.attachments) {
                std::string safe_name = sanitizeMimeParam(filename);
                sendLine("--" + boundary);
                sendLine("Content-Type: application/octet-stream; name=\"" + safe_name + "\"");
                sendLine("Content-Transfer-Encoding: base64");
                sendLine("Content-Disposition: attachment; filename=\"" + safe_name + "\"");
                sendLine("");
                sendLine(base64Encode(data));
                sendLine("");
            }

            sendLine("--" + boundary + "--");
        }

        if (!sendLine(".")) return false;

        response = readLine();
        if (response.substr(0, 3) != "250") {
            return false;
        }

        return true;
    }

    bool quit() {
        if (!connected_) return true;
        sendLine("QUIT");
        readLine();  // Ignore response
        disconnect();
        return true;
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
    bool authenticated_ = false;
    bool greeted_ = false;
    bool use_tls_ = true;
    bool tls_active_ = false;
    std::string connected_host_;

#ifdef SKYWAVE_LINK_HAS_OPENSSL
    SSL_CTX* ssl_ctx_ = nullptr;
    SSL* ssl_ = nullptr;
#endif

    static constexpr size_t MAX_LINE_LENGTH = 4096;

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
        std::string line;
        char c;

        while (line.size() < MAX_LINE_LENGTH) {
            int n;
#ifdef SKYWAVE_LINK_HAS_OPENSSL
            if (tls_active_ && ssl_) {
                n = SSL_read(ssl_, &c, 1);
            } else
#endif
            {
                n = recv(socket_, &c, 1, 0);
            }

            if (n != 1) break;
            if (c == '\n') break;
            if (c != '\r') line += c;
        }
        return line;
    }

    std::string base64Encode(const std::string& input) {
        std::vector<uint8_t> data(input.begin(), input.end());
        return base64Encode(data);
    }

    std::string base64Encode(const std::vector<uint8_t>& input) {
        static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string output;
        unsigned int val = 0;
        int valb = -6;

        for (unsigned char c : input) {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0) {
                output += chars[(val >> valb) & 0x3F];
                valb -= 6;
            }
        }

        if (valb > -6) {
            output += chars[((val << 8) >> (valb + 8)) & 0x3F];
        }

        while (output.size() % 4) {
            output += '=';
        }

        return output;
    }
};

bool sendSMTPDelivery(const SMTPDeliveryRequest& req, std::string* error) {
    if (req.host.empty()) {
        if (error) *error = "SMTP host is not configured";
        return false;
    }
    if (req.to.empty()) {
        if (error) *error = "Destination email address is empty";
        return false;
    }
    if (req.from.empty()) {
        if (error) *error = "Sender email address is empty";
        return false;
    }

    SMTPClient client;
    std::string connect_error;
    if (!client.connect(req.host, req.port, req.use_tls, req.timeout_sec, &connect_error)) {
        if (error) {
            *error = "Failed to connect to SMTP server";
            if (!connect_error.empty()) *error += ": " + connect_error;
        }
        return false;
    }

    if (!req.username.empty()) {
        if (!client.authenticate(req.username, req.password)) {
            if (error) *error = "SMTP authentication failed";
            client.disconnect();
            return false;
        }
    } else if (!client.greet()) {
        if (error) *error = "SMTP server rejected EHLO";
        client.disconnect();
        return false;
    }

    EmailMessage msg;
    msg.from = req.from;
    msg.to = req.to;
    msg.subject = req.subject;
    msg.body = req.body;

    if (!client.send(msg)) {
        if (error) *error = "SMTP send failed";
        client.disconnect();
        return false;
    }

    client.quit();
    return true;
}

} // namespace skylink
