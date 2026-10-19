// Winlink CMS client: B2F (FBB B2 forwarding protocol) over TCP/TLS.
//
// Protocol references:
//   - "Winlink Message Structure and B2 Forwarding Protocol",
//     https://winlink.org/B2F (message format, FC proposals, ;FW/;PQ/;PR).
//   - FBB forwarding protocol (http://www.f6fbb.org/protocole.html): FS
//     answers, compressed block framing, FF/FQ.
//   - la5nta/wl2k-go (MIT licence, Copyright 2015 Martin Hebnes Pedersen):
//     fbb/ and transport/telnet/ were read as the reference for CMS
//     behaviour (login prompts, handshake order, turnover semantics). The
//     secure-login salt and response computation below are adapted from
//     wl2k-go's fbb/secure.go (itself ported from paclink-unix).

#include "skywave_link/winlink_telnet_client.hpp"
#include "skywave_link/b2f.hpp"
#include "skywave_link/lzh.hpp"
#include "net_timeouts.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
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

using Clock = std::chrono::steady_clock;

constexpr const char* kTelnetPassword = "CMSTelnet";  // fixed CMS telnet login token
constexpr const char* kSid = "[SkywaveLink-1.0-B2FHM$]";
constexpr size_t kMaxLineLength = 2048;
constexpr int kMaxProposalsPerBlock = 5;
constexpr uint32_t kMaxInboundSize = 8u * 1024u * 1024u;
constexpr size_t kMaxTitleLength = 80;
constexpr size_t kBlockSize = 250;  // STX block payload (protocol max 256)
constexpr uint8_t SOH = 0x01;
constexpr uint8_t STX = 0x02;
constexpr uint8_t EOT = 0x04;

// ---------------------------------------------------------------------------
// Small string helpers

std::string trim(const std::string& s) {
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string lowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string upperAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

std::string sanitizeSingleLine(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        if (c == '\r' || c == '\n' || c == '\0') continue;
        out.push_back(c);
    }
    return trim(out);
}

bool startsWithCI(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && lowerAscii(text.substr(0, prefix.size())) == lowerAscii(prefix);
}

bool isAscii(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

// ---------------------------------------------------------------------------
// Character sets (B2F bodies default to ISO-8859-1)

// UTF-8 -> ISO-8859-1; false when a character is outside Latin-1 or the
// input is not valid UTF-8.
bool utf8ToLatin1(const std::string& in, std::string* out) {
    out->clear();
    for (size_t i = 0; i < in.size();) {
        const auto c = static_cast<unsigned char>(in[i]);
        if (c < 0x80) {
            out->push_back(static_cast<char>(c));
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < in.size() &&
                   (static_cast<unsigned char>(in[i + 1]) & 0xC0) == 0x80) {
            const unsigned cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(in[i + 1]) & 0x3Fu);
            if (cp < 0x80 || cp > 0xFF) return false;
            out->push_back(static_cast<char>(cp));
            i += 2;
        } else {
            return false;
        }
    }
    return true;
}

std::string latin1ToUtf8(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char ch : in) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x80) {
            out.push_back(ch);
        } else {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

bool validUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        size_t n = 0;
        if (c < 0x80) n = 0;
        else if ((c & 0xE0) == 0xC0) n = 1;
        else if ((c & 0xF0) == 0xE0) n = 2;
        else if ((c & 0xF8) == 0xF0) n = 3;
        else return false;
        if (s.size() - i <= n) return false;
        for (size_t k = 1; k <= n; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += n + 1;
    }
    return true;
}

std::string charsetOf(const std::string& content_type) {
    const std::string lc = lowerAscii(content_type);
    const size_t p = lc.find("charset=");
    if (p == std::string::npos) return "";
    std::string cs = lc.substr(p + 8);
    const size_t end = cs.find_first_of("; \t");
    if (end != std::string::npos) cs = cs.substr(0, end);
    cs.erase(std::remove(cs.begin(), cs.end(), '"'), cs.end());
    return cs;
}

std::string toUtf8(const std::string& bytes, const std::string& charset) {
    if (charset == "utf-8" || charset == "utf8") return bytes;
    if (charset.empty() && validUtf8(bytes)) return bytes;  // ASCII or UTF-8 without a label
    return latin1ToUtf8(bytes);                              // ISO-8859-1 (B2F default)
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string base64Decode(const std::string& in) {
    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const size_t v = chars.find(c);
        if (v == std::string::npos) continue;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

// RFC 2047 encoded words (=?charset?Q|B?text?=) -> UTF-8.
std::string decodeEncodedWords(const std::string& in) {
    std::string out;
    size_t pos = 0;
    bool last_was_word = false;
    while (pos < in.size()) {
        const size_t start = in.find("=?", pos);
        if (start == std::string::npos) {
            out += toUtf8(in.substr(pos), "");
            break;
        }
        const size_t q1 = in.find('?', start + 2);
        const size_t q2 = q1 == std::string::npos ? q1 : in.find('?', q1 + 1);
        const size_t end = q2 == std::string::npos ? q2 : in.find("?=", q2 + 1);
        if (end == std::string::npos) {
            out += toUtf8(in.substr(pos), "");
            break;
        }
        const std::string between = in.substr(pos, start - pos);
        if (!(last_was_word && trim(between).empty())) out += toUtf8(between, "");
        const std::string charset = lowerAscii(in.substr(start + 2, q1 - start - 2));
        const char enc = static_cast<char>(std::toupper(static_cast<unsigned char>(in[q1 + 1])));
        const std::string text = in.substr(q2 + 1, end - q2 - 1);
        std::string raw;
        if (enc == 'B') {
            raw = base64Decode(text);
        } else {
            for (size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '_') {
                    raw.push_back(' ');
                } else if (text[i] == '=' && i + 2 < text.size() && hexValue(text[i + 1]) >= 0 &&
                           hexValue(text[i + 2]) >= 0) {
                    raw.push_back(static_cast<char>(hexValue(text[i + 1]) * 16 + hexValue(text[i + 2])));
                    i += 2;
                } else {
                    raw.push_back(text[i]);
                }
            }
        }
        out += toUtf8(raw, charset);
        pos = end + 2;
        last_was_word = true;
    }
    return out;
}

// UTF-8 text -> one RFC 2047 Q-encoded word (ASCII input is returned as is).
std::string encodeWordQ(const std::string& utf8) {
    if (isAscii(utf8)) return utf8;
    static const char* hex = "0123456789ABCDEF";
    std::string out = "=?UTF-8?Q?";
    for (char ch : utf8) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == ' ') {
            out.push_back('_');
        } else if (c >= 0x21 && c <= 0x7E && c != '=' && c != '?' && c != '_') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('=');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    out += "?=";
    return out;
}

