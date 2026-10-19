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
    std::string to;
    std::string subject;
    std::string body;
};

bool sendSMTPDelivery(const SMTPDeliveryRequest& req, std::string* error = nullptr);

}  // namespace skylink
