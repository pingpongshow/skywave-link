// MessageStore implementation - SQLite-based message persistence

#include "skywave_link/message_store.hpp"
#include <iostream>
#include <sstream>
#include <random>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <cmath>
#include <mutex>
#include <algorithm>

#ifdef SKYWAVE_LINK_HAS_SQLITE
#include <sqlite3.h>
#endif

namespace skylink {

namespace {

constexpr uint32_t kMaxAttachmentCount = 64;
constexpr uint32_t kMaxAttachmentBlobBytes = 4 * 1024 * 1024;
constexpr uint32_t kMaxAttachmentFieldBytes = 1024 * 1024;

void appendU32LE(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

bool readU32LE(const uint8_t*& ptr, size_t& remaining, uint32_t& out) {
    if (remaining < 4) {
        return false;
    }
    out = static_cast<uint32_t>(ptr[0])
        | (static_cast<uint32_t>(ptr[1]) << 8)
        | (static_cast<uint32_t>(ptr[2]) << 16)
        | (static_cast<uint32_t>(ptr[3]) << 24);
    ptr += 4;
    remaining -= 4;
    return true;
}

bool readBytes(const uint8_t*& ptr, size_t& remaining, uint32_t len, std::string& out) {
    if (len > remaining) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(ptr), reinterpret_cast<const char*>(ptr + len));
    ptr += len;
    remaining -= len;
    return true;
}

bool readBytes(const uint8_t*& ptr, size_t& remaining, uint32_t len, std::vector<uint8_t>& out) {
    if (len > remaining) {
        return false;
    }
    out.assign(ptr, ptr + len);
    ptr += len;
    remaining -= len;
    return true;
}

std::vector<uint8_t> serializeAttachments(const std::vector<Attachment>& attachments) {
    if (attachments.empty()) {
        return {};
    }
    if (attachments.size() > kMaxAttachmentCount) {
        return {};
    }

    std::vector<uint8_t> blob;
    blob.reserve(64);
    appendU32LE(blob, static_cast<uint32_t>(attachments.size()));

    for (const auto& att : attachments) {
        if (att.filename.size() > kMaxAttachmentFieldBytes ||
            att.mime_type.size() > kMaxAttachmentFieldBytes ||
            att.data.size() > kMaxAttachmentBlobBytes) {
            return {};
        }
        if (blob.size() > kMaxAttachmentBlobBytes) {
            return {};
        }

        appendU32LE(blob, static_cast<uint32_t>(att.filename.size()));
        blob.insert(blob.end(), att.filename.begin(), att.filename.end());

        appendU32LE(blob, static_cast<uint32_t>(att.mime_type.size()));
        blob.insert(blob.end(), att.mime_type.begin(), att.mime_type.end());

        appendU32LE(blob, static_cast<uint32_t>(att.data.size()));
        blob.insert(blob.end(), att.data.begin(), att.data.end());
    }

    if (blob.size() > kMaxAttachmentBlobBytes) {
        return {};
    }
    return blob;
}

std::vector<Attachment> deserializeAttachments(const void* data, int bytes) {
    std::vector<Attachment> attachments;
    if (data == nullptr || bytes <= 0) {
        return attachments;
    }

    const auto* ptr = static_cast<const uint8_t*>(data);
    size_t remaining = static_cast<size_t>(bytes);
    if (remaining > kMaxAttachmentBlobBytes) {
        return {};
    }

    uint32_t count = 0;
    if (!readU32LE(ptr, remaining, count) || count > kMaxAttachmentCount) {
        return {};
    }

    attachments.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Attachment att;
        uint32_t filename_len = 0;
        uint32_t mime_len = 0;
        uint32_t data_len = 0;

        if (!readU32LE(ptr, remaining, filename_len) || filename_len > kMaxAttachmentFieldBytes) {
            return {};
        }
        if (!readBytes(ptr, remaining, filename_len, att.filename)) {
            return {};
        }

        if (!readU32LE(ptr, remaining, mime_len) || mime_len > kMaxAttachmentFieldBytes) {
            return {};
        }
        if (!readBytes(ptr, remaining, mime_len, att.mime_type)) {
            return {};
        }

        if (!readU32LE(ptr, remaining, data_len) || data_len > kMaxAttachmentBlobBytes) {
            return {};
        }
        if (!readBytes(ptr, remaining, data_len, att.data)) {
            return {};
        }

        attachments.push_back(std::move(att));
    }