// Drops trailing bytes of an incomplete UTF-8 sequence.
std::string utf8Prefix(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t n = max_bytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}

std::string normalizeCrlf(const std::string& in) {
    std::string out;
    out.reserve(in.size() + in.size() / 16);
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '\r') {
            out += "\r\n";
            if (i + 1 < in.size() && in[i + 1] == '\n') ++i;
        } else if (c == '\n') {
            out += "\r\n";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string normalizeLf(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\r') {
            out.push_back('\n');
            if (i + 1 < in.size() && in[i + 1] == '\n') ++i;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// MD5 (RFC 1321), needed for the secure login response.

class Md5 {
public:
    static std::array<uint8_t, 16> digest(const std::string& data) {
        static constexpr uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static constexpr int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                      5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                      4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                      6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        std::vector<uint8_t> msg(data.begin(), data.end());
        const uint64_t bit_len = static_cast<uint64_t>(msg.size()) * 8;
        msg.push_back(0x80);
        while (msg.size() % 64 != 56) msg.push_back(0);
        for (int i = 0; i < 8; ++i) msg.push_back(static_cast<uint8_t>(bit_len >> (8 * i)));

        uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
        for (size_t off = 0; off < msg.size(); off += 64) {
            uint32_t w[16];
            for (int i = 0; i < 16; ++i) {
                w[i] = static_cast<uint32_t>(msg[off + 4 * i]) |
                       (static_cast<uint32_t>(msg[off + 4 * i + 1]) << 8) |
                       (static_cast<uint32_t>(msg[off + 4 * i + 2]) << 16) |
                       (static_cast<uint32_t>(msg[off + 4 * i + 3]) << 24);
            }
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
            for (int i = 0; i < 64; ++i) {
                uint32_t f;
                int g;
                if (i < 16) { f = (b & c) | (~b & d); g = i; }
                else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
                else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
                else { f = c ^ (b | ~d); g = (7 * i) % 16; }
                const uint32_t tmp = d;
                d = c;
                c = b;
                const uint32_t x = a + f + K[i] + w[g];
                b = b + ((x << S[i]) | (x >> (32 - S[i])));
                a = tmp;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        }
        std::array<uint8_t, 16> out{};
        for (int i = 0; i < 4; ++i) {
            for (int k = 0; k < 4; ++k) out[static_cast<size_t>(4 * i + k)] = static_cast<uint8_t>(h[i] >> (8 * k));
        }
        return out;
    }
};

// Secure login salt, from wl2k-go fbb/secure.go (found in paclink-unix).
constexpr uint8_t kSecureLoginSalt[64] = {
    77, 197, 101, 206, 190, 249, 93, 200, 51, 243, 93, 237, 71, 94, 239, 138,
    68, 108, 70, 185, 225, 137, 217, 16, 51, 122, 193, 48, 194, 195, 198, 175,
    172, 169, 70, 84, 61, 62, 104, 186, 114, 52, 61, 168, 66, 129, 192, 208,
    187, 249, 232, 193, 41, 113, 41, 45, 240, 16, 29, 228, 208, 228, 61, 20};

// ---------------------------------------------------------------------------
// Connection: TCP (+ optional TLS) with buffered, deadline-bounded reads.

class Connection {
public:
    enum class Status { Ok, Timeout, Closed, Error };

    Connection() = default;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection() { close(); }

    bool open(const std::string& host, uint16_t port, bool use_tls, int io_timeout_ms,
              std::string* error) {
        // Bounded connect (5 s); the socket's SO_RCVTIMEO/SO_SNDTIMEO bound
        // writes and the TLS handshake (see net_timeouts.hpp). Reads use
        // select() with their own deadline.
        sock_ = netio::connectTcp(host, port, netio::kConnectTimeoutMs,
                                  io_timeout_ms > 0 ? io_timeout_ms : netio::kIoTimeoutMs, error);
        if (sock_ == netio::kInvalidSocket) return false;
        if (!use_tls) return true;
#ifdef SKYWAVE_LINK_HAS_OPENSSL
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) return failTls("TLS is unavailable (SSL context init failed)", error);
        SSL_CTX_set_default_verify_paths(ctx_);
        SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        ssl_ = SSL_new(ctx_);
        if (!ssl_) return failTls("Failed to allocate TLS session", error);
        SSL_set_fd(ssl_, static_cast<int>(sock_));
        SSL_set_tlsext_host_name(ssl_, host.c_str());
        if (SSL_set1_host(ssl_, host.c_str()) != 1) {
            return failTls("Failed to configure TLS hostname verification", error);
        }
        if (SSL_connect(ssl_) != 1) return failTls("TLS handshake failed", error);
        if (SSL_get_verify_result(ssl_) != X509_V_OK) {
            return failTls("TLS certificate verification failed", error);
        }
        return true;
#else
        return failTls("TLS requested but OpenSSL is not available", error);
#endif
    }

    void close() {
#ifdef SKYWAVE_LINK_HAS_OPENSSL
        if (ssl_) {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (ctx_) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
#endif
        netio::closeSocket(sock_);
        buf_.clear();
        pos_ = 0;
    }

    bool write(const std::string& data, std::string* error) {
        size_t offset = 0;
        while (offset < data.size()) {
            int written = 0;
#ifdef SKYWAVE_LINK_HAS_OPENSSL
            if (ssl_) {
                written = SSL_write(ssl_, data.data() + offset, static_cast<int>(data.size() - offset));
            } else
#endif
            {
                int flags = 0;
#ifdef MSG_NOSIGNAL
                flags = MSG_NOSIGNAL;
#endif
                written = static_cast<int>(::send(sock_, data.data() + offset,
                                                  static_cast<int>(data.size() - offset), flags));
            }
            if (written <= 0) {
                if (error) *error = "Socket write failed";
                return false;
            }
            offset += static_cast<size_t>(written);
        }
        return true;
    }

    Status readByte(uint8_t* out, Clock::time_point deadline) {
        if (pos_ >= buf_.size()) {
            const Status s = fill(deadline);
            if (s != Status::Ok) return s;
        }
        *out = buf_[pos_++];
        return Status::Ok;
    }

    // Peeks the next byte without consuming it.
    Status peekByte(uint8_t* out, Clock::time_point deadline) {
        if (pos_ >= buf_.size()) {
            const Status s = fill(deadline);
            if (s != Status::Ok) return s;
        }
        *out = buf_[pos_];
        return Status::Ok;
    }

private:
    netio::Socket sock_ = netio::kInvalidSocket;
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
#ifdef SKYWAVE_LINK_HAS_OPENSSL
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
#endif

    bool failTls(const char* why, std::string* error) {
        if (error) *error = why;
        close();
        return false;
    }

    Status fill(Clock::time_point deadline) {
        buf_.clear();
        pos_ = 0;
        uint8_t tmp[4096];
        for (;;) {
#ifdef SKYWAVE_LINK_HAS_OPENSSL
            if (ssl_ && SSL_pending(ssl_) > 0) {
                const int n = SSL_read(ssl_, tmp, sizeof(tmp));
                if (n <= 0) return Status::Error;
                buf_.assign(tmp, tmp + n);
                return Status::Ok;
            }
#endif
            const auto now = Clock::now();
            if (now >= deadline) return Status::Timeout;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            const int wait_ms = static_cast<int>(std::min<long long>(remaining, 1000));
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sock_, &rfds);
            timeval tv{};
            tv.tv_sec = wait_ms / 1000;
            tv.tv_usec = (wait_ms % 1000) * 1000;
#ifdef _WIN32
            const int ready = select(0, &rfds, nullptr, nullptr, &tv);
#else
            const int ready = select(sock_ + 1, &rfds, nullptr, nullptr, &tv);
#endif
            if (ready == 0) continue;
            if (ready < 0) return Status::Error;
            int n = 0;
#ifdef SKYWAVE_LINK_HAS_OPENSSL
            if (ssl_) {
                n = SSL_read(ssl_, tmp, sizeof(tmp));
                if (n <= 0) {
                    const int err = SSL_get_error(ssl_, n);
                    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
                    return err == SSL_ERROR_ZERO_RETURN ? Status::Closed : Status::Error;
                }
            } else
#endif
            {
                n = static_cast<int>(::recv(sock_, reinterpret_cast<char*>(tmp), sizeof(tmp), 0));
                if (n == 0) return Status::Closed;
                if (n < 0) return Status::Error;
            }
            buf_.assign(tmp, tmp + n);
            return Status::Ok;
        }
    }
};

// ---------------------------------------------------------------------------
// B2F session

struct Proposal {
    std::string line;
    char code = 0;          // 'C' for FC
    std::string type;       // "EM", "CM"
    std::string mid;
    uint32_t usize = 0;
    uint32_t csize = 0;
    char answer = '=';
};

struct RemoteTurn {
    enum class Kind { NoMore, Quit, Proposals } kind = Kind::NoMore;
    std::vector<Proposal> proposals;
};

struct OutboundMessage {
    std::string mid;
    std::string title;
    uint32_t usize = 0;
    std::vector<uint8_t> compressed;
};

bool parseUint(const std::string& s, uint32_t* out) {
    if (s.empty() || s.size() > 10) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    if (v > 0xFFFFFFFFull) return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

std::vector<std::string> splitSpaces(const std::string& s) {
    std::vector<std::string> parts;
    std::istringstream in(s);
    std::string p;
    while (in >> p) parts.push_back(p);
    return parts;
}

class B2FSession {
public:
    B2FSession(Connection& conn, int idle_timeout_ms, Clock::time_point session_deadline)
        : conn_(conn), idle_ms_(idle_timeout_ms), session_deadline_(session_deadline) {}

    const std::string& error() const { return error_; }
    bool secureLogin() const { return secure_login_; }

    bool fail(const std::string& why) {
        if (error_.empty()) error_ = why;
        return false;
    }

    bool send(const std::string& data) {
        std::string err;
        if (!conn_.write(data, &err)) return fail(err);
        return true;
    }

    // --- Telnet login: "Callsign :" and "Password :" prompts. -------------
    bool login(const std::string& callsign) {
        std::string line;
        bool sent_call = false;
        for (int guard = 0; guard < 200; ++guard) {
            if (!readLoginLine(&line)) return false;
            const std::string lc = lowerAscii(trim(line));
            if (!lc.empty() && lc[0] == '*') return fail("Winlink CMS: " + remoteError(line));
            if (startsWithCI(lc, "callsign")) {
                if (!send(callsign + "\r")) return false;
                sent_call = true;
                continue;
            }
            if (startsWithCI(lc, "password")) {
                if (!sent_call) return fail("Winlink CMS asked for a password before the callsign");
                return send(std::string(kTelnetPassword) + "\r");
            }
            if (!lc.empty() && lc[0] == '[' && lc.back() == ']') {
                // No login prompts (e.g. a gateway that starts B2F at once).
                pending_line_ = trim(line);
                return true;
            }
        }
        return fail("Winlink CMS login prompts not seen");
    }

    // --- B2F handshake ------------------------------------------------------
    bool handshake(const std::string& callsign, const std::string& password) {
        std::string sid;
        std::string challenge;
        bool prompt = false;
        for (int guard = 0; guard < 200 && !prompt; ++guard) {
            std::string line;
            if (!pending_line_.empty()) {
                line = pending_line_;
                pending_line_.clear();
            } else if (!readLine(&line)) {
                return false;
            }
            if (line[0] == '*') return fail("Winlink CMS: " + remoteError(line));
            if (line.front() == '[' && line.back() == ']') {
                sid = upperAscii(line);
            } else if (startsWithCI(line, ";PQ:")) {
                challenge = trim(line.substr(4));
            } else if (line.back() == '>') {
                prompt = true;  // the server's handshake is complete
            }
        }
        if (!prompt) return fail("Winlink CMS handshake did not end with a prompt");
        if (sid.empty()) return fail("Winlink CMS sent no SID");
        const size_t dash = sid.rfind('-');
        const std::string features = dash == std::string::npos ? sid : sid.substr(dash + 1);
        if (features.find("B2") == std::string::npos || features.find('F') == std::string::npos) {
            return fail("Winlink server does not support the B2F protocol (SID " + sid + ")");
        }

        std::string hs = ";FW: " + callsign + "\r";
        hs += std::string(kSid) + "\r";
        if (!challenge.empty()) {
            hs += ";PR: " + buildWinlinkChallengeResponse(password, challenge) + "\r";
            secure_login_ = true;
        }
        return send(hs);
    }

    // --- Outbound -----------------------------------------------------------
    bool sendProposals(const std::vector<OutboundMessage>& msgs) {
        std::vector<std::string> lines;
        std::string block;
        for (const auto& m : msgs) {
            const std::string line = "FC EM " + m.mid + " " + std::to_string(m.usize) + " " +
                                     std::to_string(m.compressed.size()) + " 0";
            lines.push_back(line);
            block += line + "\r";
        }
        char cks[8];
        std::snprintf(cks, sizeof(cks), "%02X", winlinkProposalChecksum(lines));
        block += std::string("F> ") + cks + "\r";
        return send(block);
    }

    // Reads the FS line; answers[i] is '+', '-', '=' and offsets[i] the
    // requested resume offset.
    bool readAnswers(size_t count, std::string* answers, std::vector<uint32_t>* offsets) {
        std::string line;
        for (;;) {
            if (!readLine(&line)) return false;
            if (line[0] == ';') continue;
            if (line[0] == '*') return fail("Winlink CMS: " + remoteError(line));
            if (startsWithCI(line, "FS")) break;
            return fail("Expected a proposal answer from the CMS, got: " + line);
        }
        std::string s = trim(line.substr(2));
        answers->clear();
        offsets->clear();
        size_t i = 0;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == ' ') continue;
            switch (c) {
                case '+': case 'Y': case 'y':
                    answers->push_back('+'); offsets->push_back(0); break;
                case '-': case 'N': case 'n': case 'R': case 'r':
                    answers->push_back('-'); offsets->push_back(0); break;
                case '=': case 'L': case 'l': case 'H': case 'h':
                    answers->push_back('='); offsets->push_back(0); break;
                case '!': case 'A': case 'a': {
                    size_t j = i;
                    while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j]))) ++j;
                    uint32_t off = 0;
                    if (!parseUint(s.substr(i, j - i), &off)) return fail("Bad offset in proposal answer: " + line);
                    i = j;
                    answers->push_back('+');
                    offsets->push_back(off);
                    break;
                }
                default:
                    return fail("Invalid proposal answer: " + line);
            }
        }
        if (answers->size() != count) return fail("Proposal answer count mismatch: " + line);
        return true;
    }

    bool sendMessage(const OutboundMessage& m, uint32_t offset) {
        if (offset > m.compressed.size()) return fail("CMS requested an offset beyond the message");
        const std::string off = std::to_string(offset);
        std::string out;
        out.push_back(static_cast<char>(SOH));
        out.push_back(static_cast<char>(m.title.size() + off.size() + 2));
        out += m.title;
        out.push_back('\0');
        out += off;
        out.push_back('\0');
        uint32_t sum = 0;
        for (size_t p = offset; p < m.compressed.size(); p += kBlockSize) {
            const size_t n = std::min(kBlockSize, m.compressed.size() - p);
            out.push_back(static_cast<char>(STX));
            out.push_back(static_cast<char>(n));
            for (size_t k = 0; k < n; ++k) {
                const uint8_t b = m.compressed[p + k];
                out.push_back(static_cast<char>(b));
                sum += b;
            }
        }
        out.push_back(static_cast<char>(EOT));
        out.push_back(static_cast<char>((0x100 - (sum & 0xFF)) & 0xFF));
        return send(out);
    }

    // --- Inbound ------------------------------------------------------------
    // Reads the remote's turn: a proposal block, FF or FQ.
    bool readRemoteTurn(RemoteTurn* turn) {
        turn->proposals.clear();
        std::vector<std::string> lines;
        for (int guard = 0; guard < 1000; ++guard) {
            std::string line;
            if (!readLine(&line)) return false;
            if (line[0] == ';') continue;  // comments, ;PM pending-message notes
            if (line[0] == '*') return fail("Winlink CMS: " + remoteError(line));
            if (line.size() < 2 || line[0] != 'F') return fail("Unexpected B2F line: " + line);
            const char cmd = static_cast<char>(std::toupper(static_cast<unsigned char>(line[1])));
            if (cmd == 'F') {
                turn->kind = RemoteTurn::Kind::NoMore;
                return true;
            }
            if (cmd == 'Q') {
                turn->kind = RemoteTurn::Kind::Quit;
                return true;
            }
            if (cmd == 'A' || cmd == 'B' || cmd == 'C' || cmd == 'D') {
                if (turn->proposals.size() >= static_cast<size_t>(kMaxProposalsPerBlock) * 4) {
                    return fail("Too many proposals in one block");
                }
                Proposal p;
                p.line = line;
                p.code = cmd;
                const auto parts = splitSpaces(line.substr(2));
                if (cmd == 'C' || cmd == 'D') {
                    if (parts.size() < 4 || !parseUint(parts[2], &p.usize) || !parseUint(parts[3], &p.csize)) {
                        return fail("Malformed proposal: " + line);
                    }
                    p.type = parts[0];
                    p.mid = parts[1];
                }
                lines.push_back(line);
                turn->proposals.push_back(p);
                continue;
            }
            if (cmd == '>') {
                const std::string theirs = trim(line.substr(2));
                const uint8_t ours = winlinkProposalChecksum(lines);
                if (theirs.empty() || theirs.size() > 2 || hexValue(theirs[0]) < 0 ||
                    (theirs.size() == 2 && hexValue(theirs[1]) < 0)) {
                    return fail("Malformed proposal checksum line: " + line);
                }
                const int value = theirs.size() == 2 ? hexValue(theirs[0]) * 16 + hexValue(theirs[1])
                                                     : hexValue(theirs[0]);
                if (value != ours) return fail("Proposal block checksum error");
                turn->kind = turn->proposals.empty() ? RemoteTurn::Kind::NoMore
                                                     : RemoteTurn::Kind::Proposals;
                return true;
            }
            return fail("Unknown B2F command: " + line);
        }
        return fail("B2F proposal block too long");
    }

    bool sendAnswers(const std::vector<Proposal>& props) {
        std::string line = "FS ";
        for (const auto& p : props) line.push_back(p.answer);
        return send(line + "\r");
    }

    // Receives one compressed message (SOH header, STX blocks, EOT).
    bool receiveMessage(const Proposal& p, std::vector<uint8_t>* data) {
        data->clear();
        uint8_t b = 0;
        // Line terminators of the preceding line may still be pending.
        do {
            if (!readByte(&b)) return false;
        } while (b == '\r' || b == '\n');
        if (b == '*') {
            std::string rest;
            readLine(&rest);
            return fail("Winlink CMS: " + remoteError("*" + rest));
        }
        if (b != SOH) return fail("Expected SOH from the CMS");
        uint8_t hlen = 0;
        if (!readByte(&hlen)) return false;
        std::string header;
        for (int i = 0; i < hlen; ++i) {
            if (!readByte(&b)) return false;
            header.push_back(static_cast<char>(b));
        }
        // header = title NUL offset NUL
        const size_t nul1 = header.find('\0');
        if (nul1 == std::string::npos || header.empty() || header.back() != '\0' || nul1 + 1 >= header.size()) {
            return fail("Malformed message header from the CMS");
        }
        const std::string offset = header.substr(nul1 + 1, header.size() - nul1 - 2);
        if (offset != "0") return fail("CMS sent the message at an unrequested offset");

        uint32_t sum = 0;
        for (;;) {
            if (!readByte(&b)) return false;
            if (b == STX) {
                uint8_t n8 = 0;
                if (!readByte(&n8)) return false;
                const size_t n = n8 == 0 ? 256 : n8;
                if (data->size() + n > p.csize) return fail("Message data exceeds the proposed size");
                for (size_t i = 0; i < n; ++i) {
                    if (!readByte(&b)) return false;
                    data->push_back(b);
                    sum += b;
                }
            } else if (b == EOT) {
                uint8_t cks = 0;
                if (!readByte(&cks)) return false;
                if (((sum + cks) & 0xFF) != 0) return fail("Message block checksum error");
                if (data->size() != p.csize) return fail("Message length differs from the proposal");
                return true;
            } else {
                return fail("Unexpected byte in compressed message stream");
            }
        }
    }

    // Waits (briefly) for the remote to close after FQ.
    void drain(int ms) {
        const auto deadline = std::min(session_deadline_, Clock::now() + std::chrono::milliseconds(ms));
        uint8_t b = 0;
        while (conn_.readByte(&b, deadline) == Connection::Status::Ok) {
        }
    }

