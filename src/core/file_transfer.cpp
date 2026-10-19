// FileTransfer implementation - File transfer with chunking and progress

#include "skywave_link/file_transfer.hpp"
#include "skywave_link/modem_client.hpp"
#include "skywave_link/lzh.hpp"
#include <iostream>
#include <fstream>
#include <random>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <exception>
#include <cstdlib>

namespace skylink {

namespace {

std::string shortTransferId(const std::string& id) {
    if (id.size() <= 8) {
        return id;
    }
    return id.substr(0, 8);
}

} // namespace

FileTransferManager::FileTransferManager() {
    download_path_ = "downloads";
}

FileTransferManager::~FileTransferManager() = default;

namespace {
std::string trustKey(const std::string& callsign) {
    const size_t a = callsign.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const size_t b = callsign.find_last_not_of(" \t\r\n");
    std::string out = callsign.substr(a, b - a + 1);
    for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}
}  // namespace

void FileTransferManager::setTrustedCallsigns(const std::vector<std::string>& callsigns) {
    trusted_callsigns_.clear();
    for (const auto& c : callsigns) addTrustedCallsign(c);
}

void FileTransferManager::addTrustedCallsign(const std::string& callsign) {
    const std::string key = trustKey(callsign);
    if (!key.empty() &&
        std::find(trusted_callsigns_.begin(), trusted_callsigns_.end(), key)
            == trusted_callsigns_.end()) {
        trusted_callsigns_.push_back(key);
    }
}

void FileTransferManager::removeTrustedCallsign(const std::string& callsign) {
    auto it = std::find(trusted_callsigns_.begin(), trusted_callsigns_.end(), trustKey(callsign));
    if (it != trusted_callsigns_.end()) {
        trusted_callsigns_.erase(it);
    }
}

bool FileTransferManager::isTrusted(const std::string& callsign) const {
    const std::string key = trustKey(callsign);
    return !key.empty() &&
           std::find(trusted_callsigns_.begin(), trusted_callsigns_.end(), key)
               != trusted_callsigns_.end();
}

std::string FileTransferManager::generateTransferId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint32_t> dist;

    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%08X%04X",
                  static_cast<uint32_t>(ms & 0xFFFFFFFF),
                  static_cast<uint16_t>(dist(gen) & 0xFFFF));
    return std::string(buf);
}

