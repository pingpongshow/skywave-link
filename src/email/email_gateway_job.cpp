// Email gateway network job (see email_gateway_job.hpp).

#include "skywave_link/email_gateway_job.hpp"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace skylink {

namespace {

bool stopRequested(const std::atomic<bool>* stop) {
    return stop && stop->load();
}

} // namespace

EmailGatewayJobOutput runEmailGatewayJob(const EmailGatewayJobInput& input,
                                         const EmailInboundSink& sink,
                                         const std::atomic<bool>* stop) {
    EmailGatewayJobOutput out;

    if (input.poll_pop3 && !stopRequested(stop)) {
        POP3FetchRequest req = input.pop3;
        req.stop = stop;
        const std::string target = req.host + ":" + std::to_string(req.port);
        if (sink) {
            bool delivered = false;
            POP3FetchResult result = fetchPOP3Messages(req, [&](const POP3FetchResult& r) {
                delivered = true;
                EmailGatewayJobOutput partial;
                partial.pop3_ran = true;
                partial.pop3 = r;
                partial.pop3_target = target;
                return sink(partial);
            });
            if (!delivered) {  // failed before the commit point: report it
                EmailGatewayJobOutput partial;
                partial.pop3_ran = true;
                partial.pop3 = std::move(result);
                partial.pop3_target = target;
                (void)sink(partial);
            }
        } else {
            out.pop3_ran = true;
            out.pop3_target = target;
            out.pop3 = fetchPOP3Messages(req);
        }
    }

    if (input.poll_winlink && !stopRequested(stop)) {
        WinlinkTelnetFetchRequest req = input.winlink_fetch;
        req.stop = stop;
        const std::string target = req.host + ":" + std::to_string(req.port);
        if (sink) {
            bool delivered = false;
            WinlinkTelnetFetchResult result =
                fetchWinlinkTelnetMessages(req, [&](const WinlinkTelnetFetchResult& r) {
                    delivered = true;
                    EmailGatewayJobOutput partial;
                    partial.winlink_ran = true;
                    partial.winlink = r;
                    partial.winlink_target = target;
                    return sink(partial);
                });
            if (!delivered) {
                EmailGatewayJobOutput partial;
                partial.winlink_ran = true;
                partial.winlink = std::move(result);
                partial.winlink_target = target;
                (void)sink(partial);
            }
        } else {
            out.winlink_ran = true;
            out.winlink_target = target;
            out.winlink = fetchWinlinkTelnetMessages(req);
        }
    }

    out.sends.reserve(input.outgoing.size());
    for (const auto& msg : input.outgoing) {
        EmailGatewaySendResult result;
        if (stopRequested(stop)) {
            result.error = "not attempted (shutting down)";
            out.sends.push_back(std::move(result));
            continue;
        }
        result.attempted = true;
        if (input.winlink_send_ready) {
            WinlinkTelnetRequest req = input.winlink_send;
            req.to = msg.to;
            req.subject = msg.subject;
            req.body = msg.body;
            result.winlink_tried = true;
            result.sent = sendWinlinkTelnetDelivery(req,
                                                    &result.error,
                                                    &result.winlink_login_mode,
                                                    &result.winlink_login_attempts);
            if (result.sent) {
                result.relay_mode = "Winlink Telnet";
            }
        }
        if (!result.sent && input.smtp_ready) {
            SMTPDeliveryRequest req = input.smtp;
            req.to = msg.to;
            req.subject = msg.subject;
            req.body = msg.body;
            result.smtp_tried = true;
            result.error.clear();
            result.sent = sendSMTPDelivery(req, &result.error);
            if (result.sent) {
                result.relay_mode = "SMTP";
            }
        }
        out.sends.push_back(std::move(result));
    }
    return out;
}

struct EmailGatewayWorker::Slot {
    enum class InboundState { None, Waiting, Taken, Acked };

    std::mutex mu;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    bool abandoned = false;
    bool done = false;
    EmailGatewayJobOutput output;

