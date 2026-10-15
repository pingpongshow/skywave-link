// WebServer - Lightweight HTTP/WebSocket server for remote access
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace skylink {

// HTTP request
struct HttpRequest {
    std::string method;    // GET, POST, etc.
    std::string path;      // /api/status
    std::string version;   // HTTP/1.1
    std::map<std::string, std::string> headers;
    std::string body;
    std::string query_string;
};

// HTTP response
struct HttpResponse {
    int status_code = 200;
    std::string status_text = "OK";
    std::map<std::string, std::string> headers;
    std::string body;

    void setJSON(const std::string& json);
    void setHTML(const std::string& html);
    void setPlainText(const std::string& text);
    std::string serialize() const;
};

// WebSocket frame
struct WebSocketFrame {
    bool fin = true;
    uint8_t opcode = 1;  // 1=text, 2=binary, 8=close, 9=ping, 10=pong
    std::vector<uint8_t> payload;
};

// Route handler
using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

// WebSocket message handler
using WebSocketHandler = std::function<void(int client_id, const std::string& message)>;

// Parse HTTP request from raw data
bool parseHttpRequest(const std::string& raw, HttpRequest& request);

// Check for exact `token=<value>` in query string.
bool hasQueryToken(const std::string& query_string, const std::string& token);

// WebSocket handshake key computation (SHA-1 based)
std::string computeWebSocketAccept(const std::string& key);

// WebSocket frame encoding/decoding
std::vector<uint8_t> encodeWebSocketFrame(const WebSocketFrame& frame);
bool decodeWebSocketFrame(const uint8_t* data, size_t size,
                           WebSocketFrame& frame, size_t& frame_size);

class WebServer {
public:
    WebServer();
    ~WebServer();

    // Non-copyable
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // Configuration
    void setPort(uint16_t port);
    void setBindAddress(const std::string& addr);
    void setAuthToken(const std::string& token);

    // Route registration
    void addRoute(const std::string& method, const std::string& path, HttpHandler handler);
    void setWebSocketHandler(WebSocketHandler handler);

    // Server control
    bool start();
    void stop();
    bool isRunning() const;

    // WebSocket broadcast (send to all connected WS clients)
    void broadcast(const std::string& message);

    // Get number of connected WebSocket clients
    int wsClientCount() const;

private:
    uint16_t port_ = 8080;
    std::string bind_address_ = "127.0.0.1";
    std::string auth_token_;

    std::atomic<bool> running_{false};
    std::thread server_thread_;

    // Worker threads for clean shutdown (joinable, not detached)
    std::atomic<int> active_workers_{0};
    std::mutex workers_mutex_;
    std::vector<std::thread> worker_threads_;
    std::vector<std::thread::id> finished_ids_;  // IDs of workers that have completed

    // Route table
    struct Route {
        std::string method;
        std::string path;
        HttpHandler handler;
    };
    std::vector<Route> routes_;
    mutable std::mutex routes_mutex_;

    WebSocketHandler ws_handler_;

    mutable std::mutex clients_mutex_;
    std::map<int, int> ws_clients_;  // client_id -> socket_fd
    std::atomic<int> next_client_id_{1};

    // Server loop
    void serverLoop();
    void handleClient(int client_fd);
    bool handleWebSocketUpgrade(int client_fd, const HttpRequest& req);
    void handleWebSocketConnection(int client_fd, int client_id);
    HttpResponse routeRequest(const HttpRequest& req);
    bool checkAuth(const HttpRequest& req) const;
};

} // namespace skylink