    return attachments;
}

}  // namespace

MessageStore::MessageStore() = default;

MessageStore::~MessageStore() {
    close();
}

bool MessageStore::open(const std::string& db_path) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        sqlite3_close_v2(static_cast<sqlite3*>(db_));
        db_ = nullptr;
    }

    sqlite3* db = nullptr;
    int rc = sqlite3_open(db_path.c_str(), &db);
    if (rc != SQLITE_OK) {
        std::cerr << "[MessageStore] Failed to open database: " << sqlite3_errmsg(db) << "\n";
        sqlite3_close_v2(db);
        return false;
    }

    db_ = db;

    // Enable WAL mode for better concurrent access
    executeQuery("PRAGMA journal_mode=WAL");
    executeQuery("PRAGMA synchronous=NORMAL");
    sqlite3_busy_timeout(db, 5000);  // 5 second busy timeout

    if (!createTables()) {
        sqlite3_close_v2(static_cast<sqlite3*>(db_));
        db_ = nullptr;
        return false;
    }

    return true;
#else
    (void)db_path;
    std::cerr << "[MessageStore] SQLite support not compiled in\n";
    return false;
#endif
}

void MessageStore::close() {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (db_) {
        sqlite3_close_v2(static_cast<sqlite3*>(db_));
        db_ = nullptr;
    }
#endif
}

bool MessageStore::createTables() {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS messages (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            message_id TEXT UNIQUE,
            type INTEGER NOT NULL,
            status INTEGER NOT NULL DEFAULT 0,
            from_callsign TEXT NOT NULL,
            to_callsign TEXT NOT NULL,
            subject TEXT,
            body TEXT,
            attachments BLOB,
            created_at INTEGER NOT NULL,
            sent_at INTEGER,
            delivered_at INTEGER,
            expires_at INTEGER,
            retry_count INTEGER DEFAULT 0,
            priority INTEGER NOT NULL DEFAULT 0
        );

        CREATE INDEX IF NOT EXISTS idx_messages_status ON messages(status);
        CREATE INDEX IF NOT EXISTS idx_messages_to ON messages(to_callsign);
        CREATE INDEX IF NOT EXISTS idx_messages_from ON messages(from_callsign);
        CREATE INDEX IF NOT EXISTS idx_messages_created ON messages(created_at);
        CREATE INDEX IF NOT EXISTS idx_messages_expires ON messages(expires_at);

        CREATE TABLE IF NOT EXISTS positions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            callsign TEXT NOT NULL,
            latitude REAL NOT NULL,
            longitude REAL NOT NULL,
            altitude REAL,
            speed REAL,
            course REAL,
            comment TEXT,
            timestamp INTEGER NOT NULL,
            source TEXT
        );

        CREATE INDEX IF NOT EXISTS idx_positions_callsign ON positions(callsign);
        CREATE INDEX IF NOT EXISTS idx_positions_timestamp ON positions(timestamp);

        CREATE TABLE IF NOT EXISTS contacts (
            callsign TEXT PRIMARY KEY,
            name TEXT,
            last_seen INTEGER,
            last_frequency INTEGER,
            notes TEXT
        );

        CREATE TABLE IF NOT EXISTS beacon_history (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            callsign TEXT NOT NULL,
            grid_square TEXT,
            snr_db REAL,
            message TEXT,
            has_pending INTEGER DEFAULT 0,
            email_gateway INTEGER DEFAULT 0,
            band TEXT,
            timestamp INTEGER NOT NULL
        );

        CREATE INDEX IF NOT EXISTS idx_beacon_callsign ON beacon_history(callsign);
        CREATE INDEX IF NOT EXISTS idx_beacon_timestamp ON beacon_history(timestamp);

        CREATE TABLE IF NOT EXISTS ping_history (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            callsign TEXT NOT NULL,
            snr_db REAL,
            grid_square TEXT,
            band TEXT,
            online INTEGER DEFAULT 1,
            timestamp INTEGER NOT NULL
        );

        CREATE INDEX IF NOT EXISTS idx_ping_callsign ON ping_history(callsign);
        CREATE INDEX IF NOT EXISTS idx_ping_timestamp ON ping_history(timestamp);

        CREATE TABLE IF NOT EXISTS mail_read_state (
            message_id TEXT PRIMARY KEY,
            is_read INTEGER NOT NULL DEFAULT 0,
            updated_at INTEGER NOT NULL
        );

        CREATE INDEX IF NOT EXISTS idx_mail_read_updated ON mail_read_state(updated_at);

        CREATE TABLE IF NOT EXISTS weather_cache (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            type TEXT NOT NULL,
            location TEXT NOT NULL,
            report TEXT NOT NULL,
            timestamp INTEGER NOT NULL
        );

        CREATE INDEX IF NOT EXISTS idx_weather_timestamp ON weather_cache(timestamp);
    )";

    if (!executeQuery(sql)) {
        return false;
    }

    auto hasColumn = [this](const char* table, const char* column) -> bool {
        std::string pragma = std::string("PRAGMA table_info(") + table + ")";
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), pragma.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            return false;
        }
        bool found = false;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (name && std::string(name) == column) {
                found = true;
                break;
            }
        }
        sqlite3_finalize(stmt);
        return found;
    };

    // Schema migrations for existing installations.
    if (!hasColumn("ping_history", "band")) {
        if (!executeQuery("ALTER TABLE ping_history ADD COLUMN band TEXT")) {
            std::cerr << "[MessageStore] Warning: failed to add 'band' column to ping_history\n";
        }
    }
    if (!hasColumn("mail_read_state", "updated_at")) {
        if (!executeQuery("ALTER TABLE mail_read_state ADD COLUMN updated_at INTEGER NOT NULL DEFAULT 0")) {
            std::cerr << "[MessageStore] Warning: failed to add 'updated_at' column to mail_read_state\n";
        }
    }
    if (!hasColumn("messages", "priority")) {
        if (!executeQuery("ALTER TABLE messages ADD COLUMN priority INTEGER NOT NULL DEFAULT 0")) {
            std::cerr << "[MessageStore] Warning: failed to add 'priority' column to messages\n";
        }
    }
    return true;
