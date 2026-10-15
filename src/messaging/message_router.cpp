// MessageRouter - Handler registry implementation

#include "skywave_link/message_router.hpp"
#include <algorithm>
#include <chrono>

namespace skylink {

MessageRouter::MessageRouter() = default;

void MessageRouter::registerHandler(uint8_t prefix, MessageHandler handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    handlers_[prefix] = std::move(handler);
}

void MessageRouter::registerRangeHandler(uint8_t lo, uint8_t hi, MessageHandler handler) {
    if (lo > hi) return;
    std::lock_guard<std::mutex> lock(handler_mutex_);
    range_handlers_.push_back({lo, hi, std::move(handler)});
}

void MessageRouter::setDefaultHandler(MessageHandler handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    default_handler_ = std::move(handler);
}

bool MessageRouter::dispatch(const std::vector<uint8_t>& data, const std::string& from_override) {
    if (data.empty()) return false;

    uint8_t prefix = data[0];

    // Copy the handler under lock, then invoke outside lock to avoid deadlock
    MessageHandler handler_copy;

    {
        std::lock_guard<std::mutex> lock(handler_mutex_);

        // Check exact match first
        auto it = handlers_.find(prefix);
        if (it != handlers_.end()) {
            handler_copy = it->second;
        } else {
            // Check range handlers
            for (const auto& range : range_handlers_) {
                if (prefix >= range.lo && prefix <= range.hi) {
                    handler_copy = range.handler;
                    break;
                }
            }

            // Fall back to default handler
            if (!handler_copy && default_handler_) {
                handler_copy = default_handler_;
            }
        }
    }

    if (handler_copy) {
        handler_copy(data, from_override);
        return true;
    }

    return false;
}

void MessageRouter::enqueue(MessagePriority priority, std::vector<uint8_t> data,
                            const std::string& command) {
    std::lock_guard<std::mutex> lock(outbound_mutex_);

    OutboundMessage msg;
    msg.priority = priority;
    msg.data = std::move(data);
    msg.command = command;
    msg.queued_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // Insert in priority order (lower priority value = higher priority)
    // Use upper_bound so same-priority messages are FIFO (new goes after existing)
    auto pos = std::upper_bound(outbound_queue_.begin(), outbound_queue_.end(), msg,
        [](const OutboundMessage& a, const OutboundMessage& b) {
            return static_cast<uint8_t>(a.priority) < static_cast<uint8_t>(b.priority);
        });
    outbound_queue_.insert(pos, std::move(msg));
}

bool MessageRouter::processOutbound(OutboundMessage& out) {
    std::lock_guard<std::mutex> lock(outbound_mutex_);
    if (outbound_queue_.empty()) return false;

    out = std::move(outbound_queue_.front());
    outbound_queue_.pop_front();
    return true;
}

bool MessageRouter::hasOutbound() const {
    std::lock_guard<std::mutex> lock(outbound_mutex_);
    return !outbound_queue_.empty();
}

size_t MessageRouter::outboundCount() const {
    std::lock_guard<std::mutex> lock(outbound_mutex_);
    return outbound_queue_.size();
}

void MessageRouter::clearOutbound() {
    std::lock_guard<std::mutex> lock(outbound_mutex_);
    outbound_queue_.clear();
}

} // namespace skylink
