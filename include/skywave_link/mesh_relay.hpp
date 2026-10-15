// MeshRelay - Multi-hop message relay / digipeater
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace skylink {

// Relay packet marker (#10: namespace-scope inline constexpr)
inline constexpr uint8_t RELAY_PACKET_MARKER = 0xE5;

// Relay packet header
struct RelayHeader {
    uint8_t ttl = 3;
    uint8_t hop_count = 0;
    std::string source;
    std::string destination;
    std::string message_id;
    std::vector<std::string> path;  // Callsigns traversed
};

// Route table entry
struct RouteEntry {
    std::string destination;
    std::string next_hop;
    uint8_t hop_count;
    float snr_db;
    int64_t last_seen;
};

// Relay queue entry
struct RelayQueueEntry {
    RelayHeader header;
    std::vector<uint8_t> packet;  // #9: renamed from payload to packet (full serialized packet)
    int64_t queued_at;
};

// #12: Tri-state return for processRelayPacket
enum class RelayResult { ForUs, Relayed, Dropped };

// Relay statistics
struct RelayStats {
    uint64_t packets_relayed = 0;
    uint64_t packets_received = 0;
    uint64_t packets_dropped_ttl = 0;
    uint64_t packets_dropped_hop_limit = 0;
    uint64_t packets_dropped_loop = 0;
    uint64_t packets_dropped_dedup = 0;
    uint64_t packets_dropped_queue_full = 0;  // #8: new counter
};

class MeshRelay {
public:
    MeshRelay();
    ~MeshRelay() = default;

    // Non-copyable
    MeshRelay(const MeshRelay&) = delete;
    MeshRelay& operator=(const MeshRelay&) = delete;

    // Configuration
    void setMyCallsign(const std::string& callsign);
    void setEnabled(bool enabled);
    bool isEnabled() const;
    void setDefaultTTL(uint8_t ttl);
    uint8_t getDefaultTTL() const;
    void setMaxHopCount(uint8_t max_hops);
    uint8_t getMaxHopCount() const;
    void setDedupWindowSec(int64_t dedup_window_sec);
    int64_t getDedupWindowSec() const;

    // Packet processing
    // #12: Returns RelayResult enum instead of bool
    // #11: Optional received_snr_db parameter for route learning
    RelayResult processRelayPacket(const std::vector<uint8_t>& data,
                                   std::vector<uint8_t>& payload_out,
                                   float received_snr_db = 0.0f);

    // Create a relay packet for sending via mesh
    std::vector<uint8_t> createRelayPacket(const std::string& destination,
                                           const std::vector<uint8_t>& payload,
                                           const std::string& relay_message_id = "");

    // Serialize/deserialize relay headers
    static std::vector<uint8_t> serializeHeader(const RelayHeader& header);
    static bool deserializeHeader(const uint8_t* data, size_t size,
                                   RelayHeader& header, size_t& header_size);

    // Route management
    void addRoute(const RouteEntry& route);
    void processBeaconRoute(const std::string& callsign, float snr_db);
    std::vector<RouteEntry> getRouteTable() const;
    void pruneStaleRoutes(int64_t max_age_sec);

    // Relay queue management
    bool hasRelayQueue() const;
    bool getNextRelay(RelayQueueEntry& entry);
    size_t relayQueueSize() const;

    // Statistics
    RelayStats getStats() const;

    // Build beacon payload with relay capability flag
    std::vector<uint8_t> buildRelayBeaconPayload() const;

    static constexpr int MAX_RELAY_QUEUE = 50;
    static constexpr int64_t DEDUP_WINDOW_SEC = 300;  // 5 minutes

private:
    mutable std::mutex mutex_;
    std::string my_callsign_;
    bool enabled_ = false;
    uint8_t default_ttl_ = 3;
    uint8_t max_hop_count_ = 6;
    int64_t dedup_window_sec_ = DEDUP_WINDOW_SEC;

    // #1: Member RNG instead of static locals (mutable for use in const method)
    mutable std::mt19937 gen_;
    mutable std::uniform_int_distribution<uint32_t> dist_;

    // Route table
    std::map<std::string, RouteEntry> routes_;

    // Relay queue
    std::deque<RelayQueueEntry> relay_queue_;

    // Deduplication cache: message_id -> timestamp
    std::map<std::string, int64_t> dedup_cache_;

    // #5: Cleanup counter for periodic dedup cleanup
    uint64_t process_call_count_ = 0;

    // Statistics
    RelayStats stats_;

    // Helpers
    bool isDuplicate(const std::string& message_id);
    bool isInPath(const RelayHeader& header, const std::string& callsign) const;
    std::string generateMessageId() const;
    void cleanupDedup();
    bool isBetterRoute(const RouteEntry& existing, const RouteEntry& candidate) const;
};

} // namespace skylink
