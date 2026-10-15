// POP3 client for Skywave Link email gateway inbound

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace skylink {

// A single retrieved POP3 message
struct POP3RetrievedMessage {
    std::string from;
    std::string to;
    std::string subject;
    std::string body;
    std::string raw;  // Full raw message for debugging
};

// Configuration for POP3 fetch
struct POP3FetchRequest {
    std::string host;
    uint16_t port = 995;
    bool use_tls = true;

    std::string username;
    std::string password;

    bool delete_after_fetch = false;
    int max_messages = 50;
};

struct POP3FetchResult {
    bool success = false;
    std::string error;
    std::vector<POP3RetrievedMessage> messages;
};

// Fetch messages from a POP3 server
POP3FetchResult fetchPOP3Messages(const POP3FetchRequest& req);

// Parse a raw email message into structured fields (exposed for testing)
POP3RetrievedMessage parseEmailMessage(const std::string& raw);

// POP3 dot-unstuffing: lines beginning with ".." have the leading dot removed
// (exposed for testing)
std::string dotUnstuff(const std::string& body);

}  // namespace skylink