#else
    return false;
#endif
}

bool MessageStore::executeQuery(const std::string& sql) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    if (!db_) return false;

    char* err_msg = nullptr;
    int rc = sqlite3_exec(static_cast<sqlite3*>(db_), sql.c_str(), nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "[MessageStore] SQL error: " << err_msg << "\n";
        sqlite3_free(err_msg);
        return false;
    }
    return true;
#else
    (void)sql;
    return false;
#endif
}

int64_t MessageStore::createMessage(const Message& msg) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return -1;

    const char* sql = R"(
        INSERT INTO messages (message_id, type, status, from_callsign, to_callsign,
                              subject, body, attachments, created_at, expires_at, retry_count, priority)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return -1;
    }

    std::string message_id = msg.message_id.empty() ? generateMessageId(msg.from_callsign) : msg.message_id;
    const std::vector<uint8_t> attachments_blob = serializeAttachments(msg.attachments);
    if (!msg.attachments.empty() && attachments_blob.empty()) {
        sqlite3_finalize(stmt);
        return -1;
    }
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    sqlite3_bind_text(stmt, 1, message_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, static_cast<int>(msg.type));
    sqlite3_bind_int(stmt, 3, static_cast<int>(msg.status));
    sqlite3_bind_text(stmt, 4, msg.from_callsign.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, msg.to_callsign.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, msg.subject.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, msg.body.c_str(), -1, SQLITE_TRANSIENT);
    if (attachments_blob.empty()) {
        sqlite3_bind_null(stmt, 8);
    } else {
        sqlite3_bind_blob(stmt, 8, attachments_blob.data(),
                          static_cast<int>(attachments_blob.size()), SQLITE_TRANSIENT);
    }
    sqlite3_bind_int64(stmt, 9, msg.created_at > 0 ? msg.created_at : now);
    sqlite3_bind_int64(stmt, 10, msg.expires_at);
    sqlite3_bind_int(stmt, 11, msg.retry_count);
    sqlite3_bind_int(stmt, 12, msg.priority);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        std::cerr << "[MessageStore] createMessage failed: " << sqlite3_errmsg(static_cast<sqlite3*>(db_)) << "\n";
    }
    int64_t id = -1;
    if (rc == SQLITE_DONE) {
        id = sqlite3_last_insert_rowid(static_cast<sqlite3*>(db_));
    }

    sqlite3_finalize(stmt);
    return id;
#else
    (void)msg;
    return -1;
#endif
}

std::optional<Message> MessageStore::getMessage(int64_t id) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;

    const char* sql = "SELECT id, message_id, type, status, from_callsign, to_callsign, "
                      "subject, body, attachments, created_at, sent_at, delivered_at, "
                      "expires_at, retry_count, priority FROM messages WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_int64(stmt, 1, id);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    Message msg = rowToMessage(stmt);
    sqlite3_finalize(stmt);
    return msg;
