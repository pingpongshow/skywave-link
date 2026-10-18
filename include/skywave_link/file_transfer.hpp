// FileTransfer - File transfer management with chunking and progress

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <map>
#include <chrono>
#include <fstream>

namespace skylink {

// Forward declarations
class ModemClient;

// Maximum file size (50 KB)
constexpr size_t MAX_FILE_SIZE = 50 * 1024;

// Chunk size for transmission (fits in one frame with overhead)
constexpr size_t FILE_CHUNK_SIZE = 200;

// File transfer state
enum class TransferState {
    Idle,
    Pending,        // Queued for transfer
    Negotiating,    // Waiting for accept/reject
    Transferring,   // Actively sending/receiving
    Completed,
    Failed,
    Cancelled
};

// Transfer direction
enum class TransferDirection {
    Upload,
    Download
};

// Transfer progress info
struct TransferProgress {
    std::string transfer_id;
    std::string filename;
    std::string remote_callsign;
    TransferDirection direction = TransferDirection::Upload;
    TransferState state = TransferState::Idle;
    size_t total_bytes = 0;
    size_t transferred_bytes = 0;
    size_t total_chunks = 0;
    size_t completed_chunks = 0;
    int64_t started_at = 0;
    int64_t last_activity = 0;
    std::string error_message;

    float progressPercent() const {
        return total_bytes > 0 ? (100.0f * transferred_bytes / total_bytes) : 0.0f;
    }

    int estimatedSecondsRemaining(int bytes_per_second = 100) const {
        if (bytes_per_second <= 0 || transferred_bytes >= total_bytes) return 0;
        size_t remaining = total_bytes - transferred_bytes;
        return static_cast<int>(remaining / bytes_per_second);
    }
};

// File transfer protocol messages
enum class FileMessageType : uint8_t {
    FileOffer = 0x10,       // Offer to send file
    FileAccept = 0x11,      // Accept file transfer
    FileReject = 0x12,      // Reject file transfer
    FileChunk = 0x13,       // File data chunk
    FileAck = 0x14,         // Acknowledge chunk
    FileNack = 0x15,        // Request retransmit
    FileComplete = 0x16,    // Transfer complete
    FileCancel = 0x17,      // Cancel transfer
    FileResume = 0x18       // Resume interrupted transfer
};

// Callbacks
using TransferProgressCallback = std::function<void(const TransferProgress&)>;
using TransferCompleteCallback = std::function<void(const std::string& transfer_id, bool success, const std::string& path)>;
// What the app decides about an incoming offer. Defer keeps the offer open
// (no reply sent) until acceptPendingOffer()/rejectPendingOffer(), the
// pending-offer timeout, or the end of the radio session.
enum class FileOfferDecision { Accept, Reject, Defer };
using FileOfferCallback = std::function<FileOfferDecision(const std::string& transfer_id,
                                                          const std::string& from,
                                                          const std::string& filename, size_t size)>;

// An offer waiting for the operator's decision.
struct PendingFileOffer {
    std::string transfer_id;
    std::string from;
    std::string filename;
    size_t size = 0;
    int64_t offered_at = 0;      // Unix seconds
};

// Deferred offers left unanswered this long are rejected.
constexpr int DEFAULT_PENDING_OFFER_TIMEOUT_SEC = 120;
// At most this many offers wait at once; further deferred offers are rejected.
constexpr size_t MAX_PENDING_OFFERS = 8;

class FileTransferManager {
public:
    FileTransferManager();
    ~FileTransferManager();

    // Configuration
    void setDownloadPath(const std::string& path) { download_path_ = path; }
    void setAutoAccept(bool auto_accept) { auto_accept_ = auto_accept; }
    // Replace the whole list (config / settings sync). Callsigns compare
    // trimmed and case-insensitively.
    void setTrustedCallsigns(const std::vector<std::string>& callsigns);
    void addTrustedCallsign(const std::string& callsign);
    void removeTrustedCallsign(const std::string& callsign);

    // Initiate outbound transfer
    std::string sendFile(ModemClient& modem, const std::string& remote_callsign,
                         const std::string& filepath);

    // Handle incoming file offer
    void acceptTransfer(ModemClient& modem, const std::string& transfer_id);
    void rejectTransfer(ModemClient& modem, const std::string& transfer_id);

