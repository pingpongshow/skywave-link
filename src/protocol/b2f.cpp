// B2F message format - the Winlink Message Structure (winlink.org/B2F).

#include "skywave_link/b2f.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>

namespace skylink {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

// Header values are single lines.
std::string headerValue(const std::string& v) {
    std::string out = v;
    for (char& c : out) {
        if (c == '\r' || c == '\n' || c == '\0') c = ' ';
    }
    return trim(out);
}

bool parseLength(const std::string& text, uint32_t* out) {
    if (text.empty() || text.size() > 10) return false;
    uint64_t v = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    if (v > 0xFFFFFFFFull) return false;
    *out = static_cast<uint32_t>(v);
    return true;
}

std::string utcNow() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y/%m/%d %H:%M", &tm);
    return buf;
}

void splitLegacy(const std::string& wire, std::string* callsign, std::string* address) {
    const std::string plain = b2fDisplayAddress(wire);
    const size_t at = plain.find('@');
    if (at == std::string::npos) {
        *callsign = plain;
        address->clear();
    } else {
        *callsign = plain.substr(0, at);
        *address = plain;
    }
}

std::string legacyAddress(const std::string& callsign, const std::string& address) {
    if (address.find('@') != std::string::npos) return address;
    if (!address.empty() && !callsign.empty()) return callsign + "@" + address;
    return callsign;
}

}  // namespace

const char* b2fTypeName(B2FMessageType type) {
    switch (type) {
        case B2FMessageType::Private: return "Private";
        case B2FMessageType::Bulletin: return "Bulletin";
        case B2FMessageType::Traffic: return "Private";
        case B2FMessageType::Service: return "Service";
        case B2FMessageType::Inquiry: return "Inquiry";
        case B2FMessageType::PositionReport: return "Position Report";
        case B2FMessageType::PositionRequest: return "Position Request";
        case B2FMessageType::Option: return "Option";
        case B2FMessageType::System: return "System";
    }
    return "Private";
}

B2FMessageType b2fTypeFromName(const std::string& name) {
    const std::string n = lower(trim(name));
    if (n == "bulletin") return B2FMessageType::Bulletin;
    if (n == "service") return B2FMessageType::Service;
    if (n == "inquiry") return B2FMessageType::Inquiry;
    if (n == "position report") return B2FMessageType::PositionReport;
    if (n == "position request") return B2FMessageType::PositionRequest;
    if (n == "option") return B2FMessageType::Option;
    if (n == "system") return B2FMessageType::System;
    return B2FMessageType::Private;
}

std::string b2fAddress(const std::string& addr_in) {
    const std::string addr = headerValue(addr_in);
    if (addr.empty()) return addr;
    const size_t colon = addr.find(':');
    const size_t at = addr.find('@');
    if (colon != std::string::npos && (at == std::string::npos || colon < at)) {
        return upper(addr.substr(0, colon)) + ":" + addr.substr(colon + 1);
    }
    if (at == std::string::npos) return upper(addr);
    if (lower(addr.substr(at + 1)) == "winlink.org") return upper(addr.substr(0, at));
    return "SMTP:" + addr;
}

std::string b2fDisplayAddress(const std::string& addr) {
    const std::string a = trim(addr);
    if (a.size() > 5 && lower(a.substr(0, 5)) == "smtp:") return a.substr(5);
    return a;
}

std::string B2FHeader::extraValue(const std::string& name) const {
    const std::string n = lower(name);
    for (const auto& [k, v] : extra) {
        if (lower(k) == n) return v;
    }
    return "";
}

bool B2FCodec::encode(const B2FHeader& header, const std::string& body,
                      const B2FAttachments& attachments,
                      std::vector<uint8_t>& out, std::string* error) {
    out.clear();
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };

    const std::string mid = headerValue(header.message_id);
    if (mid.empty() || mid.find(' ') != std::string::npos) return fail("B2F message id is empty or invalid");

    std::string from = header.from.empty()
        ? legacyAddress(header.from_callsign, header.from_address) : header.from;
    from = b2fAddress(from);

    std::vector<std::string> to;
    if (!header.to.empty()) {
        for (const auto& t : header.to) {
            if (!b2fAddress(t).empty()) to.push_back(b2fAddress(t));
        }
    } else {
        const std::string legacy = b2fAddress(legacyAddress(header.to_callsign, header.to_address));
        if (!legacy.empty()) to.push_back(legacy);
    }
    if (to.empty()) return fail("B2F message has no recipient");

    std::string mbo = headerValue(header.mbo);
    if (mbo.empty()) {
        mbo = from.find(':') == std::string::npos ? from : std::string("SMTP");
    }
    const std::string date = header.date_time.empty() ? utcNow() : headerValue(header.date_time);

    std::string h;
    h += "Mid: " + mid + "\r\n";
    h += "Date: " + date + "\r\n";
    h += std::string("Type: ") + b2fTypeName(header.type) + "\r\n";
    h += "From: " + from + "\r\n";
    for (const auto& t : to) h += "To: " + t + "\r\n";
    for (const auto& c : header.cc) {
        const std::string a = b2fAddress(c);
        if (!a.empty()) h += "Cc: " + a + "\r\n";
    }
    h += "Subject: " + headerValue(header.subject) + "\r\n";
    h += "Mbo: " + mbo + "\r\n";
    for (const auto& [k, v] : header.extra) {
        const std::string key = headerValue(k);
        const std::string lk = lower(key);
        if (key.empty() || key.find(':') != std::string::npos || lk == "mid" || lk == "date" ||
            lk == "type" || lk == "from" || lk == "to" || lk == "cc" || lk == "subject" ||
            lk == "mbo" || lk == "body" || lk == "file") {
            continue;
        }
        h += key + ": " + headerValue(v) + "\r\n";
    }
    h += "Body: " + std::to_string(body.size()) + "\r\n";
    for (const auto& [name, data] : attachments) {
        const std::string fname = headerValue(name);
        if (fname.empty()) return fail("B2F attachment without a file name");
        h += "File: " + std::to_string(data.size()) + " " + fname + "\r\n";
    }
    h += "\r\n";

    out.reserve(h.size() + body.size() + 2);
    out.insert(out.end(), h.begin(), h.end());
    out.insert(out.end(), body.begin(), body.end());
    out.push_back('\r');
    out.push_back('\n');
    for (const auto& att : attachments) {
        out.insert(out.end(), att.second.begin(), att.second.end());
        out.push_back('\r');
        out.push_back('\n');
    }
    return true;
}