#else
    (void)id;
    return std::nullopt;
#endif
}

std::optional<Message> MessageStore::getMessageById(const std::string& message_id) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return std::nullopt;

    const char* sql = "SELECT id, message_id, type, status, from_callsign, to_callsign, "
                      "subject, body, attachments, created_at, sent_at, delivered_at, "
                      "expires_at, retry_count, priority FROM messages WHERE message_id = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return std::nullopt;
    }

    sqlite3_bind_text(stmt, 1, message_id.c_str(), -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    Message msg = rowToMessage(stmt);
    sqlite3_finalize(stmt);
    return msg;
#else
    (void)message_id;
    return std::nullopt;
#endif
}

bool MessageStore::updateMessage(const Message& msg) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || msg.id == 0) return false;

    const char* sql = R"(
        UPDATE messages SET
            type = ?, status = ?, from_callsign = ?, to_callsign = ?,
            subject = ?, body = ?, attachments = ?, sent_at = ?, delivered_at = ?,
            expires_at = ?, retry_count = ?, priority = ?
        WHERE id = ?
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }

    const std::vector<uint8_t> attachments_blob = serializeAttachments(msg.attachments);
    if (!msg.attachments.empty() && attachments_blob.empty()) {
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_bind_int(stmt, 1, static_cast<int>(msg.type));
    sqlite3_bind_int(stmt, 2, static_cast<int>(msg.status));
    sqlite3_bind_text(stmt, 3, msg.from_callsign.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, msg.to_callsign.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, msg.subject.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, msg.body.c_str(), -1, SQLITE_TRANSIENT);
    if (attachments_blob.empty()) {
        sqlite3_bind_null(stmt, 7);
    } else {
        sqlite3_bind_blob(stmt, 7, attachments_blob.data(),
                          static_cast<int>(attachments_blob.size()), SQLITE_TRANSIENT);
    }
    sqlite3_bind_int64(stmt, 8, msg.sent_at);
    sqlite3_bind_int64(stmt, 9, msg.delivered_at);
    sqlite3_bind_int64(stmt, 10, msg.expires_at);
    sqlite3_bind_int(stmt, 11, msg.retry_count);
    sqlite3_bind_int(stmt, 12, msg.priority);
    sqlite3_bind_int64(stmt, 13, msg.id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(static_cast<sqlite3*>(db_)) > 0;
#else
    (void)msg;
    return false;
#endif
}

bool MessageStore::deleteMessage(int64_t id) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "DELETE FROM messages WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && sqlite3_changes(static_cast<sqlite3*>(db_)) > 0;
#else
    (void)id;
    return false;
#endif
}

std::vector<Message> MessageStore::getMessages(const MessageFilter& filter) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return {};

    int safe_limit = std::max(1, std::min(filter.limit, 10000));
    int safe_offset = std::max(0, filter.offset);

    std::stringstream sql;
    sql << "SELECT id, message_id, type, status, from_callsign, to_callsign, "
           "subject, body, attachments, created_at, sent_at, delivered_at, "
           "expires_at, retry_count, priority FROM messages WHERE 1=1";

    if (filter.type) {
        sql << " AND type = ?";
    }
    if (filter.status) {
        sql << " AND status = ?";
    }
    if (filter.from_callsign) {
        sql << " AND from_callsign = ?";
    }
    if (filter.to_callsign) {
        sql << " AND to_callsign = ?";
    }
    if (filter.since_timestamp) {
        sql << " AND created_at >= ?";
    }

    sql << " ORDER BY created_at DESC";
    sql << " LIMIT ? OFFSET ?";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql.str().c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return {};
    }

    int bind_idx = 1;
    if (filter.type) {
        sqlite3_bind_int(stmt, bind_idx++, static_cast<int>(*filter.type));
    }
    if (filter.status) {
        sqlite3_bind_int(stmt, bind_idx++, static_cast<int>(*filter.status));
    }
    if (filter.from_callsign) {
        sqlite3_bind_text(stmt, bind_idx++, filter.from_callsign->c_str(), -1, SQLITE_TRANSIENT);
    }
    if (filter.to_callsign) {
        sqlite3_bind_text(stmt, bind_idx++, filter.to_callsign->c_str(), -1, SQLITE_TRANSIENT);
    }
    if (filter.since_timestamp) {
        sqlite3_bind_int64(stmt, bind_idx++, *filter.since_timestamp);
    }
    sqlite3_bind_int(stmt, bind_idx++, safe_limit);
    sqlite3_bind_int(stmt, bind_idx++, safe_offset);

    std::vector<Message> messages;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        messages.push_back(rowToMessage(stmt));
    }

    sqlite3_finalize(stmt);
    return messages;
