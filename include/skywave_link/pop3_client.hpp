// POP3 client for Skywave Link email gateway inbound

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace skylink {

// A single retrieved POP3 message
struct POP3RetrievedMessage {
    std::string from;
    std::string to;
    std::string subject;
    std::string message_id;  // Message-ID header (for deduplication), may be empty
    std::string body;
    std::string raw;  // Full raw message for debugging
    // True when the message exceeded the size limit and the body is
    // incomplete. Truncated messages are never DELE'd from the server
    // (even with delete_after_fetch), so callers should not treat them as
    // delivered (they will be fetched again on the next poll).
    bool truncated = false;
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
    // Per read/write socket timeout (also bounds the TLS handshake). The
    // TCP connect itself is bounded to 5 s.
    int timeout_ms = 15000;

    // When set and true, no further RETR is started (the messages already
    // read are returned and the session ends normally). Lets an owner that
    // is shutting down bound the session to the operation in progress.
    const std::atomic<bool>* stop = nullptr;
};

struct POP3FetchResult {
    bool success = false;
    std::string error;
    std::vector<POP3RetrievedMessage> messages;
    // True when QUIT was answered +OK, i.e. the server committed the DELEs
    // (RFC 1939 section 6: deletions happen only in the UPDATE state).
    bool committed = false;
};

// Called after the messages are read (and, with delete_after_fetch, marked
// with DELE) but before QUIT. Return true once the messages are stored
// durably: QUIT is sent and the server deletes them. Return false to end
// the session without QUIT: the server then keeps every message.
using POP3CommitGate = std::function<bool(const POP3FetchResult&)>;

// Fetch messages from a POP3 server (deletions are committed right away).
POP3FetchResult fetchPOP3Messages(const POP3FetchRequest& req);
// Fetch with a commit gate (see POP3CommitGate).
POP3FetchResult fetchPOP3Messages(const POP3FetchRequest& req, const POP3CommitGate& commit);

// Parse a raw email message into structured fields (exposed for testing)
POP3RetrievedMessage parseEmailMessage(const std::string& raw);

// POP3 dot-unstuffing: lines beginning with ".." have the leading dot removed
// (exposed for testing)
std::string dotUnstuff(const std::string& body);

}  // namespace skylink