    InboundState inbound_state = InboundState::None;
    EmailGatewayJobOutput inbound;
    bool inbound_stored = false;
};

EmailGatewayWorker::EmailGatewayWorker(Runner runner) : runner_(std::move(runner)) {}

EmailGatewayWorker::~EmailGatewayWorker() {
    abandon();
}

bool EmailGatewayWorker::start(EmailGatewayJobInput input) {
    if (slot_) return false;
    auto slot = std::make_shared<Slot>();

    // Hands inbound mail to the owner and blocks until it is stored (or the
    // owner gives up). Runs on the job thread.
    EmailInboundSink sink = [slot](const EmailGatewayJobOutput& partial) {
        std::unique_lock<std::mutex> lock(slot->mu);
        if (slot->abandoned) return false;
        slot->inbound = partial;
        slot->inbound_stored = false;
        slot->inbound_state = Slot::InboundState::Waiting;
        slot->cv.notify_all();
        const bool answered = slot->cv.wait_for(
            lock, std::chrono::milliseconds(kInboundAckTimeoutMs),
            [&slot]() { return slot->abandoned || slot->inbound_state == Slot::InboundState::Acked; });
        const bool stored = answered && !slot->abandoned &&
                            slot->inbound_state == Slot::InboundState::Acked &&
                            slot->inbound_stored;
        slot->inbound_state = Slot::InboundState::None;
        slot->inbound = EmailGatewayJobOutput{};
        return stored;
    };

    try {
        // The thread owns copies of the runner and the input plus a share of
        // the slot; nothing it touches belongs to the caller.
        std::thread([slot, sink, runner = runner_, input = std::move(input)]() {
            EmailGatewayJobOutput output;
            try {
                output = runner(input, sink, &slot->stop);
            } catch (const std::exception& e) {
                output.sends.resize(input.outgoing.size());
                for (auto& s : output.sends) s.error = e.what();
            }
            std::lock_guard<std::mutex> lock(slot->mu);
            slot->output = std::move(output);
            slot->done = true;
            slot->cv.notify_all();
        }).detach();
    } catch (const std::exception&) {
        return false;
    }
    slot_ = std::move(slot);
    return true;
}

std::optional<EmailGatewayJobOutput> EmailGatewayWorker::takeInbound() {
    if (!slot_) return std::nullopt;
    std::lock_guard<std::mutex> lock(slot_->mu);
    if (slot_->inbound_state != Slot::InboundState::Waiting) return std::nullopt;
    slot_->inbound_state = Slot::InboundState::Taken;
    return slot_->inbound;
}

void EmailGatewayWorker::ackInbound(bool stored) {
    if (!slot_) return;
    std::lock_guard<std::mutex> lock(slot_->mu);
    if (slot_->inbound_state != Slot::InboundState::Taken) return;  // job gave up
    slot_->inbound_stored = stored;
    slot_->inbound_state = Slot::InboundState::Acked;
    slot_->cv.notify_all();
}

std::optional<EmailGatewayJobOutput> EmailGatewayWorker::poll() {
    if (!slot_) return std::nullopt;
    std::shared_ptr<Slot> slot = slot_;
    std::lock_guard<std::mutex> lock(slot->mu);
    if (!slot->done) return std::nullopt;
    slot_.reset();
    return std::move(slot->output);
}

bool EmailGatewayWorker::waitFor(int timeout_ms) {
    if (!slot_) return true;
    std::shared_ptr<Slot> slot = slot_;
    std::unique_lock<std::mutex> lock(slot->mu);
    return slot->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&slot]() {
        return slot->done || slot->inbound_state == Slot::InboundState::Waiting;
    });
}

void EmailGatewayWorker::requestStop() {
    if (slot_) slot_->stop = true;
}

void EmailGatewayWorker::abandon() {
    if (!slot_) return;
    {
        std::lock_guard<std::mutex> lock(slot_->mu);
        slot_->abandoned = true;
        slot_->stop = true;
        slot_->cv.notify_all();
    }
    slot_.reset();
}

} // namespace skylink