#else
    (void)filter;
    return {};
#endif
}

std::vector<Message> MessageStore::getPendingMessages(const std::string& dest_callsign) {
    MessageFilter filter;
    filter.status = MessageStatus::Pending;
    if (!dest_callsign.empty()) {
        filter.to_callsign = dest_callsign;
    }
    return getMessages(filter);
}

std::vector<Message> MessageStore::getInboxMessages(const std::string& my_callsign) {
    MessageFilter filter;
    filter.to_callsign = my_callsign;
    return getMessages(filter);
}

std::vector<Message> MessageStore::getOutboxMessages(const std::string& my_callsign) {
    MessageFilter filter;
    filter.from_callsign = my_callsign;
    return getMessages(filter);
}

int MessageStore::countPendingMessages() {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0;

    const char* sql = "SELECT COUNT(*) FROM messages WHERE status = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return 0;
    }

    sqlite3_bind_int(stmt, 1, static_cast<int>(MessageStatus::Pending));
    rc = sqlite3_step(stmt);
    int count = 0;
    if (rc == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }

    sqlite3_finalize(stmt);
    return count;
#else
    return 0;
#endif
}

bool MessageStore::markAsSent(int64_t id) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    const char* sql = "UPDATE messages SET status = ?, sent_at = ? WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int(stmt, 1, static_cast<int>(MessageStatus::Sent));
    sqlite3_bind_int64(stmt, 2, now);
    sqlite3_bind_int64(stmt, 3, id);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        std::cerr << "[MessageStore] markAsSent failed: " << sqlite3_errmsg(static_cast<sqlite3*>(db_)) << "\n";
    }
    bool changed = (rc == SQLITE_DONE && sqlite3_changes(static_cast<sqlite3*>(db_)) > 0);
    sqlite3_finalize(stmt);
    return changed;
#else
    (void)id;
    return false;
#endif
}

bool MessageStore::markAsDelivered(int64_t id) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    const char* sql = "UPDATE messages SET status = ?, delivered_at = ? WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int(stmt, 1, static_cast<int>(MessageStatus::Delivered));
    sqlite3_bind_int64(stmt, 2, now);
    sqlite3_bind_int64(stmt, 3, id);

    rc = sqlite3_step(stmt);
    bool changed = (rc == SQLITE_DONE && sqlite3_changes(static_cast<sqlite3*>(db_)) > 0);
    sqlite3_finalize(stmt);
    return changed;
#else
    (void)id;
    return false;
#endif
}

bool MessageStore::markAsFailed(int64_t id) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "UPDATE messages SET status = ? WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int(stmt, 1, static_cast<int>(MessageStatus::Failed));
    sqlite3_bind_int64(stmt, 2, id);

    rc = sqlite3_step(stmt);
    bool changed = (rc == SQLITE_DONE && sqlite3_changes(static_cast<sqlite3*>(db_)) > 0);
    sqlite3_finalize(stmt);
    return changed;
#else
    (void)id;
    return false;
#endif
}

bool MessageStore::incrementRetry(int64_t id) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return false;

    const char* sql = "UPDATE messages SET retry_count = retry_count + 1 WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, id);

    rc = sqlite3_step(stmt);
    bool changed = (rc == SQLITE_DONE && sqlite3_changes(static_cast<sqlite3*>(db_)) > 0);
    sqlite3_finalize(stmt);
    return changed;
#else
    (void)id;
    return false;
#endif
}

int MessageStore::deleteExpiredMessages() {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0;

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    const char* sql = "DELETE FROM messages WHERE expires_at > 0 AND expires_at < ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return 0;
    }

    sqlite3_bind_int64(stmt, 1, now);
    rc = sqlite3_step(stmt);
    int deleted = sqlite3_changes(static_cast<sqlite3*>(db_));
    sqlite3_finalize(stmt);
    return deleted;
#else
    return 0;
#endif
}

