// Winlink CMS client (B2F over TCP/TLS) for the Skywave Link email gateway.
//
// Session (Winlink CMS "telnet" service, as used by Winlink Express and
// la5nta/wl2k-go):
//   - TCP or TLS connect (CMS: port 8773 is TLS, 8772 plain TCP).
//   - "Callsign :" prompt -> callsign; "Password :" prompt -> the fixed
//     token "CMSTelnet". The account password is never sent: it is only
//     used for the B2F secure login (;PQ challenge -> ;PR response).
//   - B2F handshake: server SID [WL2K-...-B2FWIHJM$], ;PQ, prompt ">";
//     client ;FW: <call>, its SID and ;PR: <response>.
//   - FBB B2 exchange: FC EM proposals + F> checksum, FS answers, LZHUF
//     compressed message blocks (SOH/STX/EOT with checksum), FF / FQ.
//
// Inbound messages are acknowledged (and so removed from the CMS) only by
// the session turnover after they are received; the client performs that
// turnover only after the commit gate has accepted them.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace skylink {

struct WinlinkTelnetRequest {
    std::string host = "cms.winlink.org";
    uint16_t port = 8773;
    bool use_tls = true;
    int timeout_ms = 15000;

    std::string callsign;
    std::string password;
    std::string to;          // one or more addresses, separated by ',' or ';'
    std::string subject;
    std::string body;

    // Optional message id (max 12 characters, [A-Z0-9]). When empty a
    // random one is generated. A stable id makes a retry after an
    // ambiguous failure safe: the CMS answers "already have it" and the
    // delivery counts as done.
    std::string message_id;
};

struct WinlinkTelnetAttachment {
    std::string name;
    std::vector<uint8_t> data;
};

struct WinlinkTelnetInboundMessage {
    std::string message_id;
    std::string from;
    std::string to;          // recipients, comma separated
    std::string subject;
    std::string body;        // UTF-8, LF line endings
    std::string raw;         // the complete B2F message as received

    std::string cc;
    std::string date;        // "YYYY/MM/DD HH:MM" UTC as sent
    std::vector<WinlinkTelnetAttachment> attachments;
};

struct WinlinkTelnetFetchRequest {
    std::string host = "cms.winlink.org";
    uint16_t port = 8773;
    bool use_tls = true;
    int timeout_ms = 15000;

    std::string callsign;
    std::string password;
    // Upper bound per session. The CMS offers at most five messages per
    // proposal block and a session receives one block; the rest stay on
    // the CMS for the next poll.
    int max_messages = 20;
    // False: messages are received but never acknowledged (the session is
    // dropped after the commit gate), so the CMS keeps them.
    bool delete_after_fetch = true;

    // When set and true, no further message is accepted.
    const std::atomic<bool>* stop = nullptr;
};

struct WinlinkTelnetFetchResult {
    bool success = false;
    std::string error;
    std::string login_mode_used;
    int login_attempts = 0;
    std::vector<WinlinkTelnetInboundMessage> messages;
    // True when the fetched messages were acknowledged to the CMS (which
    // then removes them).
    bool committed = false;
    // Proposals left on the CMS (deferred) in this session.
    int deferred = 0;
};

// Called once the messages are received, before any is acknowledged.
// Return true once they are stored durably: the session then hands the
// turn back to the CMS, which marks them delivered. Return false and the
// connection is dropped without acknowledgement: the CMS keeps them all.
using WinlinkCommitGate = std::function<bool(const WinlinkTelnetFetchResult&)>;

// Deliver a single message to the Winlink CMS. True once the CMS has
// accepted the message (it took the turn after the transfer).
bool sendWinlinkTelnetDelivery(const WinlinkTelnetRequest& req,
                               std::string* error = nullptr,
                               std::string* login_mode_used = nullptr,
                               int* login_attempts = nullptr);

// Fetch inbound messages from the Winlink CMS.
WinlinkTelnetFetchResult fetchWinlinkTelnetMessages(const WinlinkTelnetFetchRequest& req);
WinlinkTelnetFetchResult fetchWinlinkTelnetMessages(const WinlinkTelnetFetchRequest& req,
                                                    const WinlinkCommitGate& commit);

// Exposed helpers for tests and diagnostics.

// The challenge of a ";PQ: <challenge>" line (or of any text containing
// one), or "".
std::string extractWinlinkChallengeToken(const std::string& prompt);
// Winlink secure login response (8 digits) for `challenge_token` and the
// account password.
std::string buildWinlinkChallengeResponse(const std::string& password,
                                          const std::string& challenge_token);
// Parses a B2F message (falls back to simple "Key: value" headers + body
// for non-B2F text).
WinlinkTelnetInboundMessage parseWinlinkInboundMessage(const std::string& raw);

// Checksum of an FBB proposal block: the two's complement of the sum of
// all proposal line bytes, each line counted with its CR.
uint8_t winlinkProposalChecksum(const std::vector<std::string>& proposal_lines);

}  // namespace skylink
