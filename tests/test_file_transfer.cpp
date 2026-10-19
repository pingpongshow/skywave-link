// FileTransferManager: filename helpers (wire-length truncation and
// non-overwriting download paths), deferred offer decisions, and the
// streaming transfer itself (flow control, end-to-end check, link loss,
// timeouts) between two managers on fake modems over loopback.

#include "test_macros.hpp"
#include "skywave_link/file_transfer.hpp"
#include "skywave_link/lzh.hpp"
#include "skywave_link/modem_client.hpp"

#include <filesystem>
#include <fstream>
#include <random>

using namespace skylink;

TEST(truncate_short_name_unchanged) {
    ASSERT_EQ(FileTransferManager::truncateFilename("photo.jpg"), std::string("photo.jpg"));
    PASS();
}

TEST(truncate_long_name_keeps_extension) {
    std::string name(300, 'a');
    name += ".jpg";
    std::string t = FileTransferManager::truncateFilename(name);
    ASSERT_EQ(t.size(), size_t(255));
    ASSERT_EQ(t.substr(t.size() - 4), std::string(".jpg"));
    PASS();
}

TEST(truncate_respects_utf8_boundary) {
    std::string name;
    for (int i = 0; i < 200; ++i) name += "\xC3\xA9";  // 'e-acute', 2 bytes each
    std::string t = FileTransferManager::truncateFilename(name, 255);
    ASSERT_LE(t.size(), size_t(255));
    ASSERT_EQ(t.size() % 2, size_t(0));  // no split sequence
    PASS();
}

TEST(unique_download_path_never_overwrites) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() /
                   ("skylink_ft_test_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);

    std::string p0 = FileTransferManager::uniqueDownloadPath(dir.string(), "report.txt");
    ASSERT_EQ(fs::path(p0).filename().string(), std::string("report.txt"));
    std::ofstream(p0) << "x";

    std::string p1 = FileTransferManager::uniqueDownloadPath(dir.string(), "report.txt");
    ASSERT_EQ(fs::path(p1).filename().string(), std::string("report (1).txt"));
    std::ofstream(p1) << "y";

    std::string p2 = FileTransferManager::uniqueDownloadPath(dir.string(), "report.txt");
    ASSERT_EQ(fs::path(p2).filename().string(), std::string("report (2).txt"));

    // A max-length name still yields a valid (<= 255 byte) unique name.
    std::string longname = FileTransferManager::truncateFilename(std::string(300, 'b') + ".bin");
    std::ofstream(dir / longname) << "z";
    std::string pl = FileTransferManager::uniqueDownloadPath(dir.string(), longname);
    ASSERT_LE(fs::path(pl).filename().string().size(), size_t(255));
    ASSERT_FALSE(fs::exists(pl));
    ASSERT_CONTAINS(fs::path(pl).filename().string(), " (1).bin");

    fs::remove_all(dir);
    PASS();
}

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

namespace {

int makeListener(uint16_t* port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    *port = ntohs(addr.sin_port);
    return fd;
}

void sendAll(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = send(fd, s.data() + off, s.size() - off, 0);
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
}

// Fake modem: answers control commands by echoing the verb (BUFFER answers
// with the configured backlog), and collects the session frames the client
// writes to the data port.
struct FakeModem {
    int lcmd = -1, ldata = -1, lvoice = -1;
    uint16_t pcmd = 0, pdata = 0, pvoice = 0;
    int cmd = -1, data = -1, voice = -1;
    std::thread responder;
    std::atomic<bool> stop{false};
    std::atomic<size_t> backlog{0};
    std::atomic<int> buffer_queries{0};
    std::string rx;          // raw bytes read from the data port
    size_t delivered = 0;    // frames already handed out by newFrames()