private:
    Connection& conn_;
    int idle_ms_;
    Clock::time_point session_deadline_;
    std::string error_;
    std::string pending_line_;
    bool secure_login_ = false;

    Clock::time_point deadline() const {
        return std::min(session_deadline_, Clock::now() + std::chrono::milliseconds(idle_ms_));
    }

    bool statusFail(Connection::Status s) {
        switch (s) {
            case Connection::Status::Timeout: return fail("Timed out waiting for the Winlink CMS");
            case Connection::Status::Closed: return fail("Winlink CMS closed the connection");
            default: return fail("Winlink CMS connection read failed");
        }
    }

    bool readByte(uint8_t* b) {
        const auto s = conn_.readByte(b, deadline());
        return s == Connection::Status::Ok ? true : statusFail(s);
    }

    // Next non-empty line (terminated by CR or LF).
    bool readLine(std::string* line) {
        line->clear();
        for (;;) {
            uint8_t b = 0;
            if (!readByte(&b)) return false;
            if (b == '\r' || b == '\n') {
                const std::string t = trim(*line);
                if (!t.empty()) {
                    *line = t;
                    return true;
                }
                line->clear();
                continue;
            }
            if (b == 0) continue;
            if (line->size() >= kMaxLineLength) return fail("Over-long line from the Winlink CMS");
            line->push_back(static_cast<char>(b));
        }
    }

    // Login phase: a line, or an unterminated "...:" prompt; telnet IAC
    // sequences are skipped.
    bool readLoginLine(std::string* line) {
        line->clear();
        for (;;) {
            uint8_t b = 0;
            if (!readByte(&b)) return false;
            if (b == 0xFF) {  // IAC <cmd> <opt>: ignore negotiation
                uint8_t skip = 0;
                if (!readByte(&skip) || !readByte(&skip)) return false;
                continue;
            }
            if (b == '\r' || b == '\n') {
                if (!trim(*line).empty()) return true;
                line->clear();
                continue;
            }
            if (b == 0) continue;
            if (line->size() >= kMaxLineLength) return fail("Over-long line from the Winlink CMS");
            line->push_back(static_cast<char>(b));
            const std::string t = lowerAscii(trim(*line));
            if (!t.empty() && t.back() == ':' &&
                (startsWithCI(t, "callsign") || startsWithCI(t, "password"))) {
                return true;
            }
        }
    }

    static std::string remoteError(const std::string& line) {
        const size_t idx = line.find_last_of('*');
        std::string msg = idx == std::string::npos ? line : line.substr(idx + 1);
        msg = trim(msg);
        return msg.empty() ? trim(line) : msg;
    }
};

