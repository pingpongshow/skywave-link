// SMTP helper for Skywave Link email gateway relay

#pragma once

#include <cstdint>
#include <string>

namespace skylink {

struct SMTPDeliveryRequest {
    std::string host;
    uint16_t port = 587;
    bool use_tls = true;

    std::string username;
    std::string password;

    std::string from;
    std::string to;
    std::string subject;
    std::string body;
};

bool sendSMTPDelivery(const SMTPDeliveryRequest& req, std::string* error = nullptr);

}  // namespace skylink
