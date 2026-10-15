// WebServer - HTTP/WebSocket server implementation

#include "skywave_link/web_server.hpp"
#include <iostream>
#include <sstream>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <string_view>
#include <cctype>
#include <cerrno>
#include <climits>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <sys/select.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <poll.h>
#endif

#ifdef SKYWAVE_LINK_HAS_OPENSSL
    #include <openssl/sha.h>
#endif

namespace skylink {

namespace {

// Maximum WebSocket payload size: 16 MB
static constexpr size_t MAX_WS_PAYLOAD = 16 * 1024 * 1024;

#ifdef SKYWAVE_LINK_HAS_OPENSSL
std::string base64Encode(const uint8_t* data, size_t len) {
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    unsigned int val = 0;
    int valb = -6;
    for (size_t i = 0; i < len; ++i) {
        val = (val << 8) + data[i];
        valb += 8;
        while (valb >= 0) {
            out += chars[(val >> valb) & 0x3F];
            valb -= 6;
        }
    }
    if (valb > -6) {
        out += chars[((val << 8) >> (valb + 8)) & 0x3F];
    }
    while (out.size() % 4) out += '=';
    return out;
}
#endif

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string toLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool hasExactQueryTokenImpl(const std::string& query_string, const std::string& token) {
    if (query_string.empty() || token.empty()) {
        return false;
    }

    size_t start = 0;
    while (start <= query_string.size()) {
        size_t end = query_string.find('&', start);
        if (end == std::string::npos) {
            end = query_string.size();
        }
        std::string_view part(query_string.data() + start, end - start);
        size_t eq = part.find('=');
        std::string_view key = part.substr(0, eq);
        std::string_view value = (eq == std::string_view::npos)
            ? std::string_view()
            : part.substr(eq + 1);

        if (key == "token" && value == token) {
            return true;
        }
        if (end == query_string.size()) {
            break;
        }
        start = end + 1;
    }
    return false;
}

// Constant-time comparison to prevent timing attacks on auth tokens
bool constTimeEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    volatile uint8_t result = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        result |= static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i]);
    }
    return result == 0;
}

// Send all bytes, looping until complete or error
bool sendAll(int fd, const void* data, size_t len) {
    const char* ptr = static_cast<const char*>(data);
    size_t remaining = len;
    while (remaining > 0) {
        size_t chunk = std::min(remaining, static_cast<size_t>(INT32_MAX));
        int sent = ::send(fd, ptr, static_cast<int>(chunk), 0);
        if (sent < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            return false;
        }
        if (sent == 0) return false;
        ptr += sent;
        remaining -= static_cast<size_t>(sent);
    }
    return true;
}

// Close a socket portably
void closeSocket(int fd) {
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

} // anonymous namespace

// --- HttpResponse ---

void HttpResponse::setJSON(const std::string& json) {
    body = json;
    headers["Content-Type"] = "application/json";
}

void HttpResponse::setHTML(const std::string& html) {
    body = html;
    headers["Content-Type"] = "text/html; charset=utf-8";
}

void HttpResponse::setPlainText(const std::string& text) {
    body = text;
    headers["Content-Type"] = "text/plain";
}

std::string HttpResponse::serialize() const {
    std::ostringstream ss;
    ss << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";
    for (const auto& [key, value] : headers) {
        ss << key << ": " << value << "\r\n";
    }
    // Only add Content-Length if not already present in headers
    if (headers.find("Content-Length") == headers.end()) {
        ss << "Content-Length: " << body.size() << "\r\n";
    }
    ss << "Connection: close\r\n";
    ss << "\r\n";
    ss << body;
    return ss.str();
}

// --- HTTP parsing ---

