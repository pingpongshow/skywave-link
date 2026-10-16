// FileTransfer implementation - File transfer with chunking and progress

#include "skywave_link/file_transfer.hpp"
#include "skywave_link/modem_client.hpp"
#include "skywave_link/lzh.hpp"
#include <iostream>
#include <fstream>
#include <random>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <exception>

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

void FileTransferManager::addTrustedCallsign(const std::string& callsign) {
    if (std::find(trusted_callsigns_.begin(), trusted_callsigns_.end(), callsign)
        == trusted_callsigns_.end()) {
        trusted_callsigns_.push_back(callsign);
    }
}

void FileTransferManager::removeTrustedCallsign(const std::string& callsign) {
    auto it = std::find(trusted_callsigns_.begin(), trusted_callsigns_.end(), callsign);
    if (it != trusted_callsigns_.end()) {
        trusted_callsigns_.erase(it);
    }
}

bool FileTransferManager::isTrusted(const std::string& callsign) const {
    return std::find(trusted_callsigns_.begin(), trusted_callsigns_.end(), callsign)
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

std::string FileTransferManager::sendFile(ModemClient& modem, const std::string& remote_callsign,
                                           const std::string& filepath) {
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

    // Read file
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

    // Compress data
    LZHCodec lzh;
    std::vector<uint8_t> compressed = lzh.compress(file_data);
    if (compressed.empty()) {
        std::cerr << "[FileTransfer] Compression produced empty payload: " << filepath << "\n";
        return "";
    }

    // Create transfer
    Transfer t;
    t.progress.transfer_id = generateTransferId();
    // The offer carries a one-byte length: truncate long names here so the
    // length byte and the bytes actually sent always agree.
    t.progress.filename = truncateFilename(std::filesystem::path(filepath).filename().string());
    t.progress.remote_callsign = remote_callsign;
    t.progress.direction = TransferDirection::Upload;
    t.progress.state = TransferState::Negotiating;
    t.progress.total_bytes = compressed.size();
    t.progress.transferred_bytes = 0;
    t.progress.total_chunks = (compressed.size() + FILE_CHUNK_SIZE - 1) / FILE_CHUNK_SIZE;
    t.progress.completed_chunks = 0;
    t.progress.started_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    t.progress.last_activity = t.progress.started_at;

    t.data = std::move(compressed);
    t.local_path = filepath;
    t.crc32 = calculateCRC32(file_data);
    t.retry_count = 0;
    t.current_chunk = 0;
    t.last_send = std::chrono::steady_clock::now();

    const std::string transfer_id = t.progress.transfer_id;

    // Store transfer
    transfers_[transfer_id] = std::move(t);
    auto it = transfers_.find(transfer_id);
    if (it == transfers_.end()) {
        std::cerr << "[FileTransfer] Internal error storing transfer\n";
        return "";
    }

    // Send file offer
    auto offer = buildFileOffer(it->second);
    modem.sendData(offer);

    return transfer_id;
}

void FileTransferManager::acceptTransfer(ModemClient& modem, const std::string& transfer_id) {
    auto it = transfers_.find(transfer_id);
    if (it == transfers_.end()) {
        return;
    }

    it->second.progress.state = TransferState::Transferring;

    auto accept = buildFileAccept(transfer_id);
    modem.sendData(accept);

}

void FileTransferManager::rejectTransfer(ModemClient& modem, const std::string& transfer_id) {
    auto it = transfers_.find(transfer_id);
    if (it == transfers_.end()) {
        return;
    }

    it->second.progress.state = TransferState::Failed;
    it->second.progress.error_message = "Rejected by user";

    auto reject = buildFileReject(transfer_id);
    modem.sendData(reject);

}

void FileTransferManager::cancelTransfer(ModemClient& modem, const std::string& transfer_id) {
    auto it = transfers_.find(transfer_id);
    if (it == transfers_.end()) {
        return;
    }

    it->second.progress.state = TransferState::Cancelled;

    auto cancel = buildFileCancel(transfer_id);
    modem.sendData(cancel);

}

void FileTransferManager::processIncomingData(ModemClient& modem, const std::vector<uint8_t>& data) {
    if (data.empty()) return;

    try {
        FileMessageType type = static_cast<FileMessageType>(data[0]);

        switch (type) {
            case FileMessageType::FileOffer:
                handleFileOffer(modem, data);
                break;
            case FileMessageType::FileAccept:
                handleFileAccept(modem, data);
                break;
            case FileMessageType::FileReject:
                handleFileReject(data);
                break;
            case FileMessageType::FileChunk:
                handleFileChunk(modem, data);
                break;
            case FileMessageType::FileAck:
                handleFileAck(modem, data);
                break;
            case FileMessageType::FileNack:
                handleFileNack(modem, data);
                break;
            case FileMessageType::FileComplete:
                handleFileComplete(data);
                break;
            case FileMessageType::FileCancel:
                handleFileCancel(data);
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
    auto now = std::chrono::steady_clock::now();

    for (auto& [id, t] : transfers_) {
        if (t.progress.state == TransferState::Transferring &&
            t.progress.direction == TransferDirection::Upload) {

            // Check for timeout and retransmit
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - t.last_send);
            if (elapsed.count() > 10) {  // 10 second timeout
                if (t.retry_count < 5) {
                    // Retransmit current chunk
                    sendNextChunk(modem, t);
                    t.retry_count++;
                } else {
                    // Too many retries
                    t.progress.state = TransferState::Failed;
                    t.progress.error_message = "Timeout - no response";
                    notifyProgress(t);
                }
            }
        }
    }
}

std::vector<TransferProgress> FileTransferManager::getActiveTransfers() const {
    std::vector<TransferProgress> result;
    for (const auto& [id, t] : transfers_) {
        if (t.progress.state == TransferState::Negotiating ||
            t.progress.state == TransferState::Transferring ||
            t.progress.state == TransferState::Pending) {
            result.push_back(t.progress);
        }
    }
    return result;
}

std::vector<TransferProgress> FileTransferManager::getCompletedTransfers() const {
    std::vector<TransferProgress> result;
    for (const auto& [id, t] : transfers_) {
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

// Build protocol messages

std::vector<uint8_t> FileTransferManager::buildFileOffer(const Transfer& t) {
    // Format: type(1) + id_len(1) + id + filename_len(1) + filename + size(4) + chunks(2) + crc(4)
    std::vector<uint8_t> msg;
    msg.push_back(static_cast<uint8_t>(FileMessageType::FileOffer));
    msg.push_back(static_cast<uint8_t>(t.progress.transfer_id.size()));
    msg.insert(msg.end(), t.progress.transfer_id.begin(), t.progress.transfer_id.end());
    const std::string name = truncateFilename(t.progress.filename);
    msg.push_back(static_cast<uint8_t>(name.size()));
    msg.insert(msg.end(), name.begin(), name.end());

    uint32_t size = static_cast<uint32_t>(t.progress.total_bytes);
    msg.push_back(size & 0xFF);
    msg.push_back((size >> 8) & 0xFF);
    msg.push_back((size >> 16) & 0xFF);
    msg.push_back((size >> 24) & 0xFF);

    uint16_t chunks = static_cast<uint16_t>(t.progress.total_chunks);
    msg.push_back(chunks & 0xFF);
    msg.push_back((chunks >> 8) & 0xFF);

    msg.push_back(t.crc32 & 0xFF);
    msg.push_back((t.crc32 >> 8) & 0xFF);
    msg.push_back((t.crc32 >> 16) & 0xFF);
    msg.push_back((t.crc32 >> 24) & 0xFF);

    return msg;
}

std::vector<uint8_t> FileTransferManager::buildFileAccept(const std::string& transfer_id) {
    std::vector<uint8_t> msg;
    msg.push_back(static_cast<uint8_t>(FileMessageType::FileAccept));
    msg.push_back(static_cast<uint8_t>(transfer_id.size()));
    msg.insert(msg.end(), transfer_id.begin(), transfer_id.end());
    return msg;
}

std::vector<uint8_t> FileTransferManager::buildFileReject(const std::string& transfer_id) {
    std::vector<uint8_t> msg;
    msg.push_back(static_cast<uint8_t>(FileMessageType::FileReject));
    msg.push_back(static_cast<uint8_t>(transfer_id.size()));
    msg.insert(msg.end(), transfer_id.begin(), transfer_id.end());
    return msg;
}

std::vector<uint8_t> FileTransferManager::buildFileChunk(const Transfer& t, int chunk_index) {
    // Format: type(1) + id_len(1) + id + chunk_index(2) + data_len(2) + data_crc32(4) + data
    if (chunk_index < 0) {
        return {};
    }

    std::vector<uint8_t> msg;
    msg.push_back(static_cast<uint8_t>(FileMessageType::FileChunk));
    msg.push_back(static_cast<uint8_t>(t.progress.transfer_id.size()));
    msg.insert(msg.end(), t.progress.transfer_id.begin(), t.progress.transfer_id.end());

    msg.push_back(chunk_index & 0xFF);
    msg.push_back((chunk_index >> 8) & 0xFF);

    size_t offset = static_cast<size_t>(chunk_index) * FILE_CHUNK_SIZE;
    if (offset > t.data.size()) {
        return {};
    }
    size_t len = std::min(FILE_CHUNK_SIZE, t.data.size() - offset);
    std::vector<uint8_t> chunk_data(t.data.begin() + offset, t.data.begin() + offset + len);
    uint32_t chunk_crc = calculateCRC32(chunk_data);

    msg.push_back(len & 0xFF);
    msg.push_back((len >> 8) & 0xFF);
    msg.push_back(chunk_crc & 0xFF);
    msg.push_back((chunk_crc >> 8) & 0xFF);
    msg.push_back((chunk_crc >> 16) & 0xFF);
    msg.push_back((chunk_crc >> 24) & 0xFF);

    msg.insert(msg.end(), chunk_data.begin(), chunk_data.end());

    return msg;
}

std::vector<uint8_t> FileTransferManager::buildFileAck(const std::string& transfer_id, int chunk_index) {
    std::vector<uint8_t> msg;
    msg.push_back(static_cast<uint8_t>(FileMessageType::FileAck));
    msg.push_back(static_cast<uint8_t>(transfer_id.size()));
    msg.insert(msg.end(), transfer_id.begin(), transfer_id.end());
    msg.push_back(chunk_index & 0xFF);
    msg.push_back((chunk_index >> 8) & 0xFF);
    return msg;
}

std::vector<uint8_t> FileTransferManager::buildFileNack(const std::string& transfer_id, int chunk_index) {
    std::vector<uint8_t> msg;
    msg.push_back(static_cast<uint8_t>(FileMessageType::FileNack));
    msg.push_back(static_cast<uint8_t>(transfer_id.size()));
    msg.insert(msg.end(), transfer_id.begin(), transfer_id.end());
    msg.push_back(chunk_index & 0xFF);
    msg.push_back((chunk_index >> 8) & 0xFF);
    return msg;
}

std::vector<uint8_t> FileTransferManager::buildFileComplete(const std::string& transfer_id, uint32_t crc) {
    std::vector<uint8_t> msg;
    msg.push_back(static_cast<uint8_t>(FileMessageType::FileComplete));
    msg.push_back(static_cast<uint8_t>(transfer_id.size()));
    msg.insert(msg.end(), transfer_id.begin(), transfer_id.end());
    msg.push_back(crc & 0xFF);
    msg.push_back((crc >> 8) & 0xFF);
    msg.push_back((crc >> 16) & 0xFF);
    msg.push_back((crc >> 24) & 0xFF);
    return msg;
}

std::vector<uint8_t> FileTransferManager::buildFileCancel(const std::string& transfer_id) {
    std::vector<uint8_t> msg;
    msg.push_back(static_cast<uint8_t>(FileMessageType::FileCancel));
    msg.push_back(static_cast<uint8_t>(transfer_id.size()));
    msg.insert(msg.end(), transfer_id.begin(), transfer_id.end());
    return msg;
}

// Handle incoming messages

void FileTransferManager::handleFileOffer(ModemClient& modem, const std::vector<uint8_t>& data) {
    if (data.size() < 3) return;

    size_t pos = 1;
    uint8_t id_len = data[pos++];
    if (id_len == 0 || pos + id_len > data.size()) return;

    std::string transfer_id(data.begin() + pos, data.begin() + pos + id_len);
    pos += id_len;

    if (pos >= data.size()) return;
    uint8_t name_len = data[pos++];
    if (name_len == 0 || pos + name_len > data.size()) return;

    std::string raw_name(data.begin() + pos, data.begin() + pos + name_len);
    pos += name_len;

    // Sanitize filename: strip path components to prevent directory traversal
    std::string filename = std::filesystem::path(raw_name).filename().string();
    if (filename.empty() || filename == "." || filename == "..") {
        modem.sendData(buildFileReject(transfer_id));
        return;
    }

    if (pos + 10 > data.size()) return;

    uint32_t size = static_cast<uint32_t>(data[pos])
                  | (static_cast<uint32_t>(data[pos+1]) << 8)
                  | (static_cast<uint32_t>(data[pos+2]) << 16)
                  | (static_cast<uint32_t>(data[pos+3]) << 24);
    pos += 4;

    uint16_t chunks = data[pos] | (data[pos+1] << 8);
    pos += 2;

    uint32_t crc = static_cast<uint32_t>(data[pos])
                 | (static_cast<uint32_t>(data[pos+1]) << 8)
                 | (static_cast<uint32_t>(data[pos+2]) << 16)
                 | (static_cast<uint32_t>(data[pos+3]) << 24);
    pos += 4;

    const size_t max_chunks = (MAX_FILE_SIZE + FILE_CHUNK_SIZE - 1) / FILE_CHUNK_SIZE;
    if (size == 0 || size > MAX_FILE_SIZE || chunks == 0 || chunks > max_chunks) {
        modem.sendData(buildFileReject(transfer_id));
        return;
    }
    const size_t expected_chunks = (size + FILE_CHUNK_SIZE - 1) / FILE_CHUNK_SIZE;
    if (chunks != expected_chunks) {
        modem.sendData(buildFileReject(transfer_id));
        return;
    }

    // Get remote callsign from modem status
    std::string remote = modem.getStatus().remote_callsign;

    // Create download transfer
    Transfer t;
    t.progress.transfer_id = transfer_id;
    t.progress.filename = filename;
    t.progress.remote_callsign = remote;
    t.progress.direction = TransferDirection::Download;
    t.progress.state = TransferState::Negotiating;
    t.progress.total_bytes = size;
    t.progress.transferred_bytes = 0;
    t.progress.total_chunks = chunks;
    t.progress.completed_chunks = 0;
    t.progress.started_at = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    t.progress.last_activity = t.progress.started_at;

    t.data.resize(size);
    t.chunk_received.resize(chunks, false);
    t.crc32 = crc;
    t.retry_count = 0;
    t.current_chunk = 0;

    transfers_[transfer_id] = std::move(t);

    // Auto-accept or ask user
    bool accept = auto_accept_ || isTrusted(remote);
    const bool trusted = isTrusted(remote);
    const bool accepted_by_policy = accept;

    if (offer_callback_ && !accept) {
        accept = offer_callback_(remote, filename, size);
    }

    if (accept) {
        acceptTransfer(modem, transfer_id);
    } else {
        rejectTransfer(modem, transfer_id);
    }
}

void FileTransferManager::handleFileAccept(ModemClient& modem, const std::vector<uint8_t>& data) {
    if (data.size() < 3) return;

    size_t pos = 1;
    uint8_t id_len = data[pos++];
    if (id_len == 0 || pos + id_len > data.size()) return;

    std::string transfer_id(data.begin() + pos, data.begin() + pos + id_len);

    auto it = transfers_.find(transfer_id);
    if (it == transfers_.end()) return;

    it->second.progress.state = TransferState::Transferring;
    it->second.current_chunk = 0;

    // Start sending chunks
    sendNextChunk(modem, it->second);
}

void FileTransferManager::handleFileReject(const std::vector<uint8_t>& data) {
    if (data.size() < 3) return;

    size_t pos = 1;
    uint8_t id_len = data[pos++];
    if (id_len == 0 || pos + id_len > data.size()) return;

    std::string transfer_id(data.begin() + pos, data.begin() + pos + id_len);

    auto it = transfers_.find(transfer_id);
    if (it == transfers_.end()) return;

    it->second.progress.state = TransferState::Failed;
    it->second.progress.error_message = "Rejected by remote";
    notifyProgress(it->second);

}

void FileTransferManager::handleFileChunk(ModemClient& modem, const std::vector<uint8_t>& data) {
    if (data.size() < 11) return;

    size_t pos = 1;
    uint8_t id_len = data[pos++];
    if (id_len == 0 || pos + id_len > data.size()) return;

    std::string transfer_id(data.begin() + pos, data.begin() + pos + id_len);
    pos += id_len;

    auto it = transfers_.find(transfer_id);
    if (it == transfers_.end()) return;

    // Only accept chunks when actively transferring
    if (it->second.progress.state != TransferState::Transferring) return;
    if (pos + 4 > data.size()) return;

    uint16_t chunk_index = data[pos] | (data[pos+1] << 8);
    pos += 2;

    uint16_t chunk_len = data[pos] | (data[pos+1] << 8);
    pos += 2;
    if (pos + 4 > data.size()) {
        modem.sendData(buildFileNack(transfer_id, chunk_index));
        return;
    }
    uint32_t expected_chunk_crc =
        static_cast<uint32_t>(data[pos])
        | (static_cast<uint32_t>(data[pos+1]) << 8)
        | (static_cast<uint32_t>(data[pos+2]) << 16)
        | (static_cast<uint32_t>(data[pos+3]) << 24);
    pos += 4;

    if (chunk_index >= it->second.chunk_received.size()) {
        modem.sendData(buildFileNack(transfer_id, chunk_index));
        return;
    }
    if (chunk_len > FILE_CHUNK_SIZE) {
        modem.sendData(buildFileNack(transfer_id, chunk_index));
        return;
    }

    if (pos + chunk_len > data.size()) {
        // Send NACK
        modem.sendData(buildFileNack(transfer_id, chunk_index));
        return;
    }

    std::vector<uint8_t> chunk_payload(data.begin() + pos, data.begin() + pos + chunk_len);
    uint32_t actual_chunk_crc = calculateCRC32(chunk_payload);
    if (actual_chunk_crc != expected_chunk_crc) {
        modem.sendData(buildFileNack(transfer_id, chunk_index));
        return;
    }

    size_t offset = chunk_index * FILE_CHUNK_SIZE;
    if (offset > it->second.progress.total_bytes) {
        modem.sendData(buildFileNack(transfer_id, chunk_index));
        return;
    }

    size_t expected_len = std::min(
        static_cast<size_t>(FILE_CHUNK_SIZE),
        it->second.progress.total_bytes - offset);
    if (chunk_len != expected_len) {
        modem.sendData(buildFileNack(transfer_id, chunk_index));
        return;
    }

    // Duplicate chunk: ACK again, but do not overwrite previously accepted data.
    if (it->second.chunk_received[chunk_index]) {
        modem.sendData(buildFileAck(transfer_id, chunk_index));
        return;
    }

    if (offset + chunk_len > it->second.data.size()) {
        modem.sendData(buildFileNack(transfer_id, chunk_index));
        return;
    }

    // Copy chunk data
    std::memcpy(it->second.data.data() + offset, chunk_payload.data(), chunk_len);

    it->second.chunk_received[chunk_index] = true;
    it->second.progress.completed_chunks++;
    it->second.progress.transferred_bytes = std::min(
        it->second.progress.total_bytes,
        it->second.progress.transferred_bytes + static_cast<size_t>(chunk_len));
    it->second.progress.last_activity = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Send ACK
    modem.sendData(buildFileAck(transfer_id, chunk_index));

    notifyProgress(it->second);

    // Check if complete
    if (it->second.progress.completed_chunks >= it->second.progress.total_chunks) {
        completeDownload(it->second);
    }
}

void FileTransferManager::handleFileAck(ModemClient& modem, const std::vector<uint8_t>& data) {
    if (data.size() < 5) return;

    size_t pos = 1;
    uint8_t id_len = data[pos++];
    if (id_len == 0 || pos + id_len > data.size()) return;

    std::string transfer_id(data.begin() + pos, data.begin() + pos + id_len);
    pos += id_len;

    auto it = transfers_.find(transfer_id);
    if (it == transfers_.end()) return;

    // Only process ACKs for active uploads
    if (it->second.progress.state != TransferState::Transferring) return;

    if (pos + 2 > data.size()) return;

    uint16_t chunk_index = data[pos] | (data[pos+1] << 8);
    if (chunk_index >= it->second.progress.total_chunks) return;

    if (static_cast<int>(chunk_index) == it->second.current_chunk) {
        it->second.progress.completed_chunks++;
        it->second.progress.transferred_bytes =
            std::min(it->second.data.size(),
                     static_cast<size_t>((chunk_index + 1) * FILE_CHUNK_SIZE));
        it->second.current_chunk++;
        it->second.retry_count = 0;
        it->second.progress.last_activity = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        notifyProgress(it->second);

        // Send next chunk or complete
        if (it->second.current_chunk < static_cast<int>(it->second.progress.total_chunks)) {
            sendNextChunk(modem, it->second);
        } else {
            // All chunks sent, send complete
            modem.sendData(buildFileComplete(transfer_id, it->second.crc32));
            it->second.progress.state = TransferState::Completed;
            notifyProgress(it->second);

            if (complete_callback_) {
                complete_callback_(transfer_id, true, it->second.local_path);
            }
        }
    }
}

void FileTransferManager::handleFileNack(ModemClient& modem, const std::vector<uint8_t>& data) {
    if (data.size() < 5) return;

    size_t pos = 1;
    uint8_t id_len = data[pos++];
    if (id_len == 0 || pos + id_len > data.size()) return;

    std::string transfer_id(data.begin() + pos, data.begin() + pos + id_len);
    pos += id_len;

    auto it = transfers_.find(transfer_id);
    if (it == transfers_.end()) return;
    if (pos + 2 > data.size()) return;

    uint16_t chunk_index = data[pos] | (data[pos+1] << 8);
    if (chunk_index >= it->second.progress.total_chunks) return;

    // Resend requested chunk
    it->second.current_chunk = chunk_index;
    sendNextChunk(modem, it->second);

}

void FileTransferManager::handleFileComplete(const std::vector<uint8_t>& data) {
    if (data.size() < 7) return;

    size_t pos = 1;
    uint8_t id_len = data[pos++];
    if (id_len == 0 || pos + id_len > data.size()) return;

    std::string transfer_id(data.begin() + pos, data.begin() + pos + id_len);
    pos += id_len;

    auto it = transfers_.find(transfer_id);
    if (it == transfers_.end()) return;
    if (pos + 4 > data.size()) return;

    if (it->second.progress.state == TransferState::Completed) {
        return;
    }

    completeDownload(it->second);
}

void FileTransferManager::handleFileCancel(const std::vector<uint8_t>& data) {
    if (data.size() < 3) return;

    size_t pos = 1;
    uint8_t id_len = data[pos++];
    if (id_len == 0 || pos + id_len > data.size()) return;

    std::string transfer_id(data.begin() + pos, data.begin() + pos + id_len);

    auto it = transfers_.find(transfer_id);
    if (it == transfers_.end()) return;

    it->second.progress.state = TransferState::Cancelled;
    it->second.progress.error_message = "Cancelled by remote";
    notifyProgress(it->second);

}

void FileTransferManager::sendNextChunk(ModemClient& modem, Transfer& t) {
    if (t.current_chunk < 0 || t.current_chunk >= static_cast<int>(t.progress.total_chunks)) {
        return;
    }

    auto chunk = buildFileChunk(t, t.current_chunk);
    if (chunk.empty()) {
        t.progress.state = TransferState::Failed;
        t.progress.error_message = "Invalid chunk index";
        notifyProgress(t);
        return;
    }
    modem.sendData(chunk);
    t.last_send = std::chrono::steady_clock::now();
}

void FileTransferManager::completeDownload(Transfer& t) {
    if (t.progress.state == TransferState::Completed) {
        return;
    }

    if (t.progress.completed_chunks < t.progress.total_chunks) {
        return;
    }

    // Decompress and verify against CRC from file offer.
    LZHCodec lzh;
    std::vector<uint8_t> decompressed = lzh.decompress(t.data);
    uint32_t actual_crc = calculateCRC32(decompressed);

    if (actual_crc != t.crc32) {
        t.progress.state = TransferState::Failed;
        t.progress.error_message = "CRC mismatch";
        notifyProgress(t);
        std::cerr << "[FileTransfer] FAIL id=" << shortTransferId(t.progress.transfer_id)
                  << " reason=crc_mismatch expected=0x" << std::hex << t.crc32
                  << " actual=0x" << actual_crc << std::dec << "\n";
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
    if (!file) {
        t.progress.state = TransferState::Failed;
        t.progress.error_message = "Cannot write file";
        notifyProgress(t);
        std::cerr << "[FileTransfer] FAIL id=" << shortTransferId(t.progress.transfer_id)
                  << " reason=cannot_write_file path=" << out.string() << "\n";
        return;
    }

    file.write(reinterpret_cast<const char*>(decompressed.data()), decompressed.size());
    file.close();

    t.progress.state = TransferState::Completed;
    t.local_path = out.string();
    notifyProgress(t);

    if (complete_callback_) {
        complete_callback_(t.progress.transfer_id, true, t.local_path);
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