// ---------------------------------------------------------------------------
// Session setup shared by send and fetch

struct SessionSettings {
    std::string host;
    uint16_t port = 0;
    bool use_tls = false;
    int timeout_ms = 15000;
    std::string callsign;
    std::string password;
};

template <typename Req>
bool prepareSettings(const Req& req, SessionSettings* s, std::string* error) {
    s->host = sanitizeSingleLine(req.host);
    s->port = req.port == 0 ? static_cast<uint16_t>(req.use_tls ? 8773 : 8772) : req.port;
    s->use_tls = req.use_tls;
    s->timeout_ms = req.timeout_ms > 0 ? req.timeout_ms : 15000;
    s->callsign = upperAscii(sanitizeSingleLine(req.callsign));
    s->password = sanitizeSingleLine(req.password);
    if (s->host.empty()) {
        if (error) *error = "Winlink host is not configured";
        return false;
    }
    if (s->callsign.empty() || s->callsign.find(' ') != std::string::npos) {
        if (error) *error = "Winlink callsign is not configured";
        return false;
    }
    if (s->password.empty()) {
        if (error) *error = "Winlink password is not configured";
        return false;
    }
    return true;
}

std::string loginModeName(bool secure, bool tls) {
    std::string mode = secure ? "b2f-secure-login" : "b2f";
    if (tls) mode += "/tls";
    return mode;
}

