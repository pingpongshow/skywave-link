// MessageStore - SQLite-based persistent message storage
// Handles store-and-forward messaging, beacon/ping history

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>

namespace skylink {

// Beacon history entry (persisted)
struct BeaconRecord {
    int64_t id = 0;
    std::string callsign;
    std::string grid_square;
    float snr_db = 0.0f;
    std::string message;
    bool has_pending = false;
    bool email_gateway = false;
    std::string band;
    int64_t timestamp = 0;
};

// Ping history entry (persisted)
struct PingRecord {
    int64_t id = 0;
    std::string callsign;
    float snr_db = 0.0f;
    std::string grid_square;
    std::string band;
    bool online = true;
    int64_t timestamp = 0;
};

// Message types
enum class MessageType {
    Private,      // Direct station-to-station
    Bulletin,     // Broadcast to multiple
    Position,     // Position report
    File,         // File attachment
    Email         // Email gateway message
};

// Message status
enum class MessageStatus {
    Draft,        // Not yet queued
    Pending,      // Queued for delivery
    Sent,         // Transmitted, awaiting ACK
    Delivered,    // ACK received
    Failed,       // Delivery failed
    Expired       // Message expired
};

// Attachment structure
struct Attachment {
    std::string filename;
    std::string mime_type;
    std::vector<uint8_t> data;
};

// Message structure
struct Message {
    int64_t id = 0;
    MessageType type = MessageType::Private;
    MessageStatus status = MessageStatus::Draft;
    std::string from_callsign;
    std::string to_callsign;
    std::string subject;
    std::string body;
    std::vector<Attachment> attachments;
    int64_t created_at = 0;      // Unix timestamp
    int64_t sent_at = 0;
    int64_t delivered_at = 0;
    int64_t expires_at = 0;
    int priority = 0;            // 0=routine, 1=priority, 2=emergency
    int retry_count = 0;
    std::string message_id;      // Unique ID for deduplication
};

// Query filters
struct MessageFilter {
    std::optional<MessageType> type;
    std::optional<MessageStatus> status;
    std::optional<std::string> from_callsign;
    std::optional<std::string> to_callsign;
    std::optional<int64_t> since_timestamp;
    int limit = 100;
    int offset = 0;
};

class MessageStore {
public:
    MessageStore();
    ~MessageStore();

    // Non-copyable
    MessageStore(const MessageStore&) = delete;
    MessageStore& operator=(const MessageStore&) = delete;

    // Database management
    bool open(const std::string& db_path);
    void close();
    bool isOpen() const { std::lock_guard<std::mutex> lock(mutex_); return db_ != nullptr; }

    // Message CRUD operations
    int64_t createMessage(const Message& msg);
    std::optional<Message> getMessage(int64_t id);
    std::optional<Message> getMessageById(const std::string& message_id);
    bool updateMessage(const Message& msg);
    bool deleteMessage(int64_t id);

    // Query operations
    std::vector<Message> getMessages(const MessageFilter& filter = {});
    std::vector<Message> getPendingMessages(const std::string& dest_callsign = "");
    std::vector<Message> getInboxMessages(const std::string& my_callsign);
    std::vector<Message> getOutboxMessages(const std::string& my_callsign);
    int countPendingMessages();

    // Status updates
    bool markAsSent(int64_t id);
    bool markAsDelivered(int64_t id);
    bool markAsFailed(int64_t id);
    bool incrementRetry(int64_t id);

    // Maintenance
    int deleteExpiredMessages();
    int deleteOldMessages(int64_t before_timestamp);
    void vacuum();

    // Generate unique message ID
    static std::string generateMessageId(const std::string& callsign);

    // Beacon history
    int64_t saveBeacon(const BeaconRecord& beacon);
    std::vector<BeaconRecord> loadRecentBeacons(int limit = 100);
    int cleanupOldBeacons(int64_t max_age_sec);

    // Ping history
    int64_t savePingResult(const PingRecord& ping);
    std::vector<PingRecord> loadRecentPings(int limit = 100);
    int cleanupOldPings(int64_t max_age_sec);

    // Mail read-state persistence (keyed by message_id)
    bool setMailReadState(const std::string& message_id, bool read);
    std::optional<bool> getMailReadState(const std::string& message_id);
    std::map<std::string, bool> loadMailReadStates(int limit = 5000);

    // Weather cache
    struct WeatherCacheEntry {
        int64_t id = 0;
        std::string type;
        std::string location;
        std::string report;
        int64_t timestamp = 0;
    };
    int64_t saveWeatherReport(const std::string& type, const std::string& location,
                              const std::string& report, int64_t timestamp = 0);
    std::vector<WeatherCacheEntry> loadWeatherReports(int limit = 50);
    int cleanupOldWeather(int64_t max_age_sec);

    // Mail read-state cleanup
    int cleanupOldMailReadStates(int64_t max_age_sec);

private:
    void* db_ = nullptr;  // sqlite3* (opaque to avoid header dependency)
    mutable std::mutex mutex_;

    bool createTables();
    bool executeQuery(const std::string& sql);
    Message rowToMessage(void* stmt);
};

} // namespace skylink