bool parseHttpRequest(const std::string& raw, HttpRequest& request) {
    // Reset all fields so callers can safely reuse the same HttpRequest object.
    request = HttpRequest{};

    if (raw.empty()) {
        return false;
    }

    // Find end of headers and preserve body bytes exactly as received.
    size_t header_end = raw.find("\r\n\r\n");
    size_t delimiter_len = 4;
    if (header_end == std::string::npos) {
        header_end = raw.find("\n\n");
        delimiter_len = 2;
    }
    if (header_end == std::string::npos) {
        return false;
    }

    if (header_end + delimiter_len > raw.size()) {
        return false;
    }
    request.body.assign(raw.data() + header_end + delimiter_len,
                        raw.size() - (header_end + delimiter_len));

    std::string_view headers(raw.data(), header_end);
    if (headers.empty()) {
        return false;
    }

    // Request line: METHOD PATH VERSION
    size_t first_line_end = headers.find('\n');
    std::string_view request_line = (first_line_end == std::string::npos)
        ? headers
        : headers.substr(0, first_line_end);
    if (!request_line.empty() && request_line.back() == '\r') {
        request_line.remove_suffix(1);
    }

    std::istringstream req_line{std::string(request_line)};
    if (!(req_line >> request.method >> request.path >> request.version)) {
        return false;
    }

    // Extract query string
    auto q = request.path.find('?');
    if (q != std::string::npos) {
        request.query_string = request.path.substr(q + 1);
        request.path = request.path.substr(0, q);
    }

    // Parse headers line-by-line
    size_t pos = (first_line_end == std::string::npos) ? headers.size() : first_line_end + 1;
    while (pos < headers.size()) {
        size_t line_end = headers.find('\n', pos);
        std::string_view line = (line_end == std::string::npos)
            ? headers.substr(pos)
            : headers.substr(pos, line_end - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            break;
        }

        size_t colon = line.find(':');
        if (colon != std::string::npos && colon > 0) {
            std::string key(line.substr(0, colon));
            std::string value = trim(std::string(line.substr(colon + 1)));

            // Lowercase key for case-insensitive lookup
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            request.headers[key] = std::move(value);
        }

        if (line_end == std::string::npos) {
            break;
        }
        pos = line_end + 1;
    }

    return true;
}

bool hasQueryToken(const std::string& query_string, const std::string& token) {
    return hasExactQueryTokenImpl(query_string, token);
}

// --- WebSocket ---

std::string computeWebSocketAccept(const std::string& key) {
    // Concatenate with magic string
    std::string concat = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

#ifdef SKYWAVE_LINK_HAS_OPENSSL
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(concat.c_str()), concat.size(), hash);
    return base64Encode(hash, SHA_DIGEST_LENGTH);
#else
    (void)concat;
    return "";
#endif
}

std::vector<uint8_t> encodeWebSocketFrame(const WebSocketFrame& frame) {
    std::vector<uint8_t> out;

    // First byte: FIN + opcode
    uint8_t b0 = frame.opcode & 0x0F;
    if (frame.fin) b0 |= 0x80;
    out.push_back(b0);

    // Payload length (server frames are not masked)
    size_t len = frame.payload.size();
    if (len < 126) {
        out.push_back(static_cast<uint8_t>(len));
    } else if (len <= 0xFFFF) {
        out.push_back(126);
        out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(len & 0xFF));
    } else {
        out.push_back(127);
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<uint8_t>((len >> (i * 8)) & 0xFF));
        }
    }

    // Payload
    out.insert(out.end(), frame.payload.begin(), frame.payload.end());
    return out;
}

