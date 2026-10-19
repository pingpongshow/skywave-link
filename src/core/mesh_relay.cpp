// MeshRelay - Multi-hop relay implementation

#include "skywave_link/mesh_relay.hpp"
#include <algorithm>
#include <chrono>
#include <random>

namespace skylink {

namespace {
int64_t now_sec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
} // anonymous namespace

// #1: Initialize member RNG in constructor
MeshRelay::MeshRelay()
    : gen_(std::random_device{}())
{}

void MeshRelay::setMyCallsign(const std::string& callsign) {
    std::lock_guard<std::mutex> lock(mutex_);
    my_callsign_ = callsign;
}

void MeshRelay::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = enabled;
}

bool MeshRelay::isEnabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

void MeshRelay::setDefaultTTL(uint8_t ttl) {
    std::lock_guard<std::mutex> lock(mutex_);
    default_ttl_ = ttl;
}

uint8_t MeshRelay::getDefaultTTL() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return default_ttl_;
}

void MeshRelay::setMaxHopCount(uint8_t max_hops) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_hop_count_ = std::max<uint8_t>(1, max_hops);
}

void MeshRelay::setRouteMaxAgeSec(int64_t max_age_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    route_max_age_sec_ = std::max<int64_t>(60, max_age_sec);
}

void MeshRelay::setDedupWindowSec(int64_t dedup_window_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    dedup_window_sec_ = std::max<int64_t>(30, dedup_window_sec);
    cleanupDedup();
}

int64_t MeshRelay::getDedupWindowSec() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dedup_window_sec_;
}

// Wire format:
// [0xE5][TTL:1][hop_count:1][path_count:1]
// [src_len:1][src:N][dst_len:1][dst:N][msg_id_len:1][msg_id:N]
// [next_hop_len:1][next_hop:N]          (0 = flood)
// [path entries: for each: len:1, callsign:N]
// [payload_len:2 LE][payload:N]

std::vector<uint8_t> MeshRelay::serializeHeader(const RelayHeader& header) {
    // #3: Validate string fields <= 255 bytes
    if (header.source.size() > 255 ||
        header.destination.size() > 255 ||
        header.message_id.size() > 255 ||
        header.next_hop.size() > 255 ||
        header.path.size() > 255) {
        return {};
    }
    for (const auto& p : header.path) {
        if (p.size() > 255) {
            return {};
        }
    }

    std::vector<uint8_t> out;
    out.push_back(RELAY_PACKET_MARKER);
    out.push_back(header.ttl);
    out.push_back(header.hop_count);
    out.push_back(static_cast<uint8_t>(header.path.size()));

    // Source
    out.push_back(static_cast<uint8_t>(header.source.size()));
    out.insert(out.end(), header.source.begin(), header.source.end());

    // Destination
    out.push_back(static_cast<uint8_t>(header.destination.size()));
    out.insert(out.end(), header.destination.begin(), header.destination.end());

    // Message ID
    out.push_back(static_cast<uint8_t>(header.message_id.size()));
    out.insert(out.end(), header.message_id.begin(), header.message_id.end());

    // Next hop
    out.push_back(static_cast<uint8_t>(header.next_hop.size()));
    out.insert(out.end(), header.next_hop.begin(), header.next_hop.end());

    // Path entries
    for (const auto& p : header.path) {
        out.push_back(static_cast<uint8_t>(p.size()));
        out.insert(out.end(), p.begin(), p.end());
    }

    return out;
}

bool MeshRelay::deserializeHeader(const uint8_t* data, size_t size,
                                   RelayHeader& header, size_t& header_size) {
    if (size < 4) return false;
    if (data[0] != RELAY_PACKET_MARKER) return false;

    size_t pos = 1;
    header.ttl = data[pos++];
    header.hop_count = data[pos++];
    uint8_t path_count = data[pos++];

    // Source
    if (pos >= size) return false;
    uint8_t src_len = data[pos++];
    if (pos + src_len > size) return false;
    header.source.assign(reinterpret_cast<const char*>(data + pos), src_len);
    pos += src_len;

    // Destination
    if (pos >= size) return false;
    uint8_t dst_len = data[pos++];
    if (pos + dst_len > size) return false;
    header.destination.assign(reinterpret_cast<const char*>(data + pos), dst_len);
    pos += dst_len;

    // Message ID
    if (pos >= size) return false;
    uint8_t id_len = data[pos++];
    if (pos + id_len > size) return false;
    header.message_id.assign(reinterpret_cast<const char*>(data + pos), id_len);
    pos += id_len;

    // Next hop
    if (pos >= size) return false;
    uint8_t hop_len = data[pos++];
    if (pos + hop_len > size) return false;
    header.next_hop.assign(reinterpret_cast<const char*>(data + pos), hop_len);
    pos += hop_len;

    // Reject packets with empty source or message_id
    if (header.source.empty() || header.destination.empty() || header.message_id.empty()) return false;

    // Path entries
    if (path_count > 32) return false;  // Reasonable upper bound for path entries
    header.path.clear();
    for (uint8_t i = 0; i < path_count; ++i) {
        if (pos >= size) return false;
        uint8_t p_len = data[pos++];
        if (pos + p_len > size) return false;
        header.path.emplace_back(reinterpret_cast<const char*>(data + pos), p_len);
        pos += p_len;
    }

    header_size = pos;
    return true;
}