std::vector<uint8_t> B2FCodec::encode(const B2FHeader& header, const std::string& body,
                                      const B2FAttachments& attachments) {
    std::vector<uint8_t> out;
    if (!encode(header, body, attachments, out, nullptr)) out.clear();
    return out;
}

bool B2FCodec::decode(const std::vector<uint8_t>& data, B2FHeader& header, std::string& body,
                      B2FAttachments& attachments, std::string* error) {
    header = B2FHeader{};
    body.clear();
    attachments.clear();
    auto fail = [&](const std::string& why) {
        header = B2FHeader{};
        body.clear();
        attachments.clear();
        if (error) *error = why;
        return false;
    };

    // Header: CRLF (or bare LF) terminated lines up to an empty line.
    size_t pos = 0;
    bool have_mid = false;
    bool have_body = false;
    bool first = true;
    for (;;) {
        const auto nl = std::find(data.begin() + static_cast<std::ptrdiff_t>(pos), data.end(),
                                  static_cast<uint8_t>('\n'));
        if (nl == data.end()) return fail("B2F header is not terminated");
        const size_t end = static_cast<size_t>(nl - data.begin());
        std::string line(data.begin() + static_cast<std::ptrdiff_t>(pos), nl);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            if (first) return fail("B2F header is empty");
            break;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0) return fail("Malformed B2F header line: " + line);
        const std::string key = lower(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        if (first && key != "mid") return fail("B2F header does not start with Mid");
        first = false;

        if (key == "mid") {
            if (have_mid || value.empty()) return fail("Bad B2F Mid field");
            header.message_id = value;
            have_mid = true;
        } else if (key == "date") {
            header.date_time = value;
        } else if (key == "type") {
            header.type_name = value;
            header.type = b2fTypeFromName(value);
        } else if (key == "from") {
            header.from = value;
        } else if (key == "to") {
            header.to.push_back(value);
        } else if (key == "cc") {
            header.cc.push_back(value);
        } else if (key == "subject") {
            header.subject = value;
        } else if (key == "mbo") {
            header.mbo = value;
        } else if (key == "body") {
            if (have_body || !parseLength(value, &header.body_size)) return fail("Bad B2F Body field");
            have_body = true;
        } else if (key == "file") {
            const size_t sp = value.find(' ');
            uint32_t size = 0;
            if (sp == std::string::npos || !parseLength(value.substr(0, sp), &size) ||
                trim(value.substr(sp + 1)).empty()) {
                return fail("Bad B2F File field");
            }
            header.attachments.emplace_back(trim(value.substr(sp + 1)), size);
        } else {
            header.extra.emplace_back(trim(line.substr(0, colon)), value);
        }
    }
    if (!have_mid) return fail("B2F message without Mid");
    if (!have_body) return fail("B2F message without Body field");

    // A section is its exact length followed by CRLF; the CRLF may be
    // missing only at the very end of the data.
    auto takeSection = [&](uint32_t size, std::vector<uint8_t>* out) -> bool {
        if (data.size() - pos < size) return false;
        out->assign(data.begin() + static_cast<std::ptrdiff_t>(pos),
                    data.begin() + static_cast<std::ptrdiff_t>(pos + size));
        pos += size;
        if (pos == data.size()) return true;
        if (data.size() - pos >= 2 && data[pos] == '\r' && data[pos + 1] == '\n') {
            pos += 2;
            return true;
        }
        return false;
    };

    std::vector<uint8_t> section;
    if (!takeSection(header.body_size, &section)) return fail("B2F body truncated or unterminated");
    body.assign(section.begin(), section.end());
    for (const auto& [name, size] : header.attachments) {
        if (pos == data.size() && size > 0) return fail("B2F attachment missing: " + name);
        if (!takeSection(size, &section)) return fail("B2F attachment truncated: " + name);
        attachments.emplace_back(name, section);
    }
    if (pos != data.size()) return fail("Trailing data after B2F message");

    if (!header.from.empty()) splitLegacy(header.from, &header.from_callsign, &header.from_address);
    if (!header.to.empty()) splitLegacy(header.to.front(), &header.to_callsign, &header.to_address);
    return true;
}

} // namespace skylink
