// Email gateway job: network I/O runs on a helper thread, one job at a
// time, and never blocks the caller even against an unreachable server.

#include "test_macros.hpp"
#include "skywave_link/email_gateway_job.hpp"

#include <atomic>
#include <memory>
#include <chrono>
#include <thread>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace skylink;

namespace {

using Clock = std::chrono::steady_clock;

bool within(Clock::time_point start, int ms) {
    return Clock::now() - start < std::chrono::milliseconds(ms);
}

#ifndef _WIN32
// A loopback port with nothing listening (connects are refused at once).
uint16_t closedPort() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    close(fd);
    return ntohs(addr.sin_port);
}
#endif

} // namespace

// The real runner against TEST-NET-1 (RFC 5737, never routed): starting the
// job returns at once and the caller can abandon it without waiting.
TEST(unreachable_server_does_not_block_caller) {
    EmailGatewayJobInput input;
    input.poll_pop3 = true;
    input.pop3.host = "192.0.2.1";
    input.pop3.port = 995;
    input.pop3.username = "user";
    input.smtp_ready = true;
    input.smtp.host = "192.0.2.1";
    input.smtp.port = 587;
    input.outgoing.push_back({1, "test@example.com", "Subject", "Body"});

    EmailGatewayWorker worker;
    const auto start = Clock::now();
    ASSERT_TRUE(worker.start(input));
    ASSERT_TRUE(within(start, 50));
    ASSERT_TRUE(worker.busy());
    ASSERT_FALSE(worker.start(input));        // one job at a time
    ASSERT_FALSE(worker.poll().has_value());  // still running
    const auto wait_start = Clock::now();
    (void)worker.waitFor(100);                // bounded wait
    ASSERT_TRUE(within(wait_start, 1000));
    worker.abandon();                         // shutdown path: no join
    ASSERT_FALSE(worker.busy());
    PASS();
}

// The result is delivered once, through poll(), on the caller's thread.
TEST(result_delivered_once) {
    std::atomic<int> runs{0};
    EmailGatewayWorker worker([&runs](const EmailGatewayJobInput& in, const EmailInboundSink&,
                                      const std::atomic<bool>*) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        runs++;
        EmailGatewayJobOutput out;
        out.sends.resize(in.outgoing.size());
        for (auto& s : out.sends) {
            s.sent = true;
            s.relay_mode = "SMTP";
        }
        return out;
    });
    EmailGatewayJobInput input;
    input.outgoing.push_back({7, "a@example.com", "s", "b"});
    input.outgoing.push_back({8, "b@example.com", "s", "b"});
    ASSERT_TRUE(worker.start(input));
    ASSERT_TRUE(worker.waitFor(2000));
    auto out = worker.poll();
    ASSERT_TRUE(out.has_value());
    ASSERT_EQ(out->sends.size(), size_t(2));
    ASSERT_TRUE(out->sends[1].sent);
    ASSERT_FALSE(worker.poll().has_value());
    ASSERT_FALSE(worker.busy());
    ASSERT_TRUE(worker.start(input));  // idle again: next job may start
    ASSERT_TRUE(worker.waitFor(2000));
    ASSERT_TRUE(worker.poll().has_value());
    ASSERT_EQ(runs.load(), 2);
    PASS();
}

#ifndef _WIN32
// Refused servers: fetch and delivery fail with an error, and delivery
// falls back from Winlink Telnet to SMTP.
TEST(refused_servers_report_errors_and_fallback) {
    const uint16_t port = closedPort();
    EmailGatewayJobInput input;
    input.poll_pop3 = true;
    input.pop3.host = "127.0.0.1";
    input.pop3.port = port;
    input.pop3.use_tls = false;
    input.winlink_send_ready = true;
    input.winlink_send.host = "127.0.0.1";
    input.winlink_send.port = port;
    input.winlink_send.callsign = "N0CALL";
    input.winlink_send.password = "secret";
    input.smtp_ready = true;
    input.smtp.host = "127.0.0.1";
    input.smtp.port = port;
    input.outgoing.push_back({1, "test@example.com", "Subject", "Body"});

    const auto start = Clock::now();
    EmailGatewayJobOutput out = runEmailGatewayJob(input);
    ASSERT_TRUE(within(start, 5000));
    ASSERT_TRUE(out.pop3_ran);
    ASSERT_FALSE(out.pop3.success);
    ASSERT_FALSE(out.pop3.error.empty());
    ASSERT_EQ(out.pop3_target, "127.0.0.1:" + std::to_string(port));
    ASSERT_EQ(out.sends.size(), size_t(1));
    ASSERT_FALSE(out.sends[0].sent);
    ASSERT_TRUE(out.sends[0].winlink_tried);
    ASSERT_TRUE(out.sends[0].smtp_tried);
    ASSERT_FALSE(out.sends[0].error.empty());
    PASS();
}
#endif

