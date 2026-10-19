// MeshRelay - Multi-hop message relay / digipeater
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
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
    // Directed relay: only this station may forward the packet (every relay
    // is a broadcast on air). Empty: flood - any relay station forwards.
    std::string next_hop;
    std::vector<std::string> path;  // Callsigns traversed
};

// Route table entry. hop_count counts transmissions: 1 = heard directly.
struct RouteEntry {
    std::string destination;
    std::string next_hop;
    uint8_t hop_count = 1;
    bool snr_known = false;      // snr_db is a measurement of the last hop
    float snr_db = 0.0f;
    int64_t last_seen = 0;
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
    uint64_t packets_dropped_nested = 0;      // payload was itself a relay packet
    uint64_t packets_not_next_hop = 0;        // directed to another relay station
    uint64_t packets_directed = 0;            // sent/relayed along a known route
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
    void setDedupWindowSec(int64_t dedup_window_sec);
    int64_t getDedupWindowSec() const;
    // Routes older than this are neither used nor kept (pruneStaleRoutes()).
    void setRouteMaxAgeSec(int64_t max_age_sec);
    static constexpr int64_t DEFAULT_ROUTE_MAX_AGE_SEC = 1800;

    // Packet processing. Every packet heard teaches a route back to its
    // source through the station it was heard from. `received_snr_db` is
    // the measured SNR of the frame when the modem reported one.
    RelayResult processRelayPacket(const std::vector<uint8_t>& data,
                                   std::vector<uint8_t>& payload_out,
                                   std::optional<float> received_snr_db = std::nullopt);

    // Create a relay packet for sending via mesh: directed through the next
    // hop of a fresh route to `destination` when one is known, flooded
    // otherwise.
    std::vector<uint8_t> createRelayPacket(const std::string& destination,
                                           const std::vector<uint8_t>& payload,
                                           const std::string& relay_message_id = "");

    // Serialize/deserialize relay headers
    static std::vector<uint8_t> serializeHeader(const RelayHeader& header);
    static bool deserializeHeader(const uint8_t* data, size_t size,
                                   RelayHeader& header, size_t& header_size);

    // Route management
    void addRoute(const RouteEntry& route);
    // A station heard directly (beacon, CQ, ...): a one-hop route.
    void processBeaconRoute(const std::string& callsign,
                            std::optional<float> snr_db = std::nullopt);
    std::vector<RouteEntry> getRouteTable() const;
    // A fresh route to `destination`, if any.
    std::optional<RouteEntry> getRoute(const std::string& destination) const;
    void pruneStaleRoutes(int64_t max_age_sec);
    void pruneStaleRoutes();   // with the configured route max age; call periodically

    // Relay queue management
    bool hasRelayQueue() const;
    bool getNextRelay(RelayQueueEntry& entry);
    size_t relayQueueSize() const;

    // Statistics
    RelayStats getStats() const;

    static constexpr int MAX_RELAY_QUEUE = 50;
    static constexpr int64_t DEDUP_WINDOW_SEC = 300;  // 5 minutes

private:
    mutable std::mutex mutex_;
    std::string my_callsign_;
    bool enabled_ = false;
    uint8_t default_ttl_ = 3;
    uint8_t max_hop_count_ = 6;
    int64_t dedup_window_sec_ = DEDUP_WINDOW_SEC;
    int64_t route_max_age_sec_ = DEFAULT_ROUTE_MAX_AGE_SEC;

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
    void learnRouteLocked(const RouteEntry& route);
    std::optional<RouteEntry> freshRouteLocked(const std::string& destination) const;
    void pruneLocked(int64_t max_age_sec);
};

} // namespace skylink