uint32_t FileTransferManager::calculateCRC32(const std::vector<uint8_t>& data) {
    // CRC-32 (IEEE 802.3)
    static const uint32_t crc_table[256] = {
        0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F,
        0xE963A535, 0x9E6495A3, 0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
        0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91, 0x1DB71064, 0x6AB020F2,
        0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
        0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9,
        0xFA0F3D63, 0x8D080DF5, 0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
        0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B, 0x35B5A8FA, 0x42B2986C,
        0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
        0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423,
        0xCFBA9599, 0xB8BDA50F, 0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
        0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D, 0x76DC4190, 0x01DB7106,
        0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
        0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D,
        0x91646C97, 0xE6635C01, 0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
        0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457, 0x65B0D9C6, 0x12B7E950,
        0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
        0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7,
        0xA4D1C46D, 0xD3D6F4FB, 0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
        0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9, 0x5005713C, 0x270241AA,
        0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
        0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81,
        0xB7BD5C3B, 0xC0BA6CAD, 0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
        0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683, 0xE3630B12, 0x94643B84,
        0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
        0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB,
        0x196C3671, 0x6E6B06E7, 0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
        0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5, 0xD6D6A3E8, 0xA1D1937E,
        0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
        0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55,
        0x316E8EEF, 0x4669BE79, 0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
        0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F, 0xC5BA3BBE, 0xB2BD0B28,
        0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
        0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F,
        0x72076785, 0x05005713, 0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
        0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21, 0x86D3D2D4, 0xF1D4E242,
        0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
        0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69,
        0x616BFFD3, 0x166CCF45, 0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
        0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB, 0xAED16A4A, 0xD9D65ADC,
        0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
        0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD706B3,
        0x54DE5729, 0x23D967BF, 0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
        0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D
    };

    uint32_t crc = 0xFFFFFFFF;
    for (uint8_t byte : data) {
        crc = crc_table[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

namespace {

// The LZH payload of an incompressible file is slightly larger than the
// file; offers are bounded with that headroom.
constexpr size_t kMaxFilePayload = MAX_FILE_SIZE + MAX_FILE_SIZE / 8 + 1024;
// ModemClient frames each session message with a 4-byte header.
constexpr size_t kSessionFrameOverhead = 4;
constexpr auto kBacklogQueryInterval = std::chrono::milliseconds(250);

void putLe32(std::vector<uint8_t>& m, uint32_t v) {
    m.push_back(static_cast<uint8_t>(v & 0xFF));
    m.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    m.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    m.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

uint32_t getLe32(const std::vector<uint8_t>& d, size_t pos) {
    return static_cast<uint32_t>(d[pos])
         | (static_cast<uint32_t>(d[pos + 1]) << 8)
         | (static_cast<uint32_t>(d[pos + 2]) << 16)
         | (static_cast<uint32_t>(d[pos + 3]) << 24);
}

int64_t unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool isActive(TransferState s) {
    return s == TransferState::Pending || s == TransferState::Negotiating ||
           s == TransferState::Transferring;
}

} // namespace

bool FileTransferManager::linkUp(const ModemClient& modem) {
    return modem.isConnected() && modem.getStatus().link_state == LinkState::Connected;
}

bool FileTransferManager::send(ModemClient& modem, const std::vector<uint8_t>& msg) {
    if (!linkUp(modem)) return false;
    if (!modem.sendData(msg)) return false;
    bytes_sent_ += msg.size() + kSessionFrameOverhead;
    return true;
}

std::string FileTransferManager::sendFile(ModemClient& modem, const std::string& remote_callsign,
                                           const std::string& filepath) {
    if (!linkUp(modem)) {
        std::cerr << "[FileTransfer] Not linked to a station; cannot send " << filepath << "\n";
        return "";
    }

    // Check if file exists and is within size limit
    std::error_code ec;
    auto file_size = std::filesystem::file_size(filepath, ec);
    if (ec) {
        std::cerr << "[FileTransfer] Cannot read file: " << filepath << "\n";
        return "";
    }

    if (file_size > MAX_FILE_SIZE) {
        std::cerr << "[FileTransfer] File too large: " << file_size << " bytes (max "
                  << MAX_FILE_SIZE << ")\n";
        return "";
    }
    if (file_size == 0) {
        std::cerr << "[FileTransfer] Refusing zero-byte file: " << filepath << "\n";
        return "";
    }

    std::ifstream file(filepath, std::ios::binary);
    if (!file) {
        std::cerr << "[FileTransfer] Cannot open file: " << filepath << "\n";
        return "";
    }

    std::vector<uint8_t> file_data(file_size);
    file.read(reinterpret_cast<char*>(file_data.data()), file_size);
    if (!file || file.gcount() != static_cast<std::streamsize>(file_size)) {
        std::cerr << "[FileTransfer] Failed to read complete file: " << filepath << "\n";
        return "";
    }

    LZHCodec lzh;
    std::vector<uint8_t> compressed = lzh.compress(file_data);
    if (compressed.empty() || compressed.size() > kMaxFilePayload) {
        std::cerr << "[FileTransfer] Compression failed: " << filepath << "\n";
        return "";
    }

    Transfer t;
    t.progress.transfer_id = generateTransferId();
    // The offer carries a one-byte length: truncate long names here so the
    // length byte and the bytes actually sent always agree.
    t.progress.filename = truncateFilename(std::filesystem::path(filepath).filename().string());
    t.progress.remote_callsign = remote_callsign;
    t.progress.direction = TransferDirection::Upload;
    t.progress.state = TransferState::Negotiating;
    t.progress.total_bytes = compressed.size();
    t.progress.started_at = unixNow();
    t.progress.last_activity = t.progress.started_at;
    t.data = std::move(compressed);
    t.local_path = filepath;
    t.crc32 = calculateCRC32(file_data);
    t.last_activity = Clock::now();

    const std::string transfer_id = t.progress.transfer_id;
    const std::vector<uint8_t> offer = buildFileOffer(t);
    transfers_[transfer_id] = std::move(t);
    if (!send(modem, offer)) {
        transfers_.erase(transfer_id);
        std::cerr << "[FileTransfer] Could not send the offer for " << filepath << "\n";
        return "";
    }
    notifyProgress(transfers_[transfer_id]);
    return transfer_id;
}

void FileTransferManager::acceptTransfer(ModemClient& modem, const std::string& transfer_id) {
    auto it = transfers_.find(transfer_id);
    if (it == transfers_.end()) {
        return;
    }
    it->second.progress.state = TransferState::Transferring;
    it->second.last_activity = Clock::now();
    send(modem, buildIdMessage(FileMessageType::FileAccept, transfer_id));
}

void FileTransferManager::rejectTransfer(ModemClient& modem, const std::string& transfer_id) {
    auto it = transfers_.find(transfer_id);
    if (it == transfers_.end()) {
        return;
    }
    it->second.progress.state = TransferState::Failed;
    it->second.progress.error_message = "Rejected by user";
    send(modem, buildIdMessage(FileMessageType::FileReject, transfer_id));
}

void FileTransferManager::cancelTransfer(ModemClient& modem, const std::string& transfer_id) {
    auto it = transfers_.find(transfer_id);
    if (it == transfers_.end() || !isActive(it->second.progress.state)) {
        return;
    }
    pending_offers_.erase(transfer_id);
    it->second.progress.state = TransferState::Cancelled;
    it->second.progress.error_message = "Cancelled";
    send(modem, buildIdMessage(FileMessageType::FileCancel, transfer_id));
    notifyProgress(it->second);
}

void FileTransferManager::linkLost(const std::string& reason) {
    std::vector<std::string> deferred;
    for (const auto& [id, offered] : pending_offers_) {
        (void)offered;
        deferred.push_back(id);
    }
    pending_offers_.clear();
    for (const auto& id : deferred) {
        failPendingOffer(id, "Offer dropped: session ended");
    }
    for (auto& [id, t] : transfers_) {
        (void)id;
        if (isActive(t.progress.state)) {
            failTransfer(t, reason);
        }
    }
}

void FileTransferManager::processIncomingData(ModemClient& modem, const std::vector<uint8_t>& data) {
    if (data.size() < 3) return;

    try {
        const auto type = static_cast<FileMessageType>(data[0]);
        if (type == FileMessageType::FileOffer) {
            handleFileOffer(modem, data);
            return;
        }

        size_t pos = 1;
        const uint8_t id_len = data[pos++];
        if (id_len == 0 || pos + id_len > data.size()) return;
        const std::string id(data.begin() + static_cast<std::ptrdiff_t>(pos),
                             data.begin() + static_cast<std::ptrdiff_t>(pos + id_len));
        pos += id_len;

        switch (type) {
            case FileMessageType::FileAccept:
                handleFileAccept(modem, id);
                break;
            case FileMessageType::FileReject:
                handleFileReject(id);
                break;
            case FileMessageType::FileChunk:
                handleFileChunk(modem, id, data, pos);
                break;
            case FileMessageType::FileComplete:
                handleFileComplete(modem, id, data, pos);
                break;
            case FileMessageType::FileAck:
                handleFileAck(id, data, pos);
                break;
            case FileMessageType::FileCancel:
                handleFileCancel(id);
                break;
            default:
                // Not a file transfer message
                break;
        }
    } catch (const std::exception& e) {
        std::cerr << "[FileTransfer] Exception while parsing incoming data: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "[FileTransfer] Unknown exception while parsing incoming data\n";
    }
}

void FileTransferManager::update(ModemClient& modem) {
    // Nothing is ever sent without a station link; whatever was in flight
    // over a link that is gone has failed.
    if (!linkUp(modem)) {
        linkLost();
        return;
    }

    const auto now = Clock::now();

    // Deferred offers nobody answered are rejected.
    for (auto pit = pending_offers_.begin(); pit != pending_offers_.end();) {
        if (now - pit->second >= pending_offer_timeout_) {
            const std::string id = pit->first;
            pit = pending_offers_.erase(pit);
            auto it = transfers_.find(id);
            if (it != transfers_.end() && it->second.progress.state == TransferState::Negotiating) {
                send(modem, buildIdMessage(FileMessageType::FileReject, id));
                failPendingOffer(id, "Offer timed out");
            }
        } else {
            ++pit;
        }
    }

    pumpUploads(modem);

    // One long inactivity timeout replaces per-chunk retransmission: the
    // modem's ARQ already delivers every byte in order or drops the link.
    for (auto& [id, t] : transfers_) {
        const TransferState s = t.progress.state;
        if (s != TransferState::Negotiating && s != TransferState::Transferring) continue;
        if (pending_offers_.count(id) > 0) continue;  // the operator is deciding
        if (now - t.last_activity < inactivity_timeout_) continue;
        const bool unanswered = (s == TransferState::Negotiating &&
                                 t.progress.direction == TransferDirection::Upload);
        send(modem, buildIdMessage(FileMessageType::FileCancel, id));
        failTransfer(t, unanswered ? "No answer to the file offer"
                                   : "Timed out (no progress)");
    }
}

size_t FileTransferManager::estimatedBacklog(const ModemClient& modem) const {
    // The last BUFFER answer, plus everything written since the query it
    // answers, plus bytes still waiting in the client's socket queue.
    const uint64_t since = bytes_sent_ > backlog_->sent_at_reply
                               ? bytes_sent_ - backlog_->sent_at_reply : 0;
    return backlog_->reported + static_cast<size_t>(since) + modem.pendingDataBytes();
}

void FileTransferManager::queryBacklog(ModemClient& modem) {
    const auto now = Clock::now();
    if (backlog_->query_outstanding || now - last_backlog_query_ < kBacklogQueryInterval) {
        return;
    }
    last_backlog_query_ = now;
    backlog_->query_outstanding = true;
    const uint64_t sent_at = bytes_sent_;
    backlog_->sent_at_query = sent_at;
    std::weak_ptr<BacklogState> weak = backlog_;
    modem.sendCommandAsync("BUFFER", [weak, sent_at](bool ok, const std::string& response) {
        auto st = weak.lock();
        if (!st) return;
        st->query_outstanding = false;
        if (!ok) return;
        // `BUFFER <bytes>`
        size_t value = 0;
        const size_t sp = response.find(' ');
        if (sp != std::string::npos) {
            value = static_cast<size_t>(std::strtoull(response.c_str() + sp + 1, nullptr, 10));
        }
        if (value != st->reported) st->changed = true;
        st->reported = value;
        st->sent_at_reply = sent_at;
    });
}

void FileTransferManager::pumpUploads(ModemClient& modem) {
    bool any = false;
    for (const auto& [id, t] : transfers_) {
        (void)id;
        if (t.progress.direction == TransferDirection::Upload &&
            t.progress.state == TransferState::Transferring) {
            any = true;
            break;
        }
    }
    if (!any) return;

    queryBacklog(modem);
    const auto now = Clock::now();
    size_t backlog = estimatedBacklog(modem);
    const bool draining = backlog_->changed;
    backlog_->changed = false;

    for (auto& [id, t] : transfers_) {
        (void)id;
        if (t.progress.direction != TransferDirection::Upload ||
            t.progress.state != TransferState::Transferring) {
            continue;
        }
        if (draining) t.last_activity = now;

        while (!t.complete_sent && t.progress.state == TransferState::Transferring) {
            std::vector<uint8_t> msg;
            size_t len = 0;
            if (t.next_offset < t.data.size()) {
                if (backlog >= FILE_TX_BACKLOG_TARGET) break;
                len = std::min(FILE_CHUNK_SIZE, t.data.size() - t.next_offset);
                msg = buildFileChunk(t, t.next_offset, len);
            } else {
                msg = buildFileComplete(t);
            }
            if (!send(modem, msg)) {
                failTransfer(t, "Could not hand data to the modem");
                break;
            }
            if (len > 0) {
                t.next_offset += len;
            } else {
                t.complete_sent = true;
            }
            backlog += msg.size() + kSessionFrameOverhead;
            t.last_activity = now;
        }
        if (t.progress.state != TransferState::Transferring) continue;

        // Bytes still queued in the modem have not gone out yet.
        const size_t aired = t.next_offset - std::min(t.next_offset, backlog);
        if (aired > t.progress.transferred_bytes) {
            t.progress.transferred_bytes = aired;
            t.progress.last_activity = unixNow();
            notifyProgress(t);
        }
    }
}

std::vector<TransferProgress> FileTransferManager::getActiveTransfers() const {
    std::vector<TransferProgress> result;
    for (const auto& [id, t] : transfers_) {
        (void)id;
        if (isActive(t.progress.state)) {
            result.push_back(t.progress);
        }
    }
    return result;
}

std::vector<TransferProgress> FileTransferManager::getCompletedTransfers() const {
    std::vector<TransferProgress> result;
    for (const auto& [id, t] : transfers_) {
        (void)id;
        if (t.progress.state == TransferState::Completed ||
            t.progress.state == TransferState::Failed ||
            t.progress.state == TransferState::Cancelled) {
            result.push_back(t.progress);
        }
    }
    return result;
}

TransferProgress FileTransferManager::getTransfer(const std::string& transfer_id) const {
    auto it = transfers_.find(transfer_id);
    if (it != transfers_.end()) {
        return it->second.progress;
    }
    return TransferProgress{};
}

// Build protocol messages (see FileMessageType for the layouts)

std::vector<uint8_t> FileTransferManager::buildIdMessage(FileMessageType type,
                                                         const std::string& transfer_id) {
    std::vector<uint8_t> msg;
    msg.push_back(static_cast<uint8_t>(type));
    msg.push_back(static_cast<uint8_t>(transfer_id.size()));
    msg.insert(msg.end(), transfer_id.begin(), transfer_id.end());
    return msg;
}

std::vector<uint8_t> FileTransferManager::buildFileOffer(const Transfer& t) {
    std::vector<uint8_t> msg = buildIdMessage(FileMessageType::FileOffer, t.progress.transfer_id);
    const std::string name = truncateFilename(t.progress.filename);
    msg.push_back(static_cast<uint8_t>(name.size()));
    msg.insert(msg.end(), name.begin(), name.end());
    putLe32(msg, static_cast<uint32_t>(t.progress.total_bytes));
    putLe32(msg, t.crc32);
    return msg;
}

std::vector<uint8_t> FileTransferManager::buildFileChunk(const Transfer& t, size_t offset, size_t len) {
    std::vector<uint8_t> msg = buildIdMessage(FileMessageType::FileChunk, t.progress.transfer_id);
    putLe32(msg, static_cast<uint32_t>(offset));
    msg.insert(msg.end(), t.data.begin() + static_cast<std::ptrdiff_t>(offset),
               t.data.begin() + static_cast<std::ptrdiff_t>(offset + len));
    return msg;
}

std::vector<uint8_t> FileTransferManager::buildFileComplete(const Transfer& t) {
    std::vector<uint8_t> msg = buildIdMessage(FileMessageType::FileComplete, t.progress.transfer_id);
    putLe32(msg, static_cast<uint32_t>(t.data.size()));
    putLe32(msg, calculateCRC32(t.data));
    return msg;
}

std::vector<uint8_t> FileTransferManager::buildFileAck(const std::string& transfer_id, bool ok,
                                                       const std::string& reason) {
    std::vector<uint8_t> msg = buildIdMessage(FileMessageType::FileAck, transfer_id);
    msg.push_back(ok ? 0 : 1);
    msg.insert(msg.end(), reason.begin(), reason.end());
    return msg;
}

// Handle incoming messages

void FileTransferManager::handleFileOffer(ModemClient& modem, const std::vector<uint8_t>& data) {
    size_t pos = 1;
    const uint8_t id_len = data[pos++];
    if (id_len == 0 || pos + id_len > data.size()) return;

    std::string transfer_id(data.begin() + static_cast<std::ptrdiff_t>(pos),
                            data.begin() + static_cast<std::ptrdiff_t>(pos + id_len));
    pos += id_len;

    if (pos >= data.size()) return;
    const uint8_t name_len = data[pos++];
    if (name_len == 0 || pos + name_len > data.size()) return;

    std::string raw_name(data.begin() + static_cast<std::ptrdiff_t>(pos),
                         data.begin() + static_cast<std::ptrdiff_t>(pos + name_len));
    pos += name_len;

    // Sanitize filename: strip path components to prevent directory traversal
    std::string filename = std::filesystem::path(raw_name).filename().string();
    if (filename.empty() || filename == "." || filename == "..") {
        send(modem, buildIdMessage(FileMessageType::FileReject, transfer_id));
        return;
    }

    if (pos + 8 > data.size()) return;
    const uint32_t size = getLe32(data, pos);
    const uint32_t crc = getLe32(data, pos + 4);

    if (size == 0 || size > kMaxFilePayload) {
        send(modem, buildIdMessage(FileMessageType::FileReject, transfer_id));
        return;
    }

    // A repeated offer for an id already known changes nothing.
    auto existing = transfers_.find(transfer_id);
    if (existing != transfers_.end() && isActive(existing->second.progress.state)) {
        return;
    }

    std::string remote = modem.getStatus().remote_callsign;

    Transfer t;
    t.progress.transfer_id = transfer_id;
    t.progress.filename = filename;
    t.progress.remote_callsign = remote;
    t.progress.direction = TransferDirection::Download;
    t.progress.state = TransferState::Negotiating;
    t.progress.total_bytes = size;
    t.progress.started_at = unixNow();
    t.progress.last_activity = t.progress.started_at;
    t.data.reserve(size);
    t.crc32 = crc;
    t.last_activity = Clock::now();

    transfers_[transfer_id] = std::move(t);

    // Auto-accept or ask the app, which may defer to the operator.
    FileOfferDecision decision = (auto_accept_ || isTrusted(remote))
        ? FileOfferDecision::Accept : FileOfferDecision::Reject;
    if (offer_callback_ && decision != FileOfferDecision::Accept) {
        decision = offer_callback_(transfer_id, remote, filename, size);
    }

    if (decision == FileOfferDecision::Defer) {
        if (pending_offers_.size() < MAX_PENDING_OFFERS) {
            pending_offers_[transfer_id] = Clock::now();
            notifyProgress(transfers_[transfer_id]);
            return;
        }
        decision = FileOfferDecision::Reject;
    }

    if (decision == FileOfferDecision::Accept) {
        acceptTransfer(modem, transfer_id);
    } else {
        rejectTransfer(modem, transfer_id);
    }
    notifyProgress(transfers_[transfer_id]);
}

bool FileTransferManager::acceptPendingOffer(ModemClient& modem, const std::string& transfer_id) {
    if (pending_offers_.erase(transfer_id) == 0) {
        return false;
    }
    auto it = transfers_.find(transfer_id);
    if (it == transfers_.end() || it->second.progress.state != TransferState::Negotiating) {
        return false;
    }
    acceptTransfer(modem, transfer_id);
    notifyProgress(it->second);
    return true;
}

bool FileTransferManager::rejectPendingOffer(ModemClient& modem, const std::string& transfer_id) {
    if (pending_offers_.erase(transfer_id) == 0) {
        return false;
    }
    auto it = transfers_.find(transfer_id);
    if (it == transfers_.end() || it->second.progress.state != TransferState::Negotiating) {
        return false;
    }
    rejectTransfer(modem, transfer_id);
    notifyProgress(it->second);
    return true;
}

void FileTransferManager::dropPendingOffers() {
    auto pending = std::move(pending_offers_);
    pending_offers_.clear();
    for (const auto& [id, offered] : pending) {
        (void)offered;
        failPendingOffer(id, "Offer dropped: session ended");
    }
}

std::vector<PendingFileOffer> FileTransferManager::getPendingOffers() const {
    std::vector<PendingFileOffer> result;
    for (const auto& [id, offered] : pending_offers_) {
        (void)offered;
        auto it = transfers_.find(id);
        if (it == transfers_.end()) continue;
        PendingFileOffer o;
        o.transfer_id = id;
        o.from = it->second.progress.remote_callsign;
        o.filename = it->second.progress.filename;
        o.size = it->second.progress.total_bytes;
        o.offered_at = it->second.progress.started_at;
        result.push_back(std::move(o));
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const PendingFileOffer& a, const PendingFileOffer& b) {
                         return a.offered_at < b.offered_at;
                     });
    return result;
}

void FileTransferManager::failPendingOffer(const std::string& transfer_id, const std::string& reason) {
    auto it = transfers_.find(transfer_id);
    if (it == transfers_.end() || it->second.progress.state != TransferState::Negotiating) {
        return;
    }
    it->second.progress.state = TransferState::Failed;
    it->second.progress.error_message = reason;
    notifyProgress(it->second);
}

void FileTransferManager::handleFileAccept(ModemClient& modem, const std::string& id) {
    auto it = transfers_.find(id);
    if (it == transfers_.end()) return;
    Transfer& t = it->second;
    if (t.progress.direction != TransferDirection::Upload ||
        t.progress.state != TransferState::Negotiating) {
        return;
    }
    t.progress.state = TransferState::Transferring;
    t.next_offset = 0;
    t.complete_sent = false;
    t.last_activity = Clock::now();
    notifyProgress(t);
    pumpUploads(modem);
}

void FileTransferManager::handleFileReject(const std::string& id) {
    auto it = transfers_.find(id);
    if (it == transfers_.end()) return;
    if (it->second.progress.direction != TransferDirection::Upload) return;
    if (!isActive(it->second.progress.state)) return;
    failTransfer(it->second, "Rejected by remote");
}

void FileTransferManager::handleFileChunk(ModemClient& modem, const std::string& id,
                                          const std::vector<uint8_t>& data, size_t pos) {
    auto it = transfers_.find(id);
    if (it == transfers_.end()) return;
    Transfer& t = it->second;
    if (t.progress.direction != TransferDirection::Download ||
        t.progress.state != TransferState::Transferring) {
        return;
    }
    if (pos + 4 > data.size()) return;
    const size_t offset = getLe32(data, pos);
    pos += 4;
    const size_t len = data.size() - pos;
    const size_t have = t.data.size();
    t.last_activity = Clock::now();

    // The session is ordered and reliable: a gap means data was lost
    // somewhere outside the modem's guarantees, and cannot be repaired.
    if (offset > have) {
        const std::string reason = "Data missing at byte " + std::to_string(have);
        send(modem, buildFileAck(id, false, reason));
        failTransfer(t, reason);
        return;
    }
    if (offset + len > t.progress.total_bytes) {
        const std::string reason = "More data than offered";
        send(modem, buildFileAck(id, false, reason));
        failTransfer(t, reason);
        return;
    }
    if (offset + len <= have) {
        return;  // a replay (EVT:STREAM_REPLAY) of bytes already held
    }
    const size_t skip = have - offset;
    t.data.insert(t.data.end(), data.begin() + static_cast<std::ptrdiff_t>(pos + skip), data.end());
    t.progress.transferred_bytes = t.data.size();
    t.progress.last_activity = unixNow();
    notifyProgress(t);
}

void FileTransferManager::handleFileComplete(ModemClient& modem, const std::string& id,
                                             const std::vector<uint8_t>& data, size_t pos) {
    auto it = transfers_.find(id);
    if (it == transfers_.end()) return;
    Transfer& t = it->second;
    if (t.progress.direction != TransferDirection::Download ||
        t.progress.state != TransferState::Transferring) {
        return;
    }
    if (pos + 8 > data.size()) return;
    const uint32_t size = getLe32(data, pos);
    const uint32_t payload_crc = getLe32(data, pos + 4);
    t.last_activity = Clock::now();

    std::string reason;
    if (size != t.progress.total_bytes) {
        reason = "Size mismatch";
    } else if (t.data.size() != size) {
        reason = "Incomplete: " + std::to_string(t.data.size()) + " of " +
                 std::to_string(size) + " bytes";
    } else if (calculateCRC32(t.data) != payload_crc) {
        reason = "CRC mismatch";
    }
    if (!reason.empty()) {
        send(modem, buildFileAck(id, false, reason));
        failTransfer(t, reason);
        return;
    }
    finishDownload(modem, t);
}

void FileTransferManager::handleFileAck(const std::string& id, const std::vector<uint8_t>& data,
                                        size_t pos) {
    auto it = transfers_.find(id);
    if (it == transfers_.end()) return;
    Transfer& t = it->second;
    if (t.progress.direction != TransferDirection::Upload ||
        t.progress.state != TransferState::Transferring) {
        return;
    }
    if (pos >= data.size()) return;
    const bool ok = data[pos] == 0;
    const std::string reason(data.begin() + static_cast<std::ptrdiff_t>(pos + 1), data.end());
    if (!ok) {
        failTransfer(t, "Receiver reported: " + (reason.empty() ? std::string("failed") : reason));
        return;
    }
    t.progress.state = TransferState::Completed;
    t.progress.transferred_bytes = t.progress.total_bytes;
    t.progress.last_activity = unixNow();
    notifyProgress(t);
    if (complete_callback_) {
        complete_callback_(id, true, t.local_path);
    }
}

void FileTransferManager::handleFileCancel(const std::string& id) {
    auto it = transfers_.find(id);
    if (it == transfers_.end() || !isActive(it->second.progress.state)) return;
    if (pending_offers_.erase(id) > 0) {
        failPendingOffer(id, "Cancelled by remote");
        return;
    }
    failTransfer(it->second, "Cancelled by remote");
    it->second.progress.state = TransferState::Cancelled;
}

void FileTransferManager::finishDownload(ModemClient& modem, Transfer& t) {
    LZHCodec lzh;
    const std::vector<uint8_t> decompressed = lzh.decompress(t.data);
    const uint32_t actual_crc = calculateCRC32(decompressed);
    if (actual_crc != t.crc32) {
        std::cerr << "[FileTransfer] FAIL id=" << shortTransferId(t.progress.transfer_id)
                  << " reason=crc_mismatch expected=0x" << std::hex << t.crc32
                  << " actual=0x" << actual_crc << std::dec << "\n";
        send(modem, buildFileAck(t.progress.transfer_id, false, "CRC mismatch"));
        failTransfer(t, "CRC mismatch");
        return;
    }

    std::error_code dir_ec;
    std::filesystem::create_directories(download_path_, dir_ec);
    // Never overwrite an existing file: pick "name (1).ext" etc.
    const std::filesystem::path out = uniqueDownloadPath(download_path_, t.progress.filename);

    std::ofstream file;
    if (!out.empty()) {
        file.open(out, std::ios::binary);
    }
    if (file) {
        file.write(reinterpret_cast<const char*>(decompressed.data()),
                   static_cast<std::streamsize>(decompressed.size()));
        file.close();
    }
    if (!file) {
        std::cerr << "[FileTransfer] FAIL id=" << shortTransferId(t.progress.transfer_id)
                  << " reason=cannot_write_file path=" << out.string() << "\n";
        send(modem, buildFileAck(t.progress.transfer_id, false, "Receiver cannot write the file"));
        failTransfer(t, "Cannot write file");
        return;
    }

    send(modem, buildFileAck(t.progress.transfer_id, true, ""));
    t.progress.state = TransferState::Completed;
    t.progress.transferred_bytes = t.progress.total_bytes;
    t.local_path = out.string();
    notifyProgress(t);
    if (complete_callback_) {
        complete_callback_(t.progress.transfer_id, true, t.local_path);
    }
}

void FileTransferManager::failTransfer(Transfer& t, const std::string& reason) {
    if (!isActive(t.progress.state)) return;
    pending_offers_.erase(t.progress.transfer_id);
    t.progress.state = TransferState::Failed;
    t.progress.error_message = reason;
    std::cerr << "[FileTransfer] FAIL id=" << shortTransferId(t.progress.transfer_id)
              << " file=" << t.progress.filename << " reason=" << reason << "\n";
    notifyProgress(t);
    if (complete_callback_) {
        complete_callback_(t.progress.transfer_id, false, t.progress.filename);
    }
}

std::string FileTransferManager::truncateFilename(const std::string& name, size_t max_bytes) {
    if (name.size() <= max_bytes) return name;

    // Keep a short extension (".jpg", ".tar.gz" tail) when it fits.
    std::string ext;
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0 && name.size() - dot <= 16 &&
        name.size() - dot < max_bytes) {
        ext = name.substr(dot);
    }
    size_t keep = max_bytes - ext.size();
    // Back off to a UTF-8 character boundary (continuation bytes are 10xxxxxx).
    while (keep > 0 && (static_cast<unsigned char>(name[keep]) & 0xC0u) == 0x80u) {
        --keep;
    }
    return name.substr(0, keep) + ext;
}

std::string FileTransferManager::uniqueDownloadPath(const std::string& dir, const std::string& name) {
    const std::filesystem::path base = std::filesystem::path(dir) / name;
    std::error_code ec;
    if (!std::filesystem::exists(base, ec) && !ec) {
        return base.string();
    }
    const std::filesystem::path p(name);
    const std::string stem = p.stem().string();
    const std::string ext = p.extension().string();
    for (int n = 1; n < 10000; ++n) {
        const std::string suffix = " (" + std::to_string(n) + ")" + ext;
        // Stay within the 255-byte filename limit after adding the suffix.
        const size_t room = suffix.size() < 255 ? 255 - suffix.size() : 0;
        const std::filesystem::path candidate =
            std::filesystem::path(dir) / (truncateFilename(stem, room) + suffix);
        ec.clear();
        if (!std::filesystem::exists(candidate, ec) && !ec) {
            return candidate.string();
        }
    }
    return "";
}

void FileTransferManager::notifyProgress(const Transfer& t) {
    if (progress_callback_) {
        progress_callback_(t.progress);
    }
}

} // namespace skylink