// #12: Returns RelayResult enum
// #11: Takes optional received_snr_db
RelayResult MeshRelay::processRelayPacket(const std::vector<uint8_t>& data,
                                           std::vector<uint8_t>& payload_out,
                                           std::optional<float> received_snr_db) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.packets_received++;

    // #7: Empty callsign guard
    if (my_callsign_.empty()) {
        return RelayResult::Dropped;
    }

    // #5: Periodic dedup cleanup (every 100 calls or when cache exceeds 1000)
    process_call_count_++;

    RelayHeader header;
    size_t header_size = 0;
    if (!deserializeHeader(data.data(), data.size(), header, header_size)) {
        return RelayResult::Dropped;
    }

    // Extract payload
    if (header_size + 2 > data.size()) return RelayResult::Dropped;
    uint16_t payload_len = static_cast<uint16_t>(data[header_size]) |
                           (static_cast<uint16_t>(data[header_size + 1]) << 8);
    size_t payload_start = header_size + 2;
    if (payload_start + payload_len > data.size()) return RelayResult::Dropped;

    // #2: Validate payload size for relay re-serialization
    if (payload_len > 65535) {
        return RelayResult::Dropped;
    }

    std::vector<uint8_t> payload(data.begin() + payload_start,
                                  data.begin() + payload_start + payload_len);

    // A relay payload must never itself be a relay packet: nesting would let
    // a sender recurse the receiver unboundedly and spoof the inner source.
    if (!payload.empty() && payload[0] == RELAY_PACKET_MARKER) {
        stats_.packets_dropped_nested++;
        return RelayResult::Dropped;
    }

    // Check dedup
    if (isDuplicate(header.message_id)) {
        stats_.packets_dropped_dedup++;
        return RelayResult::Dropped;
    }

    // Record in dedup cache
    dedup_cache_[header.message_id] = now_sec();

    // #5: Only cleanup periodically instead of every call
    if (process_call_count_ % 100 == 0 || dedup_cache_.size() > 1000) {
        cleanupDedup();
        pruneLocked(route_max_age_sec_);
    }

    // Every packet heard teaches the way back to its source: through the
    // station that transmitted it (the last relay, or the source itself).
    {
        const std::string heard_from = header.path.empty() ? header.source : header.path.back();
        const int64_t now = now_sec();
        if (header.source != my_callsign_) {
            RouteEntry back;
            back.destination = header.source;
            back.next_hop = heard_from;
            back.hop_count = static_cast<uint8_t>(std::min<int>(255, header.hop_count + 1));
            back.snr_known = received_snr_db.has_value();
            back.snr_db = received_snr_db.value_or(0.0f);
            back.last_seen = now;
            learnRouteLocked(back);
        }
        if (heard_from != my_callsign_ && heard_from != header.source) {
            RouteEntry neighbour;
            neighbour.destination = heard_from;
            neighbour.next_hop = heard_from;
            neighbour.hop_count = 1;
            neighbour.snr_known = received_snr_db.has_value();
            neighbour.snr_db = received_snr_db.value_or(0.0f);
            neighbour.last_seen = now;
            learnRouteLocked(neighbour);
        }
    }

    // #4: Broadcast relay - for broadcast ("*"), deliver locally AND relay
    bool is_for_us = (header.destination == my_callsign_ || header.destination == "*");
    bool is_broadcast = (header.destination == "*");

    if (is_for_us) {
        payload_out = payload;  // copy (may also relay for broadcast)

        // #4: For unicast, return immediately. For broadcast, fall through to relay.
        if (!is_broadcast) {
            return RelayResult::ForUs;
        }
        // Broadcast: continue to relay logic below (but still return ForUs at the end)
    }

    // Not for us (or broadcast that should also be relayed) - should we relay?
    if (!enabled_) {
        return is_for_us ? RelayResult::ForUs : RelayResult::Dropped;
    }

    // Directed packet: only the designated next hop forwards it.
    if (!is_broadcast && !header.next_hop.empty() && header.next_hop != my_callsign_) {
        stats_.packets_not_next_hop++;
        return is_for_us ? RelayResult::ForUs : RelayResult::Dropped;
    }

    // Check TTL
    if (header.ttl == 0) {
        stats_.packets_dropped_ttl++;
        return is_for_us ? RelayResult::ForUs : RelayResult::Dropped;
    }

    if (header.hop_count >= max_hop_count_) {
        stats_.packets_dropped_hop_limit++;
        return is_for_us ? RelayResult::ForUs : RelayResult::Dropped;
    }

    // Check for loop (our callsign in path)
    if (isInPath(header, my_callsign_)) {
        stats_.packets_dropped_loop++;
        return is_for_us ? RelayResult::ForUs : RelayResult::Dropped;
    }

    // Relay: decrement TTL, add ourselves to path, enqueue
    header.ttl--;
    header.hop_count++;
    if (header.path.size() >= max_hop_count_) {
        return is_for_us ? RelayResult::ForUs : RelayResult::Dropped;
    }
    header.path.push_back(my_callsign_);

    // Onward: along a fresh route when one is known (never back into the
    // path), flooded otherwise.
    header.next_hop.clear();
    if (!is_broadcast) {
        if (auto route = freshRouteLocked(header.destination)) {
            if (!isInPath(header, route->next_hop) || route->next_hop == header.destination) {
                header.next_hop = route->next_hop;
                stats_.packets_directed++;
            }
        }
    }

    // Build relay packet
    auto hdr_bytes = serializeHeader(header);
    if (hdr_bytes.empty()) {
        return is_for_us ? RelayResult::ForUs : RelayResult::Dropped;
    }
    std::vector<uint8_t> relay_packet;
    relay_packet.reserve(hdr_bytes.size() + 2 + payload.size());
    relay_packet.insert(relay_packet.end(), hdr_bytes.begin(), hdr_bytes.end());
    relay_packet.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    relay_packet.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xFF));
    relay_packet.insert(relay_packet.end(), payload.begin(), payload.end());

    if (static_cast<int>(relay_queue_.size()) < MAX_RELAY_QUEUE) {
        RelayQueueEntry entry;
        entry.header = header;
        entry.packet = std::move(relay_packet);  // #9: renamed from payload to packet
        entry.queued_at = now_sec();
        relay_queue_.push_back(std::move(entry));
        stats_.packets_relayed++;
    } else {
        // #8: Increment packets_dropped_queue_full counter
        stats_.packets_dropped_queue_full++;
    }

    return is_for_us ? RelayResult::ForUs : RelayResult::Relayed;
}