    // Deferred offers (see FileOfferDecision::Defer). Accept/reject send the
    // same packets as an immediate decision; false if the id is not pending.
    bool acceptPendingOffer(ModemClient& modem, const std::string& transfer_id);
    bool rejectPendingOffer(ModemClient& modem, const std::string& transfer_id);
    // Radio session ended: forget pending offers without replying.
    void dropPendingOffers();
    std::vector<PendingFileOffer> getPendingOffers() const;  // oldest first
    void setPendingOfferTimeout(std::chrono::seconds timeout) { pending_offer_timeout_ = timeout; }

    // Cancel active transfer
    void cancelTransfer(ModemClient& modem, const std::string& transfer_id);

    // Process incoming data (call from data receive handler)
    void processIncomingData(ModemClient& modem, const std::vector<uint8_t>& data);

    // Periodic update (call from main loop for timeouts, retries)
    void update(ModemClient& modem);

    // Query transfers
    std::vector<TransferProgress> getActiveTransfers() const;
    std::vector<TransferProgress> getCompletedTransfers() const;
    TransferProgress getTransfer(const std::string& transfer_id) const;

    // Filename helpers (exposed for testing)
    // Truncate to at most max_bytes (the wire length field is one byte),
    // keeping a short extension and never splitting a UTF-8 sequence.
    static std::string truncateFilename(const std::string& name, size_t max_bytes = 255);
    // dir/name if free, else dir/"stem (1).ext", "stem (2).ext", ...; never
    // returns an existing path. Empty string if no free name was found.
    static std::string uniqueDownloadPath(const std::string& dir, const std::string& name);

    // Callbacks
    void onProgress(TransferProgressCallback cb) { progress_callback_ = std::move(cb); }
    void onComplete(TransferCompleteCallback cb) { complete_callback_ = std::move(cb); }
    void onFileOffer(FileOfferCallback cb) { offer_callback_ = std::move(cb); }

private:
    // Internal transfer state
    struct Transfer {
        TransferProgress progress;
        std::vector<uint8_t> data;           // File data (compressed for upload)
        std::vector<bool> chunk_received;    // For downloads: which chunks received
        std::string local_path;              // Local file path
        uint32_t crc32 = 0;                  // CRC for integrity
        int retry_count = 0;
        int current_chunk = 0;               // For uploads: current chunk being sent
        std::chrono::steady_clock::time_point last_send;
    };

    std::map<std::string, Transfer> transfers_;
    std::string download_path_;
    bool auto_accept_ = false;
    std::vector<std::string> trusted_callsigns_;
    // Deferred offers: transfer id -> time offered. The offer itself lives in
    // transfers_ in state Negotiating.
    std::map<std::string, std::chrono::steady_clock::time_point> pending_offers_;
    std::chrono::seconds pending_offer_timeout_{DEFAULT_PENDING_OFFER_TIMEOUT_SEC};

    TransferProgressCallback progress_callback_;
    TransferCompleteCallback complete_callback_;
    FileOfferCallback offer_callback_;

    // Generate unique transfer ID
    std::string generateTransferId();

    // Build protocol messages
    std::vector<uint8_t> buildFileOffer(const Transfer& t);
    std::vector<uint8_t> buildFileAccept(const std::string& transfer_id);
    std::vector<uint8_t> buildFileReject(const std::string& transfer_id);
    std::vector<uint8_t> buildFileChunk(const Transfer& t, int chunk_index);
    std::vector<uint8_t> buildFileAck(const std::string& transfer_id, int chunk_index);
    std::vector<uint8_t> buildFileNack(const std::string& transfer_id, int chunk_index);
    std::vector<uint8_t> buildFileComplete(const std::string& transfer_id, uint32_t crc);
    std::vector<uint8_t> buildFileCancel(const std::string& transfer_id);

    // Parse incoming messages
    void handleFileOffer(ModemClient& modem, const std::vector<uint8_t>& data);
    void handleFileAccept(ModemClient& modem, const std::vector<uint8_t>& data);
    void handleFileReject(const std::vector<uint8_t>& data);
    void handleFileChunk(ModemClient& modem, const std::vector<uint8_t>& data);
    void handleFileAck(ModemClient& modem, const std::vector<uint8_t>& data);
    void handleFileNack(ModemClient& modem, const std::vector<uint8_t>& data);
    void handleFileComplete(const std::vector<uint8_t>& data);
    void handleFileCancel(const std::vector<uint8_t>& data);

    // Helper functions
    void sendNextChunk(ModemClient& modem, Transfer& t);
    void completeDownload(Transfer& t);
    void notifyProgress(const Transfer& t);
    uint32_t calculateCRC32(const std::vector<uint8_t>& data);
    bool isTrusted(const std::string& callsign) const;
    void failPendingOffer(const std::string& transfer_id, const std::string& reason);
};

} // namespace skylink
