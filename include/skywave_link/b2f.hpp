// B2F message format - the Winlink Message Structure (winlink.org/B2F).
//
//   Mid: <unique id, up to 12 characters>       (must be the first line)
//   Date: YYYY/MM/DD HH:MM                       (UTC)
//   Type: Private | Bulletin | Service | ...
//   From: <callsign> | SMTP:<internet address>
//   To: ...            (one line per recipient, at least one)
//   Cc: ...
//   Subject: <up to 128 characters>
//   Mbo: <originating mailbox: callsign, SMTP or CMBO>
//   Body: <exact body length in bytes>
//   File: <exact length> <file name>            (one per attachment, in order)
//   <blank line>
//   <body bytes> CRLF
//   <attachment bytes> CRLF   (for each attachment)
//
// Header lines end in CRLF; unknown header fields are preserved and
// otherwise ignored. Decoding is strict: a missing Mid or Body field, a
// malformed length, a short body/attachment or a missing section terminator
// makes decode() fail.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace skylink {

// Message type (the "Type:" field). The wire form is the word, e.g.
// "Private". Traffic is kept for existing callers and is sent as Private.
enum class B2FMessageType {
    Private = 'P',
    Bulletin = 'B',
    Traffic = 'T',
    Service = 'S',
    Inquiry = 'I',
    PositionReport = 'R',
    PositionRequest = 'Q',
    Option = 'O',
    System = 'Y'
};

const char* b2fTypeName(B2FMessageType type);
B2FMessageType b2fTypeFromName(const std::string& name);

// Winlink address form of `addr`: a bare callsign (upper-cased), the
// callsign for "<call>@winlink.org", "SMTP:<addr>" for any other internet
// address; values that already carry a "PROTO:" prefix are kept.
std::string b2fAddress(const std::string& addr);
// Strips the "SMTP:" prefix for display.
std::string b2fDisplayAddress(const std::string& addr);

struct B2FHeader {
    std::string message_id;
    B2FMessageType type = B2FMessageType::Private;
    std::string type_name;               // decoded "Type:" word (verbatim)
    std::string date_time;               // "YYYY/MM/DD HH:MM" UTC

    // Wire addresses ("K1AAA", "SMTP:user@example.com"). When `from` /
    // `to` are empty, encode() builds them from the legacy fields below.
    std::string from;
    std::vector<std::string> to;
    std::vector<std::string> cc;
    std::string mbo;                     // defaults to the From callsign

    // Legacy convenience fields. On decode, *_address holds the full
    // internet address (without "SMTP:") when the address is one, and
    // *_callsign the callsign or the local part.
    std::string from_callsign;
    std::string from_address;
    std::string to_callsign;
    std::string to_address;

    std::string subject;
    uint32_t body_size = 0;
    std::vector<std::pair<std::string, uint32_t>> attachments;  // filename, size

    // Other header fields (e.g. Content-Type), in order, written before Body.
    std::vector<std::pair<std::string, std::string>> extra;

    // Value of an extra header (case-insensitive name), or "".
    std::string extraValue(const std::string& name) const;
};

using B2FAttachments = std::vector<std::pair<std::string, std::vector<uint8_t>>>;

class B2FCodec {
public:
    // Encode a message. Fails (returns false, `out` empty) without a
    // message id or recipient. CR/LF inside header values are replaced by
    // spaces.
    bool encode(const B2FHeader& header, const std::string& body,
                const B2FAttachments& attachments,
                std::vector<uint8_t>& out, std::string* error = nullptr);

    // Same, returning an empty vector on failure.
    std::vector<uint8_t> encode(const B2FHeader& header, const std::string& body,
                                const B2FAttachments& attachments);

    // Decode a complete message. Returns false on malformed or truncated
    // input; `header`, `body` and `attachments` are only meaningful on
    // success.
    bool decode(const std::vector<uint8_t>& data, B2FHeader& header, std::string& body,
                B2FAttachments& attachments, std::string* error = nullptr);
};

} // namespace skylink