bool decodeWebSocketFrame(const uint8_t* data, size_t size,
                           WebSocketFrame& frame, size_t& frame_size) {
    if (size < 2) return false;

    frame.fin = (data[0] & 0x80) != 0;
    frame.opcode = data[0] & 0x0F;

    bool masked = (data[1] & 0x80) != 0;
    uint64_t payload_len = data[1] & 0x7F;
    size_t pos = 2;

    if (payload_len == 126) {
        if (size < 4) return false;
        payload_len = (static_cast<uint64_t>(data[2]) << 8) | data[3];
        pos = 4;
    } else if (payload_len == 127) {
        if (size < 10) return false;
        payload_len = 0;
        for (int i = 0; i < 8; ++i) {
            payload_len = (payload_len << 8) | data[2 + i];
        }
        pos = 10;
    }

    // Reject payloads larger than 16 MB
    if (payload_len > MAX_WS_PAYLOAD) return false;

    uint8_t mask_key[4] = {0};
    if (masked) {
        if (pos + 4 > size) return false;
        std::memcpy(mask_key, data + pos, 4);
        pos += 4;
    }

    // Safe overflow check: payload_len <= MAX_WS_PAYLOAD and pos <= size from earlier checks
    if (payload_len > size - pos) return false;

    frame.payload.resize(static_cast<size_t>(payload_len));
    for (uint64_t i = 0; i < payload_len; ++i) {
        frame.payload[i] = data[pos + i];
        if (masked) {
            frame.payload[i] ^= mask_key[i % 4];
        }
    }

    frame_size = pos + static_cast<size_t>(payload_len);
    return true;
}

// --- WebServer ---

WebServer::WebServer() = default;

WebServer::~WebServer() {
    stop();
}

void WebServer::setPort(uint16_t port) { port_ = port; }
void WebServer::setBindAddress(const std::string& addr) { bind_address_ = addr; }
void WebServer::setAuthToken(const std::string& token) {
    std::lock_guard<std::mutex> lock(routes_mutex_);
    auth_token_ = token;
}

void WebServer::addRoute(const std::string& method, const std::string& path, HttpHandler handler) {
    std::lock_guard<std::mutex> lock(routes_mutex_);
    routes_.push_back({method, path, std::move(handler)});
}

void WebServer::setWebSocketHandler(WebSocketHandler handler) {
    std::lock_guard<std::mutex> lock(routes_mutex_);
    ws_handler_ = std::move(handler);
}

bool WebServer::start() {
    if (running_) return false;
    running_ = true;
    server_thread_ = std::thread(&WebServer::serverLoop, this);
    return true;
}

void WebServer::stop() {
    if (!running_.exchange(false)) return;

    // Join the server thread first (it will exit because running_ is now false)
    if (server_thread_.joinable()) {
        server_thread_.join();
    }

    // Send WS close frames and shutdown (but do NOT close) all WS client sockets.
    // shutdown() unblocks recv() in worker threads so they can exit cleanly.
    // The worker threads themselves will call closeSocket() when they finish.
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto& [id, fd] : ws_clients_) {
            // Send close frame (opcode 8) before shutting down
            WebSocketFrame close_frame;
            close_frame.fin = true;
            close_frame.opcode = 8;
            auto encoded = encodeWebSocketFrame(close_frame);
            // Best-effort send; ignore errors
            ::send(fd, reinterpret_cast<const char*>(encoded.data()),
                   static_cast<int>(encoded.size()), 0);

#ifdef _WIN32
            shutdown(fd, SD_BOTH);
#else
            shutdown(fd, SHUT_RDWR);
#endif
        }
        // Don't clear ws_clients_ here -- workers will erase their own entries
    }
    // clients_mutex_ is released here before waiting for workers

    // Join all worker threads with a 30s timeout budget.
    // Workers should exit promptly since we called shutdown() on all their
    // sockets above, which unblocks any pending recv(). The timeout check
    // before each join() is merely a safeguard: if workers somehow stall
    // beyond the budget, we detach remaining threads to avoid blocking
    // indefinitely. Note that t.join() itself blocks without a timeout --
    // the budget check only prevents us from *starting* a new join() call
    // after 30s have elapsed.
    {
        std::vector<std::thread> threads_to_join;
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            threads_to_join = std::move(worker_threads_);
            finished_ids_.clear();
        }

        auto wait_start = std::chrono::steady_clock::now();
        for (auto& t : threads_to_join) {
            if (!t.joinable()) continue;

            // Calculate remaining time in the 30s budget
            auto elapsed = std::chrono::steady_clock::now() - wait_start;
            auto remaining = std::chrono::seconds(30) - elapsed;

            if (remaining <= std::chrono::seconds(0)) {
                // Budget exhausted -- detach remaining threads rather than
                // blocking indefinitely. This is a last resort; in practice
                // workers exit quickly after socket shutdown.
                std::cerr << "[WebServer] Timeout waiting for worker threads to finish; "
                          << active_workers_.load() << " worker(s) remain\n";
                t.detach();
                continue;
            }

            t.join();
        }

        // Final check: if any workers are still active, log a warning
        int remaining_workers = active_workers_.load();
        if (remaining_workers > 0) {
            std::cerr << "[WebServer] Warning: " << remaining_workers
                      << " worker(s) still active after shutdown\n";
        }
    }
}

