// Email gateway network job: the POP3 / Winlink Telnet fetches and the
// Winlink Telnet / SMTP deliveries of one gateway poll, run off the GUI
// thread.
//
// The caller snapshots everything the job needs into EmailGatewayJobInput,
// the worker performs only network calls, and the caller applies the
// EmailGatewayJobOutput (store writes, diagnostics, chat notices) on its own
// thread. The job never sees application state, so it may outlive its owner.
#pragma once

#include "skywave_link/pop3_client.hpp"
#include "skywave_link/smtp_client.hpp"
#include "skywave_link/winlink_telnet_client.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace skylink {

struct EmailGatewayOutgoing {
    int64_t store_id = 0;
    std::string to;
    std::string subject;
    std::string body;
};

struct EmailGatewayJobInput {
    bool poll_pop3 = false;
    POP3FetchRequest pop3;

    bool poll_winlink = false;
    WinlinkTelnetFetchRequest winlink_fetch;

    // Delivery: Winlink Telnet first when ready, SMTP as fallback / only
    // route. `winlink_send` and `smtp` carry the server settings; to,
    // subject and body come from each outgoing message.
    bool winlink_send_ready = false;
    WinlinkTelnetRequest winlink_send;
    bool smtp_ready = false;
    SMTPDeliveryRequest smtp;
    std::vector<EmailGatewayOutgoing> outgoing;
};

struct EmailGatewaySendResult {
    bool attempted = false;          // false: skipped because the job was stopped
    bool sent = false;
    bool winlink_tried = false;
    bool smtp_tried = false;
    std::string relay_mode;          // "Winlink Telnet" or "SMTP" when sent
    std::string error;
    std::string winlink_login_mode;
    int winlink_login_attempts = 0;
};

struct EmailGatewayJobOutput {
    bool pop3_ran = false;
    POP3FetchResult pop3;
    std::string pop3_target;     // "host:port"
    bool winlink_ran = false;
    WinlinkTelnetFetchResult winlink;
    std::string winlink_target;  // "host:port"
    std::vector<EmailGatewaySendResult> sends;  // parallel to input.outgoing
};

// Receives fetched inbound mail before the server deletes it: `partial`
// holds one fetch (pop3_ran or winlink_ran set). Return true once the mail
// is stored durably; only then are the server copies deleted (POP3 QUIT /
// Winlink kill). Fetch failures are delivered too (return value ignored).
using EmailInboundSink = std::function<bool(const EmailGatewayJobOutput& partial)>;

// Runs the network part of one gateway poll synchronously. Without a sink
// the fetch results are part of the returned output and deletions are
// committed at once. With a sink they are delivered through it instead
// (and not repeated in the output). `stop` (optional) ends the job early:
// no new message is fetched and no new delivery is started.
EmailGatewayJobOutput runEmailGatewayJob(const EmailGatewayJobInput& input,
                                         const EmailInboundSink& sink = {},
                                         const std::atomic<bool>* stop = nullptr);

// Runs one job at a time on a detached helper thread. The state shared with
// the thread lives in a reference-counted slot, so abandoning a job (or
// destroying the worker) never waits for the network, and an abandoned job
// never commits a server-side deletion.
class EmailGatewayWorker {
public:
    using Runner = std::function<EmailGatewayJobOutput(const EmailGatewayJobInput&,
                                                       const EmailInboundSink&,
                                                       const std::atomic<bool>*)>;

    // How long the job waits for ackInbound() before treating the mail as
    // not stored (the server then keeps it).
    static constexpr int kInboundAckTimeoutMs = 30000;

    explicit EmailGatewayWorker(Runner runner = runEmailGatewayJob);
    ~EmailGatewayWorker();

    EmailGatewayWorker(const EmailGatewayWorker&) = delete;
    EmailGatewayWorker& operator=(const EmailGatewayWorker&) = delete;

    // Starts a job; false while one is still running (or the thread could
    // not be started).
    bool start(EmailGatewayJobInput input);
    bool busy() const { return slot_ != nullptr; }

    // Fetched inbound mail waiting for the owner, once. The owner stores it
    // and must answer with ackInbound(stored) (the job is blocked until then).
    std::optional<EmailGatewayJobOutput> takeInbound();
    void ackInbound(bool stored);

    // The finished job's output, once; nullopt while running or idle.
    std::optional<EmailGatewayJobOutput> poll();

    // Waits up to `timeout_ms` until the job finishes or has inbound mail to
    // hand over; true when either is ready.
    bool waitFor(int timeout_ms);
    // Asks the running job to stop starting new operations.
    void requestStop();
    // Forgets the running job; its thread finishes on its own, deleting
    // nothing on the server and reporting nothing.
    void abandon();

private:
    struct Slot;
    Runner runner_;
    std::shared_ptr<Slot> slot_;
};

} // namespace skylink
