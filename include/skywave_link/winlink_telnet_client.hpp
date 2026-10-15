// Winlink CMS Telnet helper for Skywave Link email gateway relay

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace skylink {

struct WinlinkTelnetRequest {
    std::string host = "server.winlink.org";
    uint16_t port = 8772;
    bool use_tls = false;
    int timeout_ms = 15000;

    std::string callsign;
    std::string password;
    std::string to;
    std::string subject;
    std::string body;
};

struct WinlinkTelnetInboundMessage {
    std::string message_id;
    std::string from;
    std::string to;
    std::string subject;
    std::string body;
    std::string raw;
};

struct WinlinkTelnetFetchRequest {
    std::string host = "server.winlink.org";
    uint16_t port = 8772;
    bool use_tls = false;
    int timeout_ms = 15000;

    std::string callsign;
    std::string password;
    int max_messages = 20;
    bool delete_after_fetch = true;
};

struct WinlinkTelnetFetchResult {
    bool success = false;
    std::string error;
    std::string login_mode_used;
    int login_attempts = 0;
    std::vector<WinlinkTelnetInboundMessage> messages;
};

// Deliver a single message through a Winlink-compatible Telnet mailbox session.
// On success, the message is accepted by the remote BBS/CMS for forwarding.
bool sendWinlinkTelnetDelivery(const WinlinkTelnetRequest& req,
                               std::string* error = nullptr,
                               std::string* login_mode_used = nullptr,
                               int* login_attempts = nullptr);

// Poll inbound messages from Winlink-compatible Telnet mailbox.
WinlinkTelnetFetchResult fetchWinlinkTelnetMessages(const WinlinkTelnetFetchRequest& req);

// Exposed helpers for tests and diagnostics.
std::string extractWinlinkChallengeToken(const std::string& prompt);
std::string buildWinlinkChallengeResponse(const std::string& password,
                                          const std::string& challenge_token);
WinlinkTelnetInboundMessage parseWinlinkInboundMessage(const std::string& raw);

}  // namespace skylink
