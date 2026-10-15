// MessageRouter - Handler registry for incoming/outgoing radio messages
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace skylink {

// Message handler: receives raw data and optional from_override callsign
using MessageHandler = std::function<void(const std::vector<uint8_t>& data,
                                          const std::string& from_override)>;

// Outbound message priority
enum class MessagePriority : uint8_t {
    ACK = 0,      // Highest - acknowledgements
    Control = 1,  // Control messages (SNR, ping)
    Data = 2,     // Normal data
    Beacon = 3    // Lowest - beacons/CQ
};

// Outbound queue entry
struct OutboundMessage {
    MessagePriority priority;
    std::vector<uint8_t> data;
    std::string command;  // Optional modem command before data (e.g. "BEACON")
    int64_t queued_at;
};

class MessageRouter {
public:
    MessageRouter();
    ~MessageRouter() = default;

    // Non-copyable
    MessageRouter(const MessageRouter&) = delete;
    MessageRouter& operator=(const MessageRouter&) = delete;

    // Register a handler for a specific prefix byte
    void registerHandler(uint8_t prefix, MessageHandler handler);

    // Register a handler for a range of prefix bytes [lo, hi] inclusive
    void registerRangeHandler(uint8_t lo, uint8_t hi, MessageHandler handler);

    // Set default handler for unrecognized prefix bytes
    void setDefaultHandler(MessageHandler handler);

    // Dispatch an incoming message to the appropriate handler
    // Returns true if a handler was found and called
    bool dispatch(const std::vector<uint8_t>& data, const std::string& from_override = "");

    // Enqueue an outbound message with priority
    void enqueue(MessagePriority priority, std::vector<uint8_t> data,
                 const std::string& command = "");

    // Process the next outbound message (returns false if queue empty)
    // Caller should send the returned data via modem
    bool processOutbound(OutboundMessage& out);

    // Check if there are outbound messages pending
    bool hasOutbound() const;

    // Get number of pending outbound messages
    size_t outboundCount() const;

    // Clear all outbound messages
    void clearOutbound();

private:
    mutable std::mutex handler_mutex_;
    std::unordered_map<uint8_t, MessageHandler> handlers_;

    struct RangeEntry {
        uint8_t lo;
        uint8_t hi;
        MessageHandler handler;
    };
    std::vector<RangeEntry> range_handlers_;

    MessageHandler default_handler_;

    mutable std::mutex outbound_mutex_;
    std::deque<OutboundMessage> outbound_queue_;
};

} // namespace skylink