std::vector<uint8_t> MeshRelay::createRelayPacket(const std::string& destination,
                                                  const std::vector<uint8_t>& payload,
                                                  const std::string& relay_message_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    // #2: Payload size validation
    if (payload.size() > 65535) {
        return {};
    }
    // Never wrap a relay packet in another relay packet (receivers drop it).
    if (!payload.empty() && payload[0] == RELAY_PACKET_MARKER) {
        return {};
    }

    if (my_callsign_.empty() || destination.empty()) {
        return {};
    }

    RelayHeader header;
    header.ttl = default_ttl_;
    header.hop_count = 0;
    header.source = my_callsign_;
    header.destination = destination;
    header.message_id = relay_message_id.empty() ? generateMessageId() : relay_message_id;
    if (destination != "*") {
        if (auto route = freshRouteLocked(destination)) {
            header.next_hop = route->next_hop;
            stats_.packets_directed++;
        }
    }

    auto hdr_bytes = serializeHeader(header);
    // #3: serializeHeader may return empty on validation failure
    if (hdr_bytes.empty()) {
        return {};
    }

    std::vector<uint8_t> packet;
    packet.reserve(hdr_bytes.size() + 2 + payload.size());
    packet.insert(packet.end(), hdr_bytes.begin(), hdr_bytes.end());
    packet.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    packet.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xFF));
    packet.insert(packet.end(), payload.begin(), payload.end());

    return packet;
}

void MeshRelay::addRoute(const RouteEntry& route) {
    std::lock_guard<std::mutex> lock(mutex_);
    learnRouteLocked(route);
}