bool WebServer::isRunning() const {
    return running_;
}

void WebServer::broadcast(const std::string& message) {
    WebSocketFrame frame;
    frame.fin = true;
    frame.opcode = 1;  // Text
    frame.payload.assign(message.begin(), message.end());
    auto encoded = encodeWebSocketFrame(frame);

    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (auto it = ws_clients_.begin(); it != ws_clients_.end(); ) {
        if (!sendAll(it->second, encoded.data(), encoded.size())) {
            // Do NOT close the fd here. The handleWebSocketConnection recv() will
            // naturally fail (connection is dead), and handleClient will close the fd.
            // This prevents double-close.
            it = ws_clients_.erase(it);
        } else {
            ++it;
        }
    }
}

int WebServer::wsClientCount() const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    return static_cast<int>(ws_clients_.size());
}

void WebServer::serverLoop() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::cerr << "[WebServer] Failed to create socket\n";
        running_ = false;
        return;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    inet_pton(AF_INET, bind_address_.c_str(), &addr.sin_addr);

    if (bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[WebServer] Failed to bind to " << bind_address_ << ":" << port_ << "\n";
        closeSocket(server_fd);
        running_ = false;
        return;
    }

    if (listen(server_fd, 16) < 0) {
        std::cerr << "[WebServer] Failed to listen on " << bind_address_ << ":" << port_ << "\n";
        closeSocket(server_fd);
        running_ = false;
        return;
    }

    // Non-blocking accept loop using select() on all platforms
#ifndef _WIN32
    fcntl(server_fd, F_SETFL, O_NONBLOCK);
#endif

    while (running_) {
        // Use select() with timeout on both platforms to allow clean shutdown
        fd_set accept_set;
        FD_ZERO(&accept_set);
        FD_SET(server_fd, &accept_set);
        struct timeval accept_tv {};
        accept_tv.tv_sec = 0;
        accept_tv.tv_usec = 100 * 1000;  // 100ms timeout
#ifdef _WIN32
        int ret = select(0, &accept_set, nullptr, nullptr, &accept_tv);
#else
        int ret = select(server_fd + 1, &accept_set, nullptr, nullptr, &accept_tv);
#endif
        if (ret <= 0) continue;

        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) continue;

        // Launch a joinable worker thread tracked in worker_threads_.
        active_workers_.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            // Clean up any finished threads before adding a new one.
            // Completed std::threads remain joinable until explicitly joined,
            // so we track finished thread IDs and join+erase those.
            if (!finished_ids_.empty()) {
                for (auto it = worker_threads_.begin(); it != worker_threads_.end(); ) {
                    auto id_it = std::find(finished_ids_.begin(), finished_ids_.end(), it->get_id());
                    if (id_it != finished_ids_.end()) {
                        it->join();
                        finished_ids_.erase(id_it);
                        it = worker_threads_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            worker_threads_.emplace_back([this, client_fd]() {
                handleClient(client_fd);
                {
                    std::lock_guard<std::mutex> lock(workers_mutex_);
                    finished_ids_.push_back(std::this_thread::get_id());
                }
                active_workers_.fetch_sub(1);
            });
        }
    }

    closeSocket(server_fd);
}