    bool listen() {
        lcmd = makeListener(&pcmd);
        ldata = makeListener(&pdata);
        lvoice = makeListener(&pvoice);
        return lcmd >= 0 && ldata >= 0 && lvoice >= 0;
    }
    void acceptAll() {
        cmd = accept(lcmd, nullptr, nullptr);
        data = accept(ldata, nullptr, nullptr);
        voice = accept(lvoice, nullptr, nullptr);
        timeval tv{0, 50000};
        setsockopt(cmd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(data, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        responder = std::thread([this]() {
            std::string buf;
            char ch;
            while (!stop) {
                ssize_t n = recv(cmd, &ch, 1, 0);
                if (n == 0) break;
                if (n < 0) continue;
                if (ch != '\n') { buf += ch; continue; }
                std::string verb = buf.substr(0, buf.find(' '));
                buf.clear();
                if (verb == "BUFFER") {
                    ++buffer_queries;
                    sendAll(cmd, "BUFFER " + std::to_string(backlog.load()) + "\r");
                } else {
                    sendAll(cmd, verb + "\r");
                }
            }
        });
    }
    void event(const std::string& line) { sendAll(cmd, line + "\r"); }
    // Session payloads received so far (framing stripped), oldest first.
    std::vector<std::vector<uint8_t>> frames() {
        char buf[65536];
        ssize_t n;
        while ((n = recv(data, buf, sizeof(buf), MSG_DONTWAIT)) > 0) rx.append(buf, static_cast<size_t>(n));
        std::vector<std::vector<uint8_t>> out;
        size_t pos = 0;
        while (pos + 4 <= rx.size()) {
            size_t len = (static_cast<uint8_t>(rx[pos + 2]) << 8) | static_cast<uint8_t>(rx[pos + 3]);
            if (pos + 4 + len > rx.size()) break;
            out.emplace_back(rx.begin() + pos + 4, rx.begin() + pos + 4 + len);
            pos += 4 + len;
        }
        return out;
    }
    // Frames not returned by an earlier newFrames() call.
    std::vector<std::vector<uint8_t>> newFrames() {
        auto all = frames();
        std::vector<std::vector<uint8_t>> out(all.begin() + static_cast<std::ptrdiff_t>(delivered), all.end());
        delivered = all.size();
        return out;
    }
    ~FakeModem() {
        stop = true;
        if (responder.joinable()) responder.join();
        for (int fd : {cmd, data, voice, lcmd, ldata, lvoice}) if (fd >= 0) close(fd);
    }
};

void pump(ModemClient& c, int ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        c.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

uint32_t crc32(const std::vector<uint8_t>& d) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint8_t b : d) {
        c ^= b;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

void putLe(std::vector<uint8_t>& v, uint32_t x, int bytes) {
    for (int i = 0; i < bytes; ++i) v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
}

std::vector<uint8_t> idMsg(FileMessageType type, const std::string& id) {
    std::vector<uint8_t> m{static_cast<uint8_t>(type), static_cast<uint8_t>(id.size())};
    m.insert(m.end(), id.begin(), id.end());
    return m;
}

// A file as the sender would offer it (LZH payload, CRC of the original).
struct WireFile {
    std::vector<uint8_t> original;
    std::vector<uint8_t> payload;
};

WireFile makeFile() {
    WireFile f;
    const std::string text = "deferred offer test payload";
    f.original.assign(text.begin(), text.end());
    LZHCodec lzh;
    f.payload = lzh.compress(f.original);
    return f;
}

std::vector<uint8_t> offerMsg(const std::string& id, const std::string& name, const WireFile& f) {
    std::vector<uint8_t> m = idMsg(FileMessageType::FileOffer, id);
    m.push_back(static_cast<uint8_t>(name.size()));
    m.insert(m.end(), name.begin(), name.end());
    putLe(m, static_cast<uint32_t>(f.payload.size()), 4);
    putLe(m, crc32(f.original), 4);
    return m;
}

std::vector<uint8_t> chunkMsg(const std::string& id, const WireFile& f, size_t offset = 0,
                              size_t len = std::string::npos) {
    std::vector<uint8_t> m = idMsg(FileMessageType::FileChunk, id);
    putLe(m, static_cast<uint32_t>(offset), 4);
    len = std::min(len, f.payload.size() - offset);
    m.insert(m.end(), f.payload.begin() + static_cast<std::ptrdiff_t>(offset),
             f.payload.begin() + static_cast<std::ptrdiff_t>(offset + len));
    return m;
}

std::vector<uint8_t> completeMsg(const std::string& id, const WireFile& f) {
    std::vector<uint8_t> m = idMsg(FileMessageType::FileComplete, id);
    putLe(m, static_cast<uint32_t>(f.payload.size()), 4);
    putLe(m, crc32(f.payload), 4);
    return m;
}

std::vector<uint8_t> ackMsg(const std::string& id, bool ok) {
    std::vector<uint8_t> m = idMsg(FileMessageType::FileAck, id);
    m.push_back(ok ? 0 : 1);
    return m;
}

bool sawFrame(FakeModem& m, const std::vector<uint8_t>& want) {
    for (const auto& f : m.frames()) {
        if (f == want) return true;
    }
    return false;
}

bool sawType(FakeModem& m, FileMessageType type) {
    for (const auto& f : m.frames()) {
        if (!f.empty() && f[0] == static_cast<uint8_t>(type)) return true;
    }
    return false;
}

size_t countType(FakeModem& m, FileMessageType type) {
    size_t n = 0;
    for (const auto& f : m.frames()) {
        if (!f.empty() && f[0] == static_cast<uint8_t>(type)) ++n;
    }
    return n;
}

// Connected client + manager whose offer callback always defers.
struct DeferFixture {
    FakeModem m;
    ModemClient c;
    FileTransferManager ftm;
    std::vector<std::string> offered_ids;
    bool ok = false;

    DeferFixture() {
        if (!m.listen()) return;
        if (!c.connect("127.0.0.1", m.pcmd, m.pdata, m.pvoice)) return;
        m.acceptAll();
        m.event("EVT:CONNECTED K1BBB WIDE M8");
        pump(c, 100);
        ftm.onFileOffer([this](const std::string& id, const std::string&, const std::string&, size_t) {
            offered_ids.push_back(id);
            return FileOfferDecision::Defer;
        });
        ok = true;
    }
};

// Two stations, each a manager on its own client and fake modem; relay()
// carries what one writes to its data port into the other's manager, as the
// radio link would.
struct LinkFixture {
    FakeModem ma, mb;
    ModemClient a, b;
    FileTransferManager fa, fb;
    std::filesystem::path dir;
    std::vector<std::pair<std::string, bool>> done_a, done_b;
    bool ok = false;

    LinkFixture() {
        namespace fs = std::filesystem;
        dir = fs::temp_directory_path() / ("skylink_ft_link_" + std::to_string(std::random_device{}()));
        fs::create_directories(dir / "in");
        if (!ma.listen() || !mb.listen()) return;
        a.setNonBlockingCommands(true);
        b.setNonBlockingCommands(true);
        if (!a.connect("127.0.0.1", ma.pcmd, ma.pdata, ma.pvoice)) return;
        ma.acceptAll();
        if (!b.connect("127.0.0.1", mb.pcmd, mb.pdata, mb.pvoice)) return;
        mb.acceptAll();
        ma.event("EVT:CONNECTED K1BBB WIDE M8");
        mb.event("EVT:CONNECTED K1AAA WIDE M8");
        pump(a, 50);
        pump(b, 50);
        fb.setAutoAccept(true);
        fb.setDownloadPath((dir / "in").string());
        fa.onComplete([this](const std::string& id, bool success, const std::string&) {
            done_a.emplace_back(id, success);
        });
        fb.onComplete([this](const std::string& id, bool success, const std::string&) {
            done_b.emplace_back(id, success);
        });
        ok = a.getStatus().link_state == LinkState::Connected &&
             b.getStatus().link_state == LinkState::Connected;
    }
    ~LinkFixture() { std::filesystem::remove_all(dir); }

    // One step of both stations' main loops; returns frames moved A->B.
    size_t step() {
        a.poll();
        b.poll();
        fa.update(a);
        fb.update(b);
        size_t moved = 0;
        for (const auto& f : ma.newFrames()) { fb.processIncomingData(b, f); ++moved; }
        for (const auto& f : mb.newFrames()) fa.processIncomingData(a, f);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return moved;
    }
    void run(int ms) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) step();
    }
    std::string writeFile(const std::string& name, size_t size) {
        std::vector<char> bytes(size);
        std::mt19937 gen(12345);
        for (auto& ch : bytes) ch = static_cast<char>(gen() & 0xFF);  // incompressible
        const std::string path = (dir / name).string();
        std::ofstream(path, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(size));
        return path;
    }
};

std::string readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

TEST(deferred_offer_accept_then_transfer_completes) {
    namespace fs = std::filesystem;
    DeferFixture fx;
    ASSERT_TRUE(fx.ok);
    fs::path dir = fs::temp_directory_path() /
                   ("skylink_ft_defer_" + std::to_string(std::random_device{}()));
    fx.ftm.setDownloadPath(dir.string());
    bool completed = false;
    std::string completed_path;
    fx.ftm.onComplete([&](const std::string&, bool success, const std::string& path) {
        completed = success;
        completed_path = path;
    });

    const WireFile f = makeFile();
    fx.ftm.processIncomingData(fx.c, offerMsg("T1", "note.txt", f));
    pump(fx.c, 100);
    ASSERT_EQ(fx.offered_ids.size(), size_t(1));
    auto pending = fx.ftm.getPendingOffers();
    ASSERT_EQ(pending.size(), size_t(1));
    ASSERT_EQ(pending[0].transfer_id, std::string("T1"));
    ASSERT_EQ(pending[0].filename, std::string("note.txt"));
    ASSERT_EQ(pending[0].from, std::string("K1BBB"));
    ASSERT_EQ(pending[0].size, f.payload.size());
    // Nothing is answered while the operator decides.
    ASSERT_FALSE(sawType(fx.m, FileMessageType::FileAccept));
    ASSERT_FALSE(sawType(fx.m, FileMessageType::FileReject));
    // A repeated offer does not ask again.
    fx.ftm.processIncomingData(fx.c, offerMsg("T1", "note.txt", f));
    ASSERT_EQ(fx.offered_ids.size(), size_t(1));

    ASSERT_TRUE(fx.ftm.acceptPendingOffer(fx.c, "T1"));
    ASSERT_FALSE(fx.ftm.acceptPendingOffer(fx.c, "T1"));  // no longer pending
    pump(fx.c, 100);
    ASSERT_TRUE(sawFrame(fx.m, idMsg(FileMessageType::FileAccept, "T1")));
    ASSERT_TRUE(fx.ftm.getPendingOffers().empty());
    ASSERT_TRUE(fx.ftm.getTransfer("T1").state == TransferState::Transferring);

    // Data is not acknowledged chunk by chunk; the verdict follows FileComplete.
    fx.ftm.processIncomingData(fx.c, chunkMsg("T1", f));
    pump(fx.c, 100);
    ASSERT_FALSE(sawType(fx.m, FileMessageType::FileAck));
    ASSERT_FALSE(completed);
    fx.ftm.processIncomingData(fx.c, completeMsg("T1", f));
    pump(fx.c, 100);
    ASSERT_TRUE(sawFrame(fx.m, ackMsg("T1", true)));
    ASSERT_TRUE(completed);
    ASSERT_EQ(readFile(completed_path), std::string(f.original.begin(), f.original.end()));
    fs::remove_all(dir);
    PASS();
}

TEST(deferred_offer_decline_sends_reject) {
    DeferFixture fx;
    ASSERT_TRUE(fx.ok);
    const WireFile f = makeFile();
    fx.ftm.processIncomingData(fx.c, offerMsg("T2", "a.bin", f));
    ASSERT_EQ(fx.ftm.getPendingOffers().size(), size_t(1));
    ASSERT_TRUE(fx.ftm.rejectPendingOffer(fx.c, "T2"));
    pump(fx.c, 100);
    ASSERT_TRUE(sawFrame(fx.m, idMsg(FileMessageType::FileReject, "T2")));
    ASSERT_FALSE(sawType(fx.m, FileMessageType::FileAccept));
    ASSERT_TRUE(fx.ftm.getPendingOffers().empty());
    ASSERT_TRUE(fx.ftm.getTransfer("T2").state == TransferState::Failed);
    // Data for a declined offer is ignored.
    fx.ftm.processIncomingData(fx.c, chunkMsg("T2", f));
    fx.ftm.processIncomingData(fx.c, completeMsg("T2", f));
    pump(fx.c, 100);
    ASSERT_FALSE(sawType(fx.m, FileMessageType::FileAck));
    PASS();
}

TEST(deferred_offer_times_out_with_reject) {
    DeferFixture fx;
    ASSERT_TRUE(fx.ok);
    const WireFile f = makeFile();
    fx.ftm.processIncomingData(fx.c, offerMsg("T3", "a.bin", f));
    fx.ftm.update(fx.c);  // default timeout: still pending
    ASSERT_EQ(fx.ftm.getPendingOffers().size(), size_t(1));
    fx.ftm.setPendingOfferTimeout(std::chrono::seconds(0));
    fx.ftm.update(fx.c);
    pump(fx.c, 100);
    ASSERT_TRUE(fx.ftm.getPendingOffers().empty());
    ASSERT_TRUE(sawFrame(fx.m, idMsg(FileMessageType::FileReject, "T3")));
    auto t = fx.ftm.getTransfer("T3");
    ASSERT_TRUE(t.state == TransferState::Failed);
    ASSERT_EQ(t.error_message, std::string("Offer timed out"));
    ASSERT_FALSE(fx.ftm.acceptPendingOffer(fx.c, "T3"));
    PASS();
}

TEST(deferred_offer_dropped_at_session_end) {
    DeferFixture fx;
    ASSERT_TRUE(fx.ok);
    const WireFile f = makeFile();
    fx.ftm.processIncomingData(fx.c, offerMsg("T4", "a.bin", f));
    fx.ftm.processIncomingData(fx.c, offerMsg("T5", "b.bin", f));
    ASSERT_EQ(fx.ftm.getPendingOffers().size(), size_t(2));
    fx.ftm.dropPendingOffers();
    pump(fx.c, 100);
    ASSERT_TRUE(fx.ftm.getPendingOffers().empty());
    ASSERT_FALSE(sawType(fx.m, FileMessageType::FileReject));  // session is gone: no reply
    ASSERT_TRUE(fx.ftm.getTransfer("T4").state == TransferState::Failed);
    ASSERT_FALSE(fx.ftm.acceptPendingOffer(fx.c, "T5"));
    pump(fx.c, 100);
    ASSERT_FALSE(sawType(fx.m, FileMessageType::FileAccept));
    PASS();
}

TEST(deferred_offers_are_bounded_and_size_checked) {
    DeferFixture fx;
    ASSERT_TRUE(fx.ok);
    const WireFile f = makeFile();
    for (size_t i = 0; i < MAX_PENDING_OFFERS + 1; ++i) {
        fx.ftm.processIncomingData(fx.c, offerMsg("P" + std::to_string(i), "x.bin", f));
    }
    ASSERT_EQ(fx.ftm.getPendingOffers().size(), MAX_PENDING_OFFERS);
    pump(fx.c, 100);
    ASSERT_TRUE(sawFrame(fx.m, idMsg(FileMessageType::FileReject,
                                     "P" + std::to_string(MAX_PENDING_OFFERS))));

    // Oversized offers are refused before the app is asked.
    const size_t asked = fx.offered_ids.size();
    std::vector<uint8_t> big = idMsg(FileMessageType::FileOffer, "BIG");
    big.push_back(5);
    for (char ch : std::string("x.bin")) big.push_back(static_cast<uint8_t>(ch));
    putLe(big, static_cast<uint32_t>(MAX_FILE_SIZE * 2), 4);
    putLe(big, 0, 4);
    fx.ftm.processIncomingData(fx.c, big);
    ASSERT_EQ(fx.offered_ids.size(), asked);
    pump(fx.c, 100);
    ASSERT_TRUE(sawFrame(fx.m, idMsg(FileMessageType::FileReject, "BIG")));
    PASS();
}

// Trusted callsigns used to be consulted but never set (not in the config,
// not in Settings). A trusted peer's offer is accepted with no prompt;
// matching ignores case and surrounding space; anyone else still asks.
TEST(trusted_callsign_offer_is_accepted_without_prompt) {
    DeferFixture fx;
    ASSERT_TRUE(fx.ok);
    const WireFile f = makeFile();
    fx.ftm.setTrustedCallsigns({" k1bbb "});
    fx.ftm.processIncomingData(fx.c, offerMsg("T7", "t.txt", f));
    pump(fx.c, 100);
    ASSERT_TRUE(fx.offered_ids.empty());
    ASSERT_TRUE(fx.ftm.getPendingOffers().empty());
    ASSERT_TRUE(sawFrame(fx.m, idMsg(FileMessageType::FileAccept, "T7")));

    fx.ftm.setTrustedCallsigns({"K1CCC"});  // replaces the list
    fx.ftm.processIncomingData(fx.c, offerMsg("T8", "u.txt", f));
    pump(fx.c, 100);
    ASSERT_EQ(fx.offered_ids.size(), size_t(1));
    ASSERT_EQ(fx.ftm.getPendingOffers().size(), size_t(1));
    ASSERT_FALSE(sawFrame(fx.m, idMsg(FileMessageType::FileAccept, "T8")));
    PASS();
}

// The whole file streams in multi-KB chunks with no per-chunk
// acknowledgement; the receiver checks size and CRC and sends one verdict.
TEST(multi_chunk_transfer_streams_and_completes) {
    LinkFixture fx;
    ASSERT_TRUE(fx.ok);
    const std::string src = fx.writeFile("big.bin", 20000);
    const std::string id = fx.fa.sendFile(fx.a, "K1BBB", src);
    ASSERT_FALSE(id.empty());
    for (int i = 0; i < 2000 && fx.done_a.empty(); ++i) fx.step();

    ASSERT_EQ(fx.done_a.size(), size_t(1));
    ASSERT_TRUE(fx.done_a[0].second);
    ASSERT_EQ(fx.done_b.size(), size_t(1));
    ASSERT_TRUE(fx.done_b[0].second);
    const auto up = fx.fa.getTransfer(id);
    ASSERT_TRUE(up.state == TransferState::Completed);
    ASSERT_EQ(up.transferred_bytes, up.total_bytes);
    const auto down = fx.fb.getTransfer(id);
    ASSERT_TRUE(down.state == TransferState::Completed);
    ASSERT_EQ(readFile((fx.dir / "in" / "big.bin").string()), readFile(src));

    // Many chunks, each carried whole, and exactly one verdict back.
    const size_t chunks = countType(fx.ma, FileMessageType::FileChunk);
    ASSERT_GE(chunks, size_t(8));
    ASSERT_EQ(chunks, (up.total_bytes + FILE_CHUNK_SIZE - 1) / FILE_CHUNK_SIZE);
    ASSERT_EQ(countType(fx.ma, FileMessageType::FileComplete), size_t(1));
    ASSERT_EQ(countType(fx.mb, FileMessageType::FileAck), size_t(1));
    PASS();
}

// Flow control: data goes out only while the modem's backlog (BUFFER) is
// below the target; a stalled modem holds the rest back.
TEST(upload_respects_modem_backlog) {
    LinkFixture fx;
    ASSERT_TRUE(fx.ok);
    fx.ma.backlog = 100000;  // the modem reports a huge queue
    const std::string src = fx.writeFile("held.bin", 20000);
    const std::string id = fx.fa.sendFile(fx.a, "K1BBB", src);
    ASSERT_FALSE(id.empty());
    fx.run(600);
    // Only the first window went out before the BUFFER answer arrived.
    const size_t first = countType(fx.ma, FileMessageType::FileChunk);
    ASSERT_GE(first, size_t(1));
    ASSERT_LE(first * FILE_CHUNK_SIZE, FILE_TX_BACKLOG_TARGET + FILE_CHUNK_SIZE);
    ASSERT_GT(fx.ma.buffer_queries.load(), 0);
    fx.run(400);
    ASSERT_EQ(countType(fx.ma, FileMessageType::FileChunk), first);  // held back
    ASSERT_TRUE(fx.fa.getTransfer(id).state == TransferState::Transferring);

    fx.ma.backlog = 0;  // the modem drained
    for (int i = 0; i < 2000 && fx.done_a.empty(); ++i) fx.step();
    ASSERT_EQ(fx.done_a.size(), size_t(1));
    ASSERT_TRUE(fx.done_a[0].second);
    PASS();
}

// Link loss mid-transfer fails both ends at once, and nothing more is
// written to the data port afterwards (it would sit in the modem's queue).
TEST(link_loss_fails_both_sides) {
    LinkFixture fx;
    ASSERT_TRUE(fx.ok);
    fx.ma.backlog = 100000;  // keep the transfer from finishing
    const std::string src = fx.writeFile("cut.bin", 20000);
    const std::string id = fx.fa.sendFile(fx.a, "K1BBB", src);
    ASSERT_FALSE(id.empty());
    fx.run(300);
    ASSERT_TRUE(fx.fa.getTransfer(id).state == TransferState::Transferring);
    ASSERT_TRUE(fx.fb.getTransfer(id).state == TransferState::Transferring);

    fx.ma.event("EVT:DISCONNECTED LINK");
    fx.mb.event("EVT:DISCONNECTED LINK");
    pump(fx.a, 100);
    pump(fx.b, 100);
    const size_t before = fx.ma.frames().size();
    fx.ma.backlog = 0;
    fx.run(500);
    ASSERT_EQ(fx.ma.frames().size(), before);
    const auto up = fx.fa.getTransfer(id);
    const auto down = fx.fb.getTransfer(id);
    ASSERT_TRUE(up.state == TransferState::Failed);
    ASSERT_TRUE(down.state == TransferState::Failed);
    ASSERT_CONTAINS(up.error_message, "Link lost");
    ASSERT_CONTAINS(down.error_message, "Link lost");
    ASSERT_EQ(fx.done_a.size(), size_t(1));
    ASSERT_FALSE(fx.done_a[0].second);
    ASSERT_EQ(fx.done_b.size(), size_t(1));
    ASSERT_FALSE(fx.done_b[0].second);
    // Raw data is refused outright while no session is up.
    ASSERT_FALSE(fx.a.sendData({1, 2, 3}));
    // A new file cannot start either.
    ASSERT_TRUE(fx.fa.sendFile(fx.a, "K1BBB", src).empty());
    PASS();
}

// The explicit hook used by the app on EVT:DISCONNECTED.
TEST(link_lost_marks_transfers_failed) {
    LinkFixture fx;
    ASSERT_TRUE(fx.ok);
    fx.ma.backlog = 100000;
    const std::string id = fx.fa.sendFile(fx.a, "K1BBB", fx.writeFile("x.bin", 9000));
    fx.run(200);
    fx.fa.linkLost("Link lost");
    fx.fb.linkLost("Link lost");
    ASSERT_TRUE(fx.fa.getTransfer(id).state == TransferState::Failed);
    ASSERT_EQ(fx.fb.getTransfer(id).error_message, std::string("Link lost"));
    ASSERT_TRUE(fx.fa.getActiveTransfers().empty());
    PASS();
}

// A gap in the stream cannot be repaired: the receiver fails the transfer
// and tells the sender why.
TEST(receiver_rejects_gap_and_bad_crc) {
    DeferFixture fx;
    ASSERT_TRUE(fx.ok);
    fx.ftm.setAutoAccept(true);
    WireFile f;
    for (int i = 0; i < 5000; ++i) f.original.push_back(static_cast<uint8_t>((i * 7919) & 0xFF));
    f.payload = LZHCodec().compress(f.original);
    ASSERT_GT(f.payload.size(), size_t(200));

    fx.ftm.processIncomingData(fx.c, offerMsg("G1", "g.bin", f));
    fx.ftm.processIncomingData(fx.c, chunkMsg("G1", f, 0, 100));
    fx.ftm.processIncomingData(fx.c, chunkMsg("G1", f, 0, 100));   // replay: ignored
    fx.ftm.processIncomingData(fx.c, chunkMsg("G1", f, 150, 50));  // gap
    pump(fx.c, 100);
    auto t = fx.ftm.getTransfer("G1");
    ASSERT_TRUE(t.state == TransferState::Failed);
    ASSERT_CONTAINS(t.error_message, "missing");
    bool saw_fail_ack = false;
    for (const auto& m : fx.m.frames()) {
        if (m.size() > 4 && m[0] == static_cast<uint8_t>(FileMessageType::FileAck) && m[4] == 1) {
            saw_fail_ack = true;
        }
    }
    ASSERT_TRUE(saw_fail_ack);

    // Corrupted payload: the end-to-end CRC catches it.
    WireFile bad = f;
    bad.payload[10] ^= 0xFF;
    fx.ftm.processIncomingData(fx.c, offerMsg("G2", "h.bin", f));
    fx.ftm.processIncomingData(fx.c, chunkMsg("G2", bad));
    fx.ftm.processIncomingData(fx.c, completeMsg("G2", f));
    ASSERT_TRUE(fx.ftm.getTransfer("G2").state == TransferState::Failed);
    ASSERT_EQ(fx.ftm.getTransfer("G2").error_message, std::string("CRC mismatch"));
    PASS();
}

// No stop-and-wait retransmission: one long inactivity timeout instead.
TEST(inactivity_timeout_fails_quiet_transfer) {
    LinkFixture fx;
    ASSERT_TRUE(fx.ok);
    const std::string id = fx.fa.sendFile(fx.a, "K1BBB", fx.writeFile("q.bin", 3000));
    fx.a.poll();
    fx.fa.update(fx.a);  // offer out, no answer relayed
    ASSERT_TRUE(fx.fa.getTransfer(id).state == TransferState::Negotiating);
    fx.fa.setInactivityTimeout(std::chrono::seconds(0));
    fx.fa.update(fx.a);
    const auto t = fx.fa.getTransfer(id);
    ASSERT_TRUE(t.state == TransferState::Failed);
    ASSERT_CONTAINS(t.error_message, "No answer");
    pump(fx.a, 100);
    ASSERT_TRUE(sawFrame(fx.ma, idMsg(FileMessageType::FileCancel, id)));
    PASS();
}
#endif

TEST_MAIN()