void MeshRelay::learnRouteLocked(const RouteEntry& route) {
    if (route.destination.empty() || route.next_hop.empty() ||
        route.destination == my_callsign_) {
        return;
    }
    auto it = routes_.find(route.destination);
    if (it == routes_.end() || isBetterRoute(it->second, route)) {
        routes_[route.destination] = route;
    } else if (it->second.next_hop == route.next_hop &&
               it->second.hop_count == route.hop_count) {
        // Same path heard again: it is still alive.
        it->second.last_seen = std::max(it->second.last_seen, route.last_seen);
        if (route.snr_known) {
            it->second.snr_known = true;
            it->second.snr_db = route.snr_db;
        }
    }
}

void MeshRelay::processBeaconRoute(const std::string& callsign, std::optional<float> snr_db) {
    std::lock_guard<std::mutex> lock(mutex_);
    RouteEntry route;
    route.destination = callsign;
    route.next_hop = callsign;  // Direct
    route.hop_count = 1;
    route.snr_known = snr_db.has_value();
    route.snr_db = snr_db.value_or(0.0f);
    route.last_seen = now_sec();
    learnRouteLocked(route);
}

std::vector<RouteEntry> MeshRelay::getRouteTable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RouteEntry> result;
    for (const auto& [_, route] : routes_) {
        result.push_back(route);
    }
    return result;
}

std::optional<RouteEntry> MeshRelay::freshRouteLocked(const std::string& destination) const {
    auto it = routes_.find(destination);
    if (it == routes_.end()) return std::nullopt;
    if (now_sec() - it->second.last_seen > route_max_age_sec_) return std::nullopt;
    return it->second;
}

std::optional<RouteEntry> MeshRelay::getRoute(const std::string& destination) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return freshRouteLocked(destination);
}

void MeshRelay::pruneLocked(int64_t max_age_sec) {
    int64_t cutoff = now_sec() - max_age_sec;
    for (auto it = routes_.begin(); it != routes_.end(); ) {
        if (it->second.last_seen < cutoff) {
            it = routes_.erase(it);
        } else {
            ++it;
        }
    }
}

void MeshRelay::pruneStaleRoutes(int64_t max_age_sec) {
    std::lock_guard<std::mutex> lock(mutex_);
    pruneLocked(max_age_sec);
}

void MeshRelay::pruneStaleRoutes() {
    std::lock_guard<std::mutex> lock(mutex_);
    pruneLocked(route_max_age_sec_);
}

bool MeshRelay::hasRelayQueue() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !relay_queue_.empty();
}

// #6: Stale relay queue pruning - skip entries older than 60 seconds
bool MeshRelay::getNextRelay(RelayQueueEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!relay_queue_.empty()) {
        auto& front = relay_queue_.front();
        if (now_sec() - front.queued_at > 60) {
            relay_queue_.pop_front();
            continue;
        }
        entry = std::move(relay_queue_.front());
        relay_queue_.pop_front();
        return true;
    }
    return false;
}

size_t MeshRelay::relayQueueSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return relay_queue_.size();
}

RelayStats MeshRelay::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

bool MeshRelay::isDuplicate(const std::string& message_id) {
    auto it = dedup_cache_.find(message_id);
    if (it == dedup_cache_.end()) return false;
    return (now_sec() - it->second) < dedup_window_sec_;
}

bool MeshRelay::isInPath(const RelayHeader& header, const std::string& callsign) const {
    if (header.source == callsign) return true;
    for (const auto& p : header.path) {
        if (p == callsign) return true;
    }
    return false;
}

// #1: Uses member RNG (gen_, dist_) instead of static locals
std::string MeshRelay::generateMessageId() const {
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return my_callsign_ + "_" + std::to_string(us) + "_" + std::to_string(dist_(gen_));
}

bool MeshRelay::isBetterRoute(const RouteEntry& existing, const RouteEntry& candidate) const {
    // Reject routes that exceed max hop count
    if (candidate.hop_count > max_hop_count_) return false;
    // A stale route is always replaced
    if (now_sec() - existing.last_seen > route_max_age_sec_) {
        return true;
    }
    // Fewer hops is better
    if (candidate.hop_count < existing.hop_count) {
        return true;
    }
    if (candidate.hop_count > existing.hop_count) {
        return false;
    }
    // Same hop count: a measured, stronger last hop is better; a measured
    // one beats an unmeasured one.
    if (candidate.snr_known && existing.snr_known) {
        return candidate.snr_db > existing.snr_db;
    }
    return candidate.snr_known && !existing.snr_known;
}

void MeshRelay::cleanupDedup() {
    int64_t cutoff = now_sec() - dedup_window_sec_;
    for (auto it = dedup_cache_.begin(); it != dedup_cache_.end(); ) {
        if (it->second < cutoff) {
            it = dedup_cache_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace skylink