Clock::time_point sessionDeadline(int timeout_ms) {
    // The whole exchange is bounded too, not only each read.
    const int total_ms = std::max(60000, timeout_ms * 20);
    return Clock::now() + std::chrono::milliseconds(total_ms);
}

#ifdef _WIN32
struct WinSockGuard {
    bool ok = false;
    WSADATA data{};
    WinSockGuard() { ok = WSAStartup(MAKEWORD(2, 2), &data) == 0; }
    ~WinSockGuard() { if (ok) WSACleanup(); }
};
#endif

std::string generateMid() {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::random_device rd;
    std::mt19937_64 rng((static_cast<uint64_t>(rd()) << 32) ^ rd() ^
                        static_cast<uint64_t>(Clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<int> dist(0, 35);
    std::string mid;
    for (int i = 0; i < 12; ++i) mid.push_back(alphabet[dist(rng)]);
    return mid;
}

bool validMid(const std::string& mid) {
    if (mid.empty() || mid.size() > 12) return false;
    return std::all_of(mid.begin(), mid.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
    });
}

std::string utcDateNow() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y/%m/%d %H:%M", &tm);
    return buf;
}

std::vector<std::string> splitRecipients(const std::string& to) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : to) {
        if (c == ',' || c == ';') {
            if (!trim(cur).empty()) out.push_back(trim(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!trim(cur).empty()) out.push_back(trim(cur));
    return out;
}

// Builds the B2F message and its proposal data for an outbound delivery.
bool buildOutbound(const WinlinkTelnetRequest& req, const std::string& callsign,
                   OutboundMessage* out, std::string* error) {
    const auto recipients = splitRecipients(sanitizeSingleLine(req.to));
    if (recipients.empty()) {
        if (error) *error = "Winlink destination is empty";
        return false;
    }
    const std::string mid = req.message_id.empty() ? generateMid() : upperAscii(trim(req.message_id));
    if (!validMid(mid)) {
        if (error) *error = "Invalid Winlink message id";
        return false;
    }

    std::string subject = sanitizeSingleLine(req.subject);
    if (subject.empty()) subject = "(no subject)";
    if (!validUtf8(subject)) subject = latin1ToUtf8(subject);
    subject = utf8Prefix(subject, 128);
    std::string wire_subject = encodeWordQ(subject);
    while (wire_subject.size() > 128 && !subject.empty()) {
        subject = utf8Prefix(subject, subject.size() - 1);
        wire_subject = encodeWordQ(subject);
    }

    std::string body_text = normalizeCrlf(req.body);
    if (body_text.empty()) body_text = "\r\n";  // a B2F body may not be empty
    std::string body_bytes;
    std::string charset = "ISO-8859-1";
    if (!utf8ToLatin1(body_text, &body_bytes)) {
        // Outside Latin-1: send UTF-8 (or, for bytes that are not UTF-8,
        // the bytes as they are, labelled ISO-8859-1).
        body_bytes = body_text;
        if (validUtf8(body_text)) charset = "UTF-8";
    }

    B2FHeader h;
    h.message_id = mid;
    h.date_time = utcDateNow();
    h.type = B2FMessageType::Private;
    h.from = callsign;
    for (const auto& r : recipients) h.to.push_back(b2fAddress(r));
    h.subject = wire_subject;
    h.mbo = callsign;
    h.extra.emplace_back("Content-Type", "text/plain; charset=" + charset);
    h.extra.emplace_back("Content-Transfer-Encoding", "8bit");

    B2FCodec codec;
    std::vector<uint8_t> encoded;
    std::string enc_error;
    if (!codec.encode(h, body_bytes, {}, encoded, &enc_error)) {
        if (error) *error = "Cannot encode Winlink message: " + enc_error;
        return false;
    }

    out->mid = mid;
    out->usize = static_cast<uint32_t>(encoded.size());
    out->compressed = LZHCodec().compress(encoded);
    std::string title = wire_subject;
    if (title.size() > kMaxTitleLength) title = title.substr(0, kMaxTitleLength);
    out->title = title;
    return true;
}

WinlinkTelnetInboundMessage inboundFromB2F(const B2FHeader& h, const std::string& body,
                                           const B2FAttachments& attachments,
                                           const std::string& raw) {
    WinlinkTelnetInboundMessage m;
    m.message_id = h.message_id;
    m.from = b2fDisplayAddress(h.from);
    std::string to;
    for (const auto& t : h.to) {
        if (!to.empty()) to += ", ";
        to += b2fDisplayAddress(t);
    }
    m.to = to;
    std::string cc;
    for (const auto& c : h.cc) {
        if (!cc.empty()) cc += ", ";
        cc += b2fDisplayAddress(c);
    }
    m.cc = cc;
    m.subject = decodeEncodedWords(h.subject);
    m.date = h.date_time;
    std::string text = normalizeLf(toUtf8(body, charsetOf(h.extraValue("Content-Type"))));
    while (!text.empty() && text.back() == '\n') text.pop_back();
    m.body = text;
    m.raw = raw;
    for (const auto& [name, data] : attachments) {
        m.attachments.push_back({decodeEncodedWords(name), data});
    }
    return m;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public helpers

uint8_t winlinkProposalChecksum(const std::vector<std::string>& proposal_lines) {
    uint32_t sum = 0;
    for (const auto& line : proposal_lines) {
        for (char c : line) sum += static_cast<unsigned char>(c);
        sum += '\r';
    }
    return static_cast<uint8_t>((0x100 - (sum & 0xFF)) & 0xFF);
}

std::string extractWinlinkChallengeToken(const std::string& prompt) {
    std::istringstream in(prompt);
    std::string line;
    while (std::getline(in, line, '\n')) {
        // CMS lines end in CR; a buffer may hold several.
        std::istringstream sub(line);
        std::string part;
        while (std::getline(sub, part, '\r')) {
            const std::string t = trim(part);
            if (startsWithCI(t, ";PQ:")) {
                const std::string token = trim(t.substr(4));
                if (!token.empty() && token.find(' ') == std::string::npos) return token;
            }
        }
    }
    return "";
}

std::string buildWinlinkChallengeResponse(const std::string& password,
                                          const std::string& challenge_token) {
    // MD5(challenge + password + salt); the response is the low 30 bits of
    // the little-endian first word of the digest, as 8 decimal digits
    // (wl2k-go fbb/secure.go).
    std::string payload = challenge_token + password;
    payload.append(reinterpret_cast<const char*>(kSecureLoginSalt), sizeof(kSecureLoginSalt));
    const auto sum = Md5::digest(payload);
    const uint32_t pr = (static_cast<uint32_t>(sum[3] & 0x3F) << 24) |
                        (static_cast<uint32_t>(sum[2]) << 16) |
                        (static_cast<uint32_t>(sum[1]) << 8) |
                        static_cast<uint32_t>(sum[0]);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08u", pr);
    const std::string s(buf);
    return s.substr(s.size() - 8);
}

WinlinkTelnetInboundMessage parseWinlinkInboundMessage(const std::string& raw) {
    B2FCodec codec;
    B2FHeader h;
    std::string body;
    B2FAttachments attachments;
    const std::vector<uint8_t> data(raw.begin(), raw.end());
    if (codec.decode(data, h, body, attachments)) {
        return inboundFromB2F(h, body, attachments, raw);
    }

    // Not B2F: "Key: value" headers, blank line, body.
    WinlinkTelnetInboundMessage msg;
    msg.raw = raw;
    const std::string text = normalizeLf(raw);
    const size_t split = text.find("\n\n");
    const std::string head = split == std::string::npos ? text : text.substr(0, split);
    std::istringstream in(head);
    std::string line;
    bool saw_header = false;
    while (std::getline(in, line)) {
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = lowerAscii(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        if (key == "from") { msg.from = b2fDisplayAddress(value); saw_header = true; }
        else if (key == "to") { msg.to = b2fDisplayAddress(value); saw_header = true; }
        else if (key == "subject") { msg.subject = decodeEncodedWords(value); saw_header = true; }
        else if (key == "mid" || key == "msgid" || key == "id") { msg.message_id = value; saw_header = true; }
    }
    std::string body_text = saw_header ? (split == std::string::npos ? "" : text.substr(split + 2)) : text;
    while (!body_text.empty() && body_text.back() == '\n') body_text.pop_back();
    msg.body = body_text;
    return msg;
}

// ---------------------------------------------------------------------------
// Send

bool sendWinlinkTelnetDelivery(const WinlinkTelnetRequest& req,
                               std::string* error,
                               std::string* login_mode_used,
                               int* login_attempts) {
    if (login_mode_used) login_mode_used->clear();
    if (login_attempts) *login_attempts = 0;

    SessionSettings s;
    if (!prepareSettings(req, &s, error)) return false;
    OutboundMessage msg;
    if (!buildOutbound(req, s.callsign, &msg, error)) return false;

#ifdef _WIN32
    WinSockGuard win_sock;
    if (!win_sock.ok) {
        if (error) *error = "WSAStartup failed";
        return false;
    }
#endif

    Connection conn;
    std::string conn_error;
    if (login_attempts) *login_attempts = 1;
    if (!conn.open(s.host, s.port, s.use_tls, s.timeout_ms, &conn_error)) {
        if (error) *error = conn_error;
        return false;
    }
    B2FSession session(conn, s.timeout_ms, sessionDeadline(s.timeout_ms));
    auto failed = [&]() {
        if (error) *error = session.error();
        conn.close();
        return false;
    };

    if (!session.login(s.callsign) || !session.handshake(s.callsign, s.password)) return failed();
    if (login_mode_used) *login_mode_used = loginModeName(session.secureLogin(), s.use_tls);

    // Our turn: one proposal.
    if (!session.sendProposals({msg})) return failed();
    std::string answers;
    std::vector<uint32_t> offsets;
    if (!session.readAnswers(1, &answers, &offsets)) return failed();
    const char answer = answers[0];
    if (answer == '+') {
        if (!session.sendMessage(msg, offsets[0])) return failed();
    } else if (answer == '=') {
        session.fail("Winlink CMS deferred the message");
        return failed();
    }

    // The CMS taking its turn confirms receipt of the transfer.
    RemoteTurn turn;
    if (!session.readRemoteTurn(&turn)) return failed();
    // From here on the delivery is confirmed; closing errors do not matter
    // (the CMS often sends FQ and hangs up itself after a deferral).
    if (turn.kind == RemoteTurn::Kind::Proposals) {
        // Inbound mail is not taken in a delivery session (no commit gate):
        // defer it all; the next fetch picks it up.
        for (auto& p : turn.proposals) p.answer = '=';
        if (session.sendAnswers(turn.proposals)) (void)session.send("FQ\r");
    } else if (turn.kind == RemoteTurn::Kind::NoMore) {
        (void)session.send("FQ\r");
    }
    session.drain(2000);
    conn.close();

    if (answer == '-') {
        // "Already received": only meaningful for a caller-supplied id
        // (a retry of an earlier, possibly delivered attempt).
        if (req.message_id.empty()) {
            if (error) *error = "Winlink CMS rejected the message (duplicate message id)";
            return false;
        }
    }
    if (error) error->clear();
    return true;
}

// ---------------------------------------------------------------------------
// Fetch

WinlinkTelnetFetchResult fetchWinlinkTelnetMessages(const WinlinkTelnetFetchRequest& req) {
    return fetchWinlinkTelnetMessages(req, [](const WinlinkTelnetFetchResult&) { return true; });
}

WinlinkTelnetFetchResult fetchWinlinkTelnetMessages(const WinlinkTelnetFetchRequest& req,
                                                    const WinlinkCommitGate& commit) {
    WinlinkTelnetFetchResult result;
    if (req.max_messages <= 0) {
        result.error = "Winlink max_messages must be > 0";
        return result;
    }
    SessionSettings s;
    if (!prepareSettings(req, &s, &result.error)) return result;

#ifdef _WIN32
    WinSockGuard win_sock;
    if (!win_sock.ok) {
        result.error = "WSAStartup failed";
        return result;
    }
#endif

    Connection conn;
    result.login_attempts = 1;
    if (!conn.open(s.host, s.port, s.use_tls, s.timeout_ms, &result.error)) return result;

    B2FSession session(conn, s.timeout_ms, sessionDeadline(s.timeout_ms));
    auto failed = [&]() {
        result.success = false;
        result.messages.clear();
        result.error = session.error();
        conn.close();  // nothing acknowledged: the CMS keeps everything
        return result;
    };

    if (!session.login(s.callsign) || !session.handshake(s.callsign, s.password)) return failed();
    result.login_mode_used = loginModeName(session.secureLogin(), s.use_tls);

    // Our turn: nothing to send.
    if (!session.send("FF\r")) return failed();

    RemoteTurn turn;
    if (!session.readRemoteTurn(&turn)) return failed();

    if (turn.kind == RemoteTurn::Kind::Proposals) {
        std::set<std::string> seen;
        int accepted = 0;
        const int limit = std::min(req.max_messages, kMaxProposalsPerBlock);
        for (auto& p : turn.proposals) {
            const bool stopping = req.stop && req.stop->load();
            const bool ok = !stopping && p.code == 'C' && p.type == "EM" && validMid(p.mid) &&
                            seen.insert(p.mid).second && p.usize <= kMaxInboundSize &&
                            p.csize >= LZH_HEADER_SIZE && p.csize <= kMaxInboundSize && accepted < limit;
            // Never '-': that tells the CMS we already have the message.
            p.answer = ok ? '+' : '=';
            if (ok) ++accepted;
            else ++result.deferred;
        }
        if (!session.sendAnswers(turn.proposals)) return failed();

        for (const auto& p : turn.proposals) {
            if (p.answer != '+') continue;
            std::vector<uint8_t> compressed;
            if (!session.receiveMessage(p, &compressed)) return failed();
            std::vector<uint8_t> plain;
            std::string lzh_error;
            if (!LZHCodec().decompress(compressed, plain, &lzh_error, p.usize) || plain.size() != p.usize) {
                session.fail("Message " + p.mid + ": " + (lzh_error.empty() ? "size mismatch" : lzh_error));
                return failed();
            }
            B2FCodec codec;
            B2FHeader h;
            std::string body;
            B2FAttachments attachments;
            std::string b2f_error;
            if (!codec.decode(plain, h, body, attachments, &b2f_error)) {
                session.fail("Message " + p.mid + ": " + b2f_error);
                return failed();
            }
            result.messages.push_back(
                inboundFromB2F(h, body, attachments, std::string(plain.begin(), plain.end())));
        }
    }

    // Everything is received and nothing acknowledged yet: the gate decides.
    result.success = true;
    const bool stored = commit ? commit(result) : true;
    if (!stored || (!req.delete_after_fetch && !result.messages.empty())) {
        // Drop the session without the turnover: the CMS keeps the messages.
        conn.close();
        return result;
    }

    // Our turn: FQ ends the session; receiving it, the CMS marks the
    // messages delivered.
    if (turn.kind != RemoteTurn::Kind::Quit) {
        if (!session.send("FQ\r")) {
            conn.close();
            if (!result.messages.empty()) {
                // Stored, but the CMS may not have seen the acknowledgement:
                // it may offer the messages again (duplicates, never loss).
                result.error = "Acknowledgement to the Winlink CMS failed: " + session.error();
                return result;
            }
        } else {
            session.drain(2000);
        }
    }
    result.committed = true;
    conn.close();
    return result;
}

}  // namespace skylink