void WebServer::handleClient(int client_fd) {
    // Read request headers with deadline so shutdown can proceed.
    std::string raw;
    raw.reserve(4096);
    char buf[4096];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    while (running_ && std::chrono::steady_clock::now() < deadline) {
#ifdef _WIN32
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(client_fd, &readfds);
        struct timeval tv {};
        tv.tv_sec = 0;
        tv.tv_usec = 200 * 1000;
        int ready = select(0, &readfds, nullptr, nullptr, &tv);
#else
        struct pollfd pfd;
        pfd.fd = client_fd;
        pfd.events = POLLIN;
        int ready = poll(&pfd, 1, 200);
#endif
        if (ready == 0) {
            continue;
        }
        if (ready < 0) {
            break;
        }

        int n = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            break;
        }
        raw.append(buf, static_cast<size_t>(n));
        if (raw.find("\r\n\r\n") != std::string::npos) {
            break;
        }
        if (raw.size() >= 64 * 1024) {
            break;
        }
    }

    if (raw.empty() || raw.find("\r\n\r\n") == std::string::npos) {
        closeSocket(client_fd);
        return;
    }

    // Read remaining body bytes if Content-Length indicates more data
    {
        size_t header_end = raw.find("\r\n\r\n");
        size_t body_start = header_end + 4;
        size_t body_received = raw.size() - body_start;

        // Extract Content-Length from raw headers (case-insensitive search)
        size_t content_length = 0;
        std::string headers_section = raw.substr(0, header_end);
        std::string headers_lower = toLowerAscii(headers_section);
        size_t cl_pos = headers_lower.find("content-length:");
        if (cl_pos != std::string::npos) {
            size_t val_start = cl_pos + 15;  // strlen("content-length:")
            size_t val_end = headers_lower.find("\r\n", val_start);
            if (val_end == std::string::npos) val_end = headers_lower.size();
            std::string cl_str = trim(headers_section.substr(val_start, val_end - val_start));
            try {
                content_length = std::stoull(cl_str);
            } catch (...) {
                // Malformed Content-Length — reject request
                closeSocket(client_fd);
                return;
            }
        }

        // Cap at 1 MB to prevent abuse
        static constexpr size_t MAX_BODY_SIZE = 1024 * 1024;
        if (content_length > MAX_BODY_SIZE) {
            closeSocket(client_fd);
            return;
        }

        // Read remaining body bytes
        while (body_received < content_length && running_ &&
               std::chrono::steady_clock::now() < deadline) {
#ifdef _WIN32
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(client_fd, &readfds);
            struct timeval tv {};
            tv.tv_sec = 0;
            tv.tv_usec = 200 * 1000;
            int ready = select(0, &readfds, nullptr, nullptr, &tv);
#else
            struct pollfd pfd;
            pfd.fd = client_fd;
            pfd.events = POLLIN;
            int ready = poll(&pfd, 1, 200);
#endif
            if (ready == 0) continue;
            if (ready < 0) break;

            int n = recv(client_fd, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            raw.append(buf, static_cast<size_t>(n));
            body_received += static_cast<size_t>(n);
        }
    }

    HttpRequest req;
    if (!parseHttpRequest(raw, req)) {
        closeSocket(client_fd);
        return;
    }

    // Check for WebSocket upgrade
    auto upgrade_it = req.headers.find("upgrade");
    if (upgrade_it != req.headers.end() &&
        toLowerAscii(trim(upgrade_it->second)) == "websocket") {
        if (handleWebSocketUpgrade(client_fd, req)) {
            int client_id = next_client_id_.fetch_add(1);
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                ws_clients_[client_id] = client_fd;
            }
            handleWebSocketConnection(client_fd, client_id);
            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                ws_clients_.erase(client_id);
            }
        }
        closeSocket(client_fd);
        return;
    }

    // Regular HTTP request
    auto response = routeRequest(req);
    auto serialized = response.serialize();
    sendAll(client_fd, serialized.c_str(), serialized.size());

    closeSocket(client_fd);
}