int MessageStore::deleteOldMessages(int64_t before_timestamp) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0;

    const char* sql = "DELETE FROM messages WHERE created_at < ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return 0;
    }

    sqlite3_bind_int64(stmt, 1, before_timestamp);
    rc = sqlite3_step(stmt);
    int deleted = sqlite3_changes(static_cast<sqlite3*>(db_));
    sqlite3_finalize(stmt);
    return deleted;
#else
    (void)before_timestamp;
    return 0;
#endif
}

void MessageStore::vacuum() {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    executeQuery("VACUUM");
#endif
}

std::string MessageStore::generateMessageId(const std::string& callsign) {
    static std::mutex rng_mutex;
    static std::mt19937_64 gen(std::random_device{}());
    static std::uniform_int_distribution<uint64_t> dist;

    uint64_t rand_val;
    {
        std::lock_guard<std::mutex> lock(rng_mutex);
        rand_val = dist(gen);
    }

    auto now = std::chrono::system_clock::now();
    auto epoch = now.time_since_epoch();
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(epoch).count();

    std::stringstream ss;
    ss << callsign << "_" << std::hex << seconds << "_" << std::hex << (rand_val & 0xFFFFFFFFFFFF);
    return ss.str();
}

Message MessageStore::rowToMessage(void* stmt_ptr) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    sqlite3_stmt* stmt = static_cast<sqlite3_stmt*>(stmt_ptr);
    Message msg;

    msg.id = sqlite3_column_int64(stmt, 0);

    const char* message_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (message_id) msg.message_id = message_id;

    int type_val = sqlite3_column_int(stmt, 2);
    if (type_val >= 0 && type_val <= static_cast<int>(MessageType::Email)) {
        msg.type = static_cast<MessageType>(type_val);
    } else {
        msg.type = MessageType::Private;  // default for out-of-range
    }

    int status_val = sqlite3_column_int(stmt, 3);
    if (status_val >= 0 && status_val <= static_cast<int>(MessageStatus::Expired)) {
        msg.status = static_cast<MessageStatus>(status_val);
    } else {
        msg.status = MessageStatus::Draft;  // default for out-of-range
    }

    const char* from = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    if (from) msg.from_callsign = from;

    const char* to = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
    if (to) msg.to_callsign = to;

    const char* subject = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    if (subject) msg.subject = subject;

    const char* body = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
    if (body) msg.body = body;

    const void* attachments_blob = sqlite3_column_blob(stmt, 8);
    int attachments_blob_size = sqlite3_column_bytes(stmt, 8);
    msg.attachments = deserializeAttachments(attachments_blob, attachments_blob_size);

    msg.created_at = sqlite3_column_int64(stmt, 9);
    msg.sent_at = sqlite3_column_int64(stmt, 10);
    msg.delivered_at = sqlite3_column_int64(stmt, 11);
    msg.expires_at = sqlite3_column_int64(stmt, 12);
    msg.retry_count = sqlite3_column_int(stmt, 13);
    msg.priority = sqlite3_column_int(stmt, 14);

    return msg;
#else
    (void)stmt_ptr;
    return Message{};
#endif
}

// --- Beacon history ---

int64_t MessageStore::saveBeacon(const BeaconRecord& beacon) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return -1;

    const char* sql = R"(
        INSERT INTO beacon_history (callsign, grid_square, snr_db, message,
                                     has_pending, email_gateway, band, timestamp)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return -1;

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    double snr_val = std::isfinite(beacon.snr_db) ? beacon.snr_db : 0.0;
    sqlite3_bind_text(stmt, 1, beacon.callsign.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, beacon.grid_square.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 3, snr_val);
    sqlite3_bind_text(stmt, 4, beacon.message.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, beacon.has_pending ? 1 : 0);
    sqlite3_bind_int(stmt, 6, beacon.email_gateway ? 1 : 0);
    sqlite3_bind_text(stmt, 7, beacon.band.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 8, beacon.timestamp > 0 ? beacon.timestamp : now);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        std::cerr << "[MessageStore] saveBeacon failed: " << sqlite3_errmsg(static_cast<sqlite3*>(db_)) << "\n";
    }
    int64_t id = -1;
    if (rc == SQLITE_DONE) {
        id = sqlite3_last_insert_rowid(static_cast<sqlite3*>(db_));
    }
    sqlite3_finalize(stmt);
    return id;
#else
    (void)beacon;
    return -1;
#endif
}