// Fetched mail reaches the owner before the job may delete server copies:
// the sink blocks until ackInbound(), and its answer is what the fetch
// client uses to decide on POP3 QUIT / Winlink kill.
TEST(inbound_handoff_waits_for_owner_ack) {
    std::atomic<int> sink_answer{-1};
    EmailGatewayWorker worker([&sink_answer](const EmailGatewayJobInput&, const EmailInboundSink& sink,
                                             const std::atomic<bool>*) {
        EmailGatewayJobOutput partial;
        partial.pop3_ran = true;
        partial.pop3.success = true;
        partial.pop3.messages.resize(2);
        sink_answer = sink(partial) ? 1 : 0;
        return EmailGatewayJobOutput{};
    });
    ASSERT_TRUE(worker.start(EmailGatewayJobInput{}));
    ASSERT_TRUE(worker.waitFor(2000));        // inbound ready
    auto inbound = worker.takeInbound();
    ASSERT_TRUE(inbound.has_value());
    ASSERT_EQ(inbound->pop3.messages.size(), size_t(2));
    ASSERT_FALSE(worker.poll().has_value());  // job still waiting for the ack
    ASSERT_EQ(sink_answer.load(), -1);
    worker.ackInbound(true);
    ASSERT_TRUE(worker.waitFor(2000));
    ASSERT_TRUE(worker.poll().has_value());
    ASSERT_EQ(sink_answer.load(), 1);
    PASS();
}

// Abandoning (shutdown past its bound) while mail is waiting answers "not
// stored": the job then never commits a deletion.
TEST(abandon_never_commits_deletion) {
    auto answer = std::make_shared<std::atomic<int>>(-1);
    {
        EmailGatewayWorker worker([answer](const EmailGatewayJobInput&, const EmailInboundSink& sink,
                                           const std::atomic<bool>*) {
            EmailGatewayJobOutput partial;
            partial.pop3_ran = true;
            *answer = sink(partial) ? 1 : 0;
            return EmailGatewayJobOutput{};
        });
        ASSERT_TRUE(worker.start(EmailGatewayJobInput{}));
        ASSERT_TRUE(worker.waitFor(2000));
        ASSERT_TRUE(worker.takeInbound().has_value());
        worker.abandon();  // owner gives up before storing
    }
    for (int i = 0; i < 200 && answer->load() == -1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(answer->load(), 0);
    PASS();
}

// A stop request (shutdown) skips deliveries not yet started; they are
// reported as not attempted so the owner keeps them queued.
TEST(stop_skips_unstarted_deliveries) {
    EmailGatewayJobInput input;
    input.smtp_ready = true;
    input.smtp.host = "127.0.0.1";
    input.outgoing.push_back({1, "a@example.com", "s", "b"});
    input.outgoing.push_back({2, "b@example.com", "s", "b"});
    std::atomic<bool> stop{true};
    EmailGatewayJobOutput out = runEmailGatewayJob(input, {}, &stop);
    ASSERT_EQ(out.sends.size(), size_t(2));
    ASSERT_FALSE(out.sends[0].attempted);
    ASSERT_FALSE(out.sends[1].attempted);
    ASSERT_FALSE(out.sends[0].sent);
    PASS();
}

// TEST-NET-1 is never routed: the 5 s connect bound ends the job.
TEST(connect_timeout_bounds_the_job) {
    EmailGatewayJobInput input;
    input.smtp_ready = true;
    input.smtp.host = "192.0.2.1";
    input.smtp.port = 587;
    input.outgoing.push_back({1, "a@example.com", "s", "b"});
    const auto start = Clock::now();
    EmailGatewayJobOutput out = runEmailGatewayJob(input);
    ASSERT_TRUE(within(start, 8000));
    ASSERT_EQ(out.sends.size(), size_t(1));
    ASSERT_TRUE(out.sends[0].attempted);
    ASSERT_FALSE(out.sends[0].sent);
    PASS();
}

TEST_MAIN()