bool WebServer::handleWebSocketUpgrade(int client_fd, const HttpRequest& req) {
    auto key_it = req.headers.find("sec-websocket-key");
    if (key_it == req.headers.end()) return false;

    // Auth check for WebSocket
    if (!checkAuth(req)) {
        // Send 401 response before returning
        const char* resp_401 = "HTTP/1.1 401 Unauthorized\r\n\r\n";
        sendAll(client_fd, resp_401, std::strlen(resp_401));
        return false;
    }

    std::string accept = computeWebSocketAccept(key_it->second);
    if (accept.empty()) {
        return false;
    }

    std::ostringstream response;
    response << "HTTP/1.1 101 Switching Protocols\r\n"
             << "Upgrade: websocket\r\n"
             << "Connection: Upgrade\r\n"
             << "Sec-WebSocket-Accept: " << accept << "\r\n"
             << "\r\n";

    auto resp_str = response.str();
    sendAll(client_fd, resp_str.c_str(), resp_str.size());
    return true;
}

void WebServer::handleWebSocketConnection(int client_fd, int client_id) {
    uint8_t recv_buf[4096];
    std::vector<uint8_t> accum_buf;  // Accumulation buffer for partial frame reads

    // Per-connection fragmentation state
    uint8_t frag_opcode = 0;              // Original opcode of fragmented message
    std::vector<uint8_t> frag_payload;    // Accumulated payload across fragments
    bool in_fragmented_msg = false;       // Whether we're accumulating fragments

    while (running_) {
#ifdef _WIN32
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(client_fd, &readfds);
        struct timeval tv {};
        tv.tv_sec = 0;
        tv.tv_usec = 100 * 1000;
        int ret = select(0, &readfds, nullptr, nullptr, &tv);
        if (ret <= 0) continue;
#else
        struct pollfd pfd;
        pfd.fd = client_fd;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, 100);
        if (ret <= 0) continue;
#endif

        int n = recv(client_fd, reinterpret_cast<char*>(recv_buf), sizeof(recv_buf), 0);
        if (n <= 0) break;

        // Append received data to accumulation buffer
        accum_buf.insert(accum_buf.end(), recv_buf, recv_buf + n);

        // Process complete frames from the buffer
        while (!accum_buf.empty()) {
            WebSocketFrame frame;
            size_t frame_size;
            if (!decodeWebSocketFrame(accum_buf.data(), accum_buf.size(), frame, frame_size)) {
                // Not enough data for a complete frame yet, or malformed
                // If the buffer is unreasonably large with no valid frame, disconnect
                if (accum_buf.size() > MAX_WS_PAYLOAD) {
                    accum_buf.clear();
                    return;
                }
                break;
            }

            // Consume the frame from the buffer
            accum_buf.erase(accum_buf.begin(), accum_buf.begin() + static_cast<ptrdiff_t>(frame_size));

            // Handle control frames (opcode >= 8) -- these can appear between fragments
            if (frame.opcode == 8) {
                // Echo close frame per RFC 6455
                WebSocketFrame close_reply;
                close_reply.fin = true;
                close_reply.opcode = 8;
                close_reply.payload = frame.payload;  // Echo status code
                auto close_encoded = encodeWebSocketFrame(close_reply);
                {
                    std::lock_guard<std::mutex> lock(clients_mutex_);
                    sendAll(client_fd, close_encoded.data(), close_encoded.size());
                }
                return;
            }
            if (frame.opcode == 9) {
                // Ping -> Pong (lock to serialize with broadcast writes on same fd)
                WebSocketFrame pong;
                pong.fin = true;
                pong.opcode = 10;
                pong.payload = frame.payload;
                auto encoded = encodeWebSocketFrame(pong);
                {
                    std::lock_guard<std::mutex> lock(clients_mutex_);
                    sendAll(client_fd, encoded.data(), encoded.size());
                }
                continue;
            }
            if (frame.opcode == 10) {
                // Pong -- ignore
                continue;
            }

            // Reject reserved/unknown opcodes (RFC 6455 section 5.2)
            if (frame.opcode > 10) {
                continue;  // Skip unknown opcodes
            }

            // Handle data frames with fragmentation support
            if (frame.opcode == 1 || frame.opcode == 2) {
                // Start of a new message
                if (frame.fin) {
                    // Complete single-frame message -- deliver immediately
                    if (in_fragmented_msg) {
                        // Protocol error: new data frame while fragmented message in progress
                        // Discard accumulated fragments and deliver this one
                        frag_payload.clear();
                        in_fragmented_msg = false;
                    }
                    std::lock_guard<std::mutex> lock(routes_mutex_);
                    if (ws_handler_) {
                        std::string msg(frame.payload.begin(), frame.payload.end());
                        ws_handler_(client_id, msg);
                    }
                } else {
                    // First fragment of a multi-frame message
                    in_fragmented_msg = true;
                    frag_opcode = frame.opcode;
                    frag_payload = std::move(frame.payload);
                }
            } else if (frame.opcode == 0) {
                // Continuation frame
                if (!in_fragmented_msg) {
                    // Protocol error: continuation without a started message, ignore
                    continue;
                }

                // Append payload to fragment buffer
                frag_payload.insert(frag_payload.end(),
                                    frame.payload.begin(), frame.payload.end());

                // Guard against unbounded accumulation
                if (frag_payload.size() > MAX_WS_PAYLOAD) {
                    frag_payload.clear();
                    in_fragmented_msg = false;
                    return;  // Disconnect misbehaving client
                }

                if (frame.fin) {
                    // Final fragment -- deliver the complete reassembled message
                    in_fragmented_msg = false;
                    std::lock_guard<std::mutex> lock(routes_mutex_);
                    if (ws_handler_) {
                        std::string msg(frag_payload.begin(), frag_payload.end());
                        ws_handler_(client_id, msg);
                    }
                    frag_payload.clear();
                }
            }
        }
    }
}