std::vector<BeaconRecord> MessageStore::loadRecentBeacons(int limit) {
    if (limit <= 0) limit = 1;
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return {};

    const char* sql = "SELECT id, callsign, grid_square, snr_db, message, "
                      "has_pending, email_gateway, band, timestamp "
                      "FROM beacon_history ORDER BY timestamp DESC LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return {};

    sqlite3_bind_int(stmt, 1, limit);

    std::vector<BeaconRecord> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        BeaconRecord b;
        b.id = sqlite3_column_int64(stmt, 0);
        const char* cs = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (cs) b.callsign = cs;
        const char* gs = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        if (gs) b.grid_square = gs;
        b.snr_db = static_cast<float>(sqlite3_column_double(stmt, 3));
        const char* msg = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        if (msg) b.message = msg;
        b.has_pending = sqlite3_column_int(stmt, 5) != 0;
        b.email_gateway = sqlite3_column_int(stmt, 6) != 0;
        const char* band = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
        if (band) b.band = band;
        b.timestamp = sqlite3_column_int64(stmt, 8);
        results.push_back(std::move(b));
    }

    sqlite3_finalize(stmt);
    return results;
#else
    (void)limit;
    return {};
#endif
}

int MessageStore::cleanupOldBeacons(int64_t max_age_sec) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0;

    int64_t cutoff = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - max_age_sec;

    const char* sql = "DELETE FROM beacon_history WHERE timestamp < ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return 0;

    sqlite3_bind_int64(stmt, 1, cutoff);
    rc = sqlite3_step(stmt);
    int deleted = sqlite3_changes(static_cast<sqlite3*>(db_));
    sqlite3_finalize(stmt);
    return deleted;
#else
    (void)max_age_sec;
    return 0;
#endif
}

// --- Ping history ---

int64_t MessageStore::savePingResult(const PingRecord& ping) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return -1;

    const char* sql = R"(
        INSERT INTO ping_history (callsign, snr_db, grid_square, band, online, timestamp)
        VALUES (?, ?, ?, ?, ?, ?)
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return -1;

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    double snr_val = std::isfinite(ping.snr_db) ? ping.snr_db : 0.0;
    sqlite3_bind_text(stmt, 1, ping.callsign.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, snr_val);
    sqlite3_bind_text(stmt, 3, ping.grid_square.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, ping.band.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, ping.online ? 1 : 0);
    sqlite3_bind_int64(stmt, 6, ping.timestamp > 0 ? ping.timestamp : now);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        std::cerr << "[MessageStore] savePingResult failed: " << sqlite3_errmsg(static_cast<sqlite3*>(db_)) << "\n";
    }
    int64_t id = -1;
    if (rc == SQLITE_DONE) {
        id = sqlite3_last_insert_rowid(static_cast<sqlite3*>(db_));
    }
    sqlite3_finalize(stmt);
    return id;
#else
    (void)ping;
    return -1;
#endif
}

std::vector<PingRecord> MessageStore::loadRecentPings(int limit) {
    if (limit <= 0) limit = 1;
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return {};

    const char* sql = "SELECT id, callsign, snr_db, grid_square, band, online, timestamp "
                      "FROM ping_history ORDER BY timestamp DESC LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return {};

    sqlite3_bind_int(stmt, 1, limit);

    std::vector<PingRecord> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PingRecord p;
        p.id = sqlite3_column_int64(stmt, 0);
        const char* cs = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (cs) p.callsign = cs;
        p.snr_db = static_cast<float>(sqlite3_column_double(stmt, 2));
        const char* gs = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        if (gs) p.grid_square = gs;
        const char* band = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        if (band) p.band = band;
        p.online = sqlite3_column_int(stmt, 5) != 0;
        p.timestamp = sqlite3_column_int64(stmt, 6);
        results.push_back(std::move(p));
    }

    sqlite3_finalize(stmt);
    return results;
#else
    (void)limit;
    return {};
#endif
}

int MessageStore::cleanupOldPings(int64_t max_age_sec) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0;

    int64_t cutoff = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - max_age_sec;

    const char* sql = "DELETE FROM ping_history WHERE timestamp < ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return 0;

    sqlite3_bind_int64(stmt, 1, cutoff);
    rc = sqlite3_step(stmt);
    int deleted = sqlite3_changes(static_cast<sqlite3*>(db_));
    sqlite3_finalize(stmt);
    return deleted;
#else
    (void)max_age_sec;
    return 0;
#endif
}

