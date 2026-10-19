// FileTransfer - File transfer management with chunking and progress

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <map>
#include <chrono>
#include <fstream>
#include <memory>

namespace skylink {

// Forward declarations
class ModemClient;

// Default limit on a file's size (files.max_file_size; 50 KB). The limit
// in force is FileTransferManager::setMaxFileSize().
constexpr size_t MAX_FILE_SIZE = 50 * 1024;
// Hard ceiling for any configured limit (the offer carries a u32 and the
// whole file is held in memory).
constexpr size_t MAX_FILE_SIZE_CEILING = 16u * 1024u * 1024u;

// Payload bytes per FileChunk message. The modem's ARQ session is reliable
// and ordered, so chunks are streamed back to back with no per-chunk
// acknowledgement; the size only bounds one data-port message.
constexpr size_t FILE_CHUNK_SIZE = 2048;

// Flow control: new chunks are written only while the modem's TX backlog
// (BUFFER, plus bytes written since that answer) is below this, so a file is
// never dumped into the modem at once and a cancel is not stuck behind it.
constexpr size_t FILE_TX_BACKLOG_TARGET = 8192;

// A transfer with no progress at all (nothing sent or drained, nothing
// received) for this long fails. Generous on purpose: at the lowest modes
// one chunk alone takes minutes, and the receiver's operator may take up to
// DEFAULT_PENDING_OFFER_TIMEOUT_SEC to answer an offer.
constexpr int DEFAULT_FILE_INACTIVITY_TIMEOUT_SEC = 180;

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
    int64_t started_at = 0;
    int64_t last_activity = 0;
    std::string error_message;
    // Download: a remote station's upload into this station's BBS folder
    // (FileOfferKind::BbsUpload). Upload: we offered it as one.
    bool bbs_upload = false;

    float progressPercent() const {
        return total_bytes > 0 ? (100.0f * transferred_bytes / total_bytes) : 0.0f;
    }

    int estimatedSecondsRemaining(int bytes_per_second = 100) const {
        if (bytes_per_second <= 0 || transferred_bytes >= total_bytes) return 0;
        size_t remaining = total_bytes - transferred_bytes;
        return static_cast<int>(remaining / bytes_per_second);
    }
};

// File transfer protocol messages (session data, framed by ModemClient).
// Every message starts with type(1) + id_len(1) + id; integers are LE.
//   FileOffer    + name_len(1) + name + payload_size(4) + file_crc32(4)
//                  + flags(1)  (bit 0: BBS upload)
//   FileAccept / FileReject / FileCancel   (id only)
//   FileChunk    + offset(4) + data          (data length = rest of message)
//   FileComplete + payload_size(4) + payload_crc32(4)   (after the last chunk)
//   FileAck      + status(1: 0 ok, 1 failed) + reason text
//                  (receiver's final verdict after the size/CRC check)
// The payload is the LZH-compressed file; file_crc32 covers the original.
enum class FileMessageType : uint8_t {
    FileOffer = 0x10,       // Offer to send file
    FileAccept = 0x11,      // Accept file transfer
    FileReject = 0x12,      // Reject file transfer
    FileChunk = 0x13,       // File data at an offset
    FileAck = 0x14,         // Receiver's final verdict
    FileComplete = 0x16,    // Sender: all data written
    FileCancel = 0x17,      // Cancel transfer
    FileLast = 0x18         // End of the reserved file-transfer range
};

// What an outgoing offer is: a file for the operator at the other end, or
// an upload into the remote station's BBS folder.
enum class FileOfferKind { Direct, BbsUpload };
inline constexpr uint8_t kFileOfferFlagBbsUpload = 0x01;

// When this station accepts remote BBS uploads: allow_uploads AND the
// sender in allowed_uploaders (an empty list admits nobody), and no larger
// than max_upload_size. Accepted uploads need no operator decision and are
// written to `directory` (never overwriting a file).
struct BbsUploadPolicy {
    bool allow_uploads = false;
    std::string directory;
    std::vector<std::string> allowed_uploaders;
    size_t max_upload_size = 50 * 1024;
};
// Empty when allowed, else the reason given to the sender.
std::string bbsUploadRefusal(const BbsUploadPolicy& policy, const std::string& from,
                             size_t file_size);

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
    // files.max_file_size: the largest file sent or accepted (clamped to
    // 1 KB .. MAX_FILE_SIZE_CEILING).
    void setMaxFileSize(size_t bytes);
    size_t maxFileSize() const { return max_file_size_; }
    void setBbsUploadPolicy(const BbsUploadPolicy& policy) { bbs_policy_ = policy; }

    // Initiate outbound transfer. Returns the transfer id, or "" with the
    // reason in lastSendError().
    std::string sendFile(ModemClient& modem, const std::string& remote_callsign,
                         const std::string& filepath,
                         FileOfferKind kind = FileOfferKind::Direct);
    const std::string& lastSendError() const { return last_send_error_; }

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

    // The radio session ended (or the modem went away): every transfer still
    // negotiating or transferring fails with `reason`, and pending offers are
    // dropped. Nothing is sent. update() does the same by itself whenever it
    // sees the station link is not Connected.
    void linkLost(const std::string& reason = "Link lost");

    // Process incoming data (call from data receive handler)
    void processIncomingData(ModemClient& modem, const std::vector<uint8_t>& data);

    // Periodic update (call from the main loop): streams upload data under
    // flow control, expires idle transfers and unanswered offers. Never
    // sends anything unless the station link is Connected.
    void update(ModemClient& modem);
    void setInactivityTimeout(std::chrono::seconds timeout) { inactivity_timeout_ = timeout; }

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

    // Callbacks. onComplete fires once per transfer that ends on its own:
    // success, or failure for any reason other than this station's own
    // decision (reject/cancel); `path` is the file (or its name on failure).
    void onProgress(TransferProgressCallback cb) { progress_callback_ = std::move(cb); }
    void onComplete(TransferCompleteCallback cb) { complete_callback_ = std::move(cb); }
    void onFileOffer(FileOfferCallback cb) { offer_callback_ = std::move(cb); }

