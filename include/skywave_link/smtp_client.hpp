// SMTP helper for Skywave Link email gateway relay

#pragma once

#include <cstdint>
#include <string>

namespace skylink {

struct SMTPDeliveryRequest {
    std::string host;
    uint16_t port = 587;
    // Port 465: implicit TLS. Any other port: STARTTLS is required (the
    // connection fails rather than authenticating in cleartext).
    bool use_tls = true;
    // Socket send/receive timeout (also bounds the TLS handshake). The TCP
    // connect itself is bounded to 5 s.
    int timeout_sec = 15;

    std::string username;
    std::string password;

    std::string from;
    // Display name in the From header (station.name); empty: address only.
    std::string from_name;
    std::string to;
    std::string subject;
    std::string body;
};

bool sendSMTPDelivery(const SMTPDeliveryRequest& req, std::string* error = nullptr);

// Header helpers (exposed for testing). A header value that is not plain
// printable ASCII becomes an RFC 2047 encoded word (=?UTF-8?B?...?=); CR/LF
// never survive.
std::string encodeHeaderText(const std::string& text);
// `"Display Name" <addr>`, `=?UTF-8?B?...?= <addr>`, or just `addr`.
std::string formatFromHeader(const std::string& display_name, const std::string& address);

}  // namespace skylink
