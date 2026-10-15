// B2F Message Format - Binary message encoding/decoding
// Compatible with standard radio mail protocols

#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace skylink {

// B2F Message Types
enum class B2FMessageType {
    Private = 'P',
    Bulletin = 'B',
    Traffic = 'T'
};

// B2F Message Header
struct B2FHeader {
    std::string message_id;
    B2FMessageType type = B2FMessageType::Private;
    std::string from_callsign;
    std::string from_address;
    std::string to_callsign;
    std::string to_address;
    std::string subject;
    std::string date_time;
    uint32_t body_size = 0;
    std::vector<std::pair<std::string, uint32_t>> attachments;  // filename, size
};

class B2FCodec {
public:
    // Encode a message to B2F format
    std::vector<uint8_t> encode(const B2FHeader& header, const std::string& body,
                                 const std::vector<std::pair<std::string, std::vector<uint8_t>>>& attachments);

    // Decode B2F format to message components
    bool decode(const std::vector<uint8_t>& data, B2FHeader& header, std::string& body,
                std::vector<std::pair<std::string, std::vector<uint8_t>>>& attachments);
};

} // namespace skylink