private:
    using Clock = std::chrono::steady_clock;

    // Internal transfer state
    struct Transfer {
        TransferProgress progress;
        std::vector<uint8_t> data;           // LZH payload (upload: all of it; download: received so far)
        std::string local_path;              // Local file path
        uint32_t crc32 = 0;                  // CRC-32 of the original file
        size_t next_offset = 0;              // Upload: payload bytes written to the modem
        bool complete_sent = false;          // Upload: FileComplete written
        bool complete_received = false;      // Download: FileComplete seen
        size_t max_original_size = 0;        // Download: limit on the decompressed file
        std::string target_dir;              // Download: where the file goes
        Clock::time_point last_activity;
    };

    // Modem TX backlog bookkeeping shared by all uploads. BUFFER replies
    // arrive through a callback that may outlive this object (it is held by
    // the ModemClient), so the state it touches lives behind a shared_ptr.
    struct BacklogState {
        bool query_outstanding = false;
        size_t reported = 0;                 // last BUFFER answer
        uint64_t sent_at_query = 0;          // bytes_sent when that query went out
        uint64_t sent_at_reply = 0;          // ... for the query that `reported` answers
        bool changed = false;                // the last answer differed from the one before
    };
    std::shared_ptr<BacklogState> backlog_ = std::make_shared<BacklogState>();
    uint64_t bytes_sent_ = 0;                // data-port bytes this manager has written
    Clock::time_point last_backlog_query_{};

    std::map<std::string, Transfer> transfers_;
    std::string download_path_;
    bool auto_accept_ = false;
    size_t max_file_size_ = MAX_FILE_SIZE;
    BbsUploadPolicy bbs_policy_;
    std::string last_send_error_;
    std::vector<std::string> trusted_callsigns_;
    // Deferred offers: transfer id -> time offered. The offer itself lives in
    // transfers_ in state Negotiating.
    std::map<std::string, Clock::time_point> pending_offers_;
    std::chrono::seconds pending_offer_timeout_{DEFAULT_PENDING_OFFER_TIMEOUT_SEC};
    std::chrono::seconds inactivity_timeout_{DEFAULT_FILE_INACTIVITY_TIMEOUT_SEC};

    TransferProgressCallback progress_callback_;
    TransferCompleteCallback complete_callback_;
    FileOfferCallback offer_callback_;

    // Generate unique transfer ID
    std::string generateTransferId();

    // Build protocol messages
    std::vector<uint8_t> buildFileOffer(const Transfer& t);
    std::vector<uint8_t> buildIdMessage(FileMessageType type, const std::string& transfer_id);
    std::vector<uint8_t> buildFileChunk(const Transfer& t, size_t offset, size_t len);
    std::vector<uint8_t> buildFileComplete(const Transfer& t);
    std::vector<uint8_t> buildFileAck(const std::string& transfer_id, bool ok,
                                      const std::string& reason);

    // Parse incoming messages
    void handleFileOffer(ModemClient& modem, const std::vector<uint8_t>& data);
    void handleFileAccept(ModemClient& modem, const std::string& id);
    void handleFileReject(const std::string& id);
    void handleFileChunk(ModemClient& modem, const std::string& id,
                         const std::vector<uint8_t>& data, size_t pos);
    void handleFileComplete(ModemClient& modem, const std::string& id,
                            const std::vector<uint8_t>& data, size_t pos);
    void handleFileAck(const std::string& id, const std::vector<uint8_t>& data, size_t pos);
    void handleFileCancel(const std::string& id);

    // Helper functions
    bool send(ModemClient& modem, const std::vector<uint8_t>& msg);
    static bool linkUp(const ModemClient& modem);
    size_t estimatedBacklog(const ModemClient& modem) const;
    void queryBacklog(ModemClient& modem);
    void pumpUploads(ModemClient& modem);
    void finishDownload(ModemClient& modem, Transfer& t);
    void failTransfer(Transfer& t, const std::string& reason);
    void notifyProgress(const Transfer& t);
    static uint32_t calculateCRC32(const std::vector<uint8_t>& data);
    bool isTrusted(const std::string& callsign) const;
    void failPendingOffer(const std::string& transfer_id, const std::string& reason);
};

} // namespace skylink
