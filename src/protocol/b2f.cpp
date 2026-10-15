// B2F Message Format - Binary message encoding/decoding
// Compatible with standard radio mail protocols

#include "skywave_link/b2f.hpp"
#include <sstream>

namespace skylink {

std::vector<uint8_t> B2FCodec::encode(const B2FHeader& header, const std::string& body,
                                       const std::vector<std::pair<std::string, std::vector<uint8_t>>>& attachments) {
    std::vector<uint8_t> result;

    // Build header
    std::stringstream ss;
    ss << "MID: " << header.message_id << "\r\n";
    ss << "Date: " << header.date_time << "\r\n";
    ss << "Type: " << static_cast<char>(header.type) << "\r\n";
    ss << "From: " << header.from_callsign;
    if (!header.from_address.empty()) ss << "@" << header.from_address;
    ss << "\r\n";
    ss << "To: " << header.to_callsign;
    if (!header.to_address.empty()) ss << "@" << header.to_address;
    ss << "\r\n";
    ss << "Subject: " << header.subject << "\r\n";
    ss << "Body: " << body.size() << "\r\n";

    for (const auto& [filename, data] : attachments) {
        ss << "File: " << data.size() << " " << filename << "\r\n";
    }

    ss << "\r\n";  // End of header

    // Add header to result
    std::string header_str = ss.str();
    result.insert(result.end(), header_str.begin(), header_str.end());

    // Add body
    result.insert(result.end(), body.begin(), body.end());
    result.push_back('\r');
    result.push_back('\n');

    // Add attachments
    for (const auto& [filename, data] : attachments) {
        result.insert(result.end(), data.begin(), data.end());
        result.push_back('\r');
        result.push_back('\n');
    }

    return result;
}

bool B2FCodec::decode(const std::vector<uint8_t>& data, B2FHeader& header, std::string& body,
                       std::vector<std::pair<std::string, std::vector<uint8_t>>>& attachments) {
    // Find end of header (blank line)
    std::string content(data.begin(), data.end());
    size_t header_end = content.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return false;
    }

    // Parse header
    std::istringstream ss(content.substr(0, header_end));
    std::string line;
    std::vector<std::pair<std::string, uint32_t>> file_info;

    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        size_t colon = line.find(": ");
        if (colon == std::string::npos) continue;

        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 2);

        if (key == "MID") {
            header.message_id = value;
        } else if (key == "Date") {
            header.date_time = value;
        } else if (key == "Type" && !value.empty()) {
            header.type = static_cast<B2FMessageType>(value[0]);
        } else if (key == "From") {
            size_t at = value.find('@');
            if (at != std::string::npos) {
                header.from_callsign = value.substr(0, at);
                header.from_address = value.substr(at + 1);
            } else {
                header.from_callsign = value;
            }
        } else if (key == "To") {
            size_t at = value.find('@');
            if (at != std::string::npos) {
                header.to_callsign = value.substr(0, at);
                header.to_address = value.substr(at + 1);
            } else {
                header.to_callsign = value;
            }
        } else if (key == "Subject") {
            header.subject = value;
        } else if (key == "Body") {
            try { header.body_size = static_cast<uint32_t>(std::stoul(value)); }
            catch (...) { return false; }
        } else if (key == "File") {
            size_t space = value.find(' ');
            if (space != std::string::npos) {
                try {
                    uint32_t size = static_cast<uint32_t>(std::stoul(value.substr(0, space)));
                    std::string filename = value.substr(space + 1);
                    file_info.push_back({filename, size});
                } catch (...) { return false; }
            }
        }
    }

    // Extract body
    size_t body_start = header_end + 4;
    if (body_start + header.body_size <= content.size()) {
        body = content.substr(body_start, header.body_size);
    }

    // Extract attachments
    size_t attach_start = body_start + header.body_size + 2;  // +2 for CRLF
    for (const auto& [filename, size] : file_info) {
        if (attach_start + size <= data.size()) {
            std::vector<uint8_t> attach_data(data.begin() + attach_start,
                                              data.begin() + attach_start + size);
            attachments.push_back({filename, attach_data});
            attach_start += size + 2;  // +2 for CRLF
        }
    }

    header.attachments = file_info;
    return true;
}

} // namespace skylink