bool MessageStore::setMailReadState(const std::string& message_id, bool read) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || message_id.empty()) return false;

    const char* sql = R"(
        INSERT INTO mail_read_state (message_id, is_read, updated_at)
        VALUES (?, ?, ?)
        ON CONFLICT(message_id) DO UPDATE SET
            is_read = excluded.is_read,
            updated_at = excluded.updated_at
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;

    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    sqlite3_bind_text(stmt, 1, message_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, read ? 1 : 0);
    sqlite3_bind_int64(stmt, 3, now);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
#else
    (void)message_id;
    (void)read;
    return false;
#endif
}

std::optional<bool> MessageStore::getMailReadState(const std::string& message_id) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_ || message_id.empty()) return std::nullopt;

    const char* sql = "SELECT is_read FROM mail_read_state WHERE message_id = ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return std::nullopt;

    sqlite3_bind_text(stmt, 1, message_id.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }

    const bool is_read = sqlite3_column_int(stmt, 0) != 0;
    sqlite3_finalize(stmt);
    return is_read;
#else
    (void)message_id;
    return std::nullopt;
#endif
}

std::map<std::string, bool> MessageStore::loadMailReadStates(int limit) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, bool> states;
    if (!db_) return states;

    const char* sql = "SELECT message_id, is_read "
                      "FROM mail_read_state ORDER BY updated_at DESC LIMIT ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return states;

    sqlite3_bind_int(stmt, 1, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (!id) continue;
        states[id] = sqlite3_column_int(stmt, 1) != 0;
    }

    sqlite3_finalize(stmt);
    return states;
#else
    (void)limit;
    return {};
#endif
}

// --- Weather cache ---

int64_t MessageStore::saveWeatherReport(const std::string& type, const std::string& location,
                                         const std::string& report, int64_t timestamp) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return -1;

    const char* sql = R"(
        INSERT INTO weather_cache (type, location, report, timestamp)
        VALUES (?, ?, ?, ?)
    )";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return -1;

    int64_t ts = timestamp;
    if (ts <= 0) {
        ts = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    sqlite3_bind_text(stmt, 1, type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, location.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, report.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, ts);

    rc = sqlite3_step(stmt);
    int64_t id = -1;
    if (rc == SQLITE_DONE) {
        id = sqlite3_last_insert_rowid(static_cast<sqlite3*>(db_));
    }
    sqlite3_finalize(stmt);
    return id;
#else
    (void)type; (void)location; (void)report; (void)timestamp;
    return -1;
#endif
}

std::vector<MessageStore::WeatherCacheEntry> MessageStore::loadWeatherReports(int limit) {
    if (limit <= 0) limit = 1;
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return {};

    const char* sql = "SELECT id, type, location, report, timestamp "
                      "FROM weather_cache ORDER BY timestamp DESC LIMIT ?";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return {};

    sqlite3_bind_int(stmt, 1, limit);

    std::vector<WeatherCacheEntry> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WeatherCacheEntry e;
        e.id = sqlite3_column_int64(stmt, 0);
        const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (t) e.type = t;
        const char* l = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        if (l) e.location = l;
        const char* r = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        if (r) e.report = r;
        e.timestamp = sqlite3_column_int64(stmt, 4);
        results.push_back(std::move(e));
    }

    sqlite3_finalize(stmt);
    return results;
#else
    (void)limit;
    return {};
#endif
}

int MessageStore::cleanupOldWeather(int64_t max_age_sec) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0;

    int64_t cutoff = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - max_age_sec;

    const char* sql = "DELETE FROM weather_cache WHERE timestamp < ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return 0;

    sqlite3_bind_int64(stmt, 1, cutoff);
    rc = sqlite3_step(stmt);
    int deleted = sqlite3_changes(static_cast<sqlite3*>(db_));
    sqlite3_finalize(stmt);
    return deleted;
#else
    (void)max_age_sec;
    return 0;
#endif
}

int MessageStore::cleanupOldMailReadStates(int64_t max_age_sec) {
#ifdef SKYWAVE_LINK_HAS_SQLITE
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0;

    int64_t cutoff = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - max_age_sec;

    const char* sql = "DELETE FROM mail_read_state WHERE updated_at < ?";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(static_cast<sqlite3*>(db_), sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return 0;

    sqlite3_bind_int64(stmt, 1, cutoff);
    rc = sqlite3_step(stmt);
    int deleted = sqlite3_changes(static_cast<sqlite3*>(db_));
    sqlite3_finalize(stmt);
    return deleted;
#else
    (void)max_age_sec;
    return 0;
#endif
}

} // namespace skylink