HttpResponse WebServer::routeRequest(const HttpRequest& req) {
    // Auth check
    if (!checkAuth(req)) {
        HttpResponse resp;
        resp.status_code = 401;
        resp.status_text = "Unauthorized";
        resp.setPlainText("Unauthorized");
        return resp;
    }

    // Find matching route
    {
        std::lock_guard<std::mutex> lock(routes_mutex_);
        for (const auto& route : routes_) {
            if (route.method == req.method && route.path == req.path) {
                return route.handler(req);
            }
        }
    }

    // 404
    HttpResponse resp;
    resp.status_code = 404;
    resp.status_text = "Not Found";
    resp.setPlainText("Not Found");
    return resp;
}

bool WebServer::checkAuth(const HttpRequest& req) const {
    // Snapshot auth_token_ under lock for thread-safe access
    std::string token_snapshot;
    {
        std::lock_guard<std::mutex> lock(routes_mutex_);
        token_snapshot = auth_token_;
    }

    if (token_snapshot.empty()) return true;  // No auth required

    auto auth_it = req.headers.find("authorization");
    if (auth_it != req.headers.end()) {
        // Expect "Bearer <token>"
        if (auth_it->second.size() > 7 && toLowerAscii(auth_it->second.substr(0, 7)) == "bearer ") {
            return constTimeEqual(auth_it->second.substr(7), token_snapshot);
        }
    }

    // Also check query parameter -- use constant-time comparison
    // We need to extract the token value from the query string and compare
    // hasExactQueryTokenImpl uses == internally, so we replicate with constTimeEqual
    if (!req.query_string.empty()) {
        size_t start = 0;
        while (start <= req.query_string.size()) {
            size_t end = req.query_string.find('&', start);
            if (end == std::string::npos) {
                end = req.query_string.size();
            }
            std::string_view part(req.query_string.data() + start, end - start);
            size_t eq = part.find('=');
            std::string_view key = part.substr(0, eq);
            if (key == "token" && eq != std::string_view::npos) {
                std::string value(part.substr(eq + 1));
                if (constTimeEqual(value, token_snapshot)) {
                    return true;
                }
            }
            if (end == req.query_string.size()) {
                break;
            }
            start = end + 1;
        }
    }

    return false;
}

} // namespace skylink
