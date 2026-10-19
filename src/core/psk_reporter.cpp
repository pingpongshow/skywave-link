// PSK Reporter uploader - see include/skywave_link/psk_reporter.hpp and
// https://pskreporter.info/pskdev.html

#include "skywave_link/psk_reporter.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <limits>
#include <random>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <netinet/in.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <errno.h>
#endif

namespace skylink {

namespace {

constexpr uint16_t kIpfixVersion = 10;
constexpr uint16_t kSetIdTemplate = 2;
constexpr uint16_t kSetIdOptionsTemplate = 3;
constexpr uint16_t kVarLen = 0xFFFF;
constexpr size_t kHeaderBytes = 16;
constexpr size_t kMaxStringBytes = 254;  // spec: length code no more than 254

// PSK Reporter information elements (enterprise 30351)
constexpr uint16_t kIeSenderCallsign = 1;
constexpr uint16_t kIeReceiverCallsign = 2;
constexpr uint16_t kIeSenderLocator = 3;
constexpr uint16_t kIeReceiverLocator = 4;
constexpr uint16_t kIeFrequency = 5;
constexpr uint16_t kIeSnr = 6;
constexpr uint16_t kIeDecoderSoftware = 8;
constexpr uint16_t kIeAntennaInformation = 9;
constexpr uint16_t kIeMode = 10;
constexpr uint16_t kIeInformationSource = 11;
// IANA flowStartSeconds
constexpr uint16_t kIeFlowStartSeconds = 150;

constexpr uint8_t kInformationSourceAutomatic = 1;

struct FieldSpec {
    uint16_t id;
    uint16_t length;
    bool enterprise;
};

void put8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
void put16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
}
void put32(std::vector<uint8_t>& b, uint32_t v) {
    put16(b, static_cast<uint16_t>(v >> 16));
    put16(b, static_cast<uint16_t>(v));
}
void set16(std::vector<uint8_t>& b, size_t at, size_t v) {
    b[at] = static_cast<uint8_t>((v >> 8) & 0xFF);
    b[at + 1] = static_cast<uint8_t>(v & 0xFF);
}
void padTo4(std::vector<uint8_t>& b, size_t set_start) {
    while ((b.size() - set_start) % 4 != 0) b.push_back(0);
}
size_t strBytes(const std::string& s) { return 1 + std::min(s.size(), kMaxStringBytes); }
void putStr(std::vector<uint8_t>& b, const std::string& s) {
    size_t n = std::min(s.size(), kMaxStringBytes);
    b.push_back(static_cast<uint8_t>(n));
    b.insert(b.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}
size_t pad4(size_t n) { return (n + 3) & ~size_t(3); }

void putField(std::vector<uint8_t>& b, const FieldSpec& f) {
    if (f.enterprise) {
        put16(b, static_cast<uint16_t>(0x8000 | f.id));
        put16(b, f.length);
        put32(b, PskReporterClient::kEnterpriseNumber);
    } else {
        put16(b, f.id);
        put16(b, f.length);
    }
}

bool hasLocator(const PskReporterSpot& s) { return !s.locator.empty(); }

std::vector<FieldSpec> senderFields(uint16_t tpl) {
    const bool snr = tpl == PskReporterClient::kTplSenderSnr ||
                     tpl == PskReporterClient::kTplSenderSnrLoc;
    const bool loc = tpl == PskReporterClient::kTplSenderLoc ||
                     tpl == PskReporterClient::kTplSenderSnrLoc;
    std::vector<FieldSpec> f;
    f.push_back({kIeSenderCallsign, kVarLen, true});
    f.push_back({kIeFrequency, 4, true});
    if (snr) f.push_back({kIeSnr, 1, true});
    f.push_back({kIeMode, kVarLen, true});
    f.push_back({kIeInformationSource, 1, true});
    if (loc) f.push_back({kIeSenderLocator, kVarLen, true});
    f.push_back({kIeFlowStartSeconds, 4, false});
    return f;
}

std::vector<FieldSpec> receiverFields(bool antenna) {
    std::vector<FieldSpec> f{{kIeReceiverCallsign, kVarLen, true},
                             {kIeReceiverLocator, kVarLen, true},
                             {kIeDecoderSoftware, kVarLen, true}};
    if (antenna) f.push_back({kIeAntennaInformation, kVarLen, true});
    return f;
}

// Receiver template: Options Template Set (id 3), scope field count 1, as in
// the spec's sample ("00 03 00 24 99 92 00 03 00 01 ...").
void putReceiverTemplate(std::vector<uint8_t>& b, bool antenna) {
    const size_t start = b.size();
    auto fields = receiverFields(antenna);
    put16(b, kSetIdOptionsTemplate);
    put16(b, 0);
    put16(b, antenna ? PskReporterClient::kTplReceiverAntenna : PskReporterClient::kTplReceiver);
    put16(b, static_cast<uint16_t>(fields.size()));
    put16(b, 1);  // scope field count
    for (const auto& f : fields) putField(b, f);
    padTo4(b, start);
    set16(b, start + 2, b.size() - start);
}

void putSenderTemplate(std::vector<uint8_t>& b, uint16_t tpl) {
    const size_t start = b.size();
    auto fields = senderFields(tpl);
    put16(b, kSetIdTemplate);
    put16(b, 0);
    put16(b, tpl);
    put16(b, static_cast<uint16_t>(fields.size()));
    for (const auto& f : fields) putField(b, f);
    padTo4(b, start);
    set16(b, start + 2, b.size() - start);
}

size_t receiverTemplateBytes(bool antenna) { return pad4(10 + 8 * receiverFields(antenna).size()); }
size_t senderTemplateBytes(uint16_t tpl) {
    size_t n = 8;
    for (const auto& f : senderFields(tpl)) n += f.enterprise ? 8 : 4;
    return pad4(n);
}

size_t senderRecordBytes(const PskReporterSpot& s) {
    size_t n = strBytes(s.callsign) + 4 + strBytes(s.mode) + 1 + 4;
    if (s.has_snr) n += 1;
    if (hasLocator(s)) n += strBytes(s.locator);
    return n;
}

void putSenderRecord(std::vector<uint8_t>& b, const PskReporterSpot& s) {
    putStr(b, s.callsign);
    put32(b, static_cast<uint32_t>(std::min<uint64_t>(s.frequency_hz, 0xFFFFFFFFull)));
    if (s.has_snr) {
        int v = std::clamp(s.snr_db, -128, 127);
        put8(b, static_cast<uint8_t>(static_cast<int8_t>(v)));
    }
    putStr(b, s.mode);
    put8(b, kInformationSourceAutomatic);
    if (hasLocator(s)) putStr(b, s.locator);
    put32(b, s.flow_start_unix);
}

// Amateur band index for dedup ("a change might be a move to a different band").
int bandOf(uint64_t hz) {
    static const struct { uint64_t lo, hi; } kBands[] = {
        {135700, 137800},       {472000, 479000},       {1800000, 2000000},
        {3500000, 4000000},     {5250000, 5450000},     {7000000, 7300000},
        {10100000, 10150000},   {14000000, 14350000},   {18068000, 18168000},
        {21000000, 21450000},   {24890000, 24990000},   {28000000, 29700000},
        {50000000, 54000000},   {70000000, 71000000},   {144000000, 148000000},
        {219000000, 225000000}, {420000000, 450000000}, {902000000, 928000000},
        {1240000000, 1300000000}};
    int i = 1;
    for (const auto& b : kBands) {
        if (hz >= b.lo && hz <= b.hi) return i;
        ++i;
    }
    return 1000 + static_cast<int>(hz / 1000000);  // out of band: 1 MHz buckets
}

std::string trimUpper(const std::string& in) {
    size_t a = 0, e = in.size();
    while (a < e && std::isspace(static_cast<unsigned char>(in[a]))) ++a;
    while (e > a && std::isspace(static_cast<unsigned char>(in[e - 1]))) --e;
    std::string out = in.substr(a, e - a);
    for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

uint64_t steadySeconds() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

std::string socketErrorText() {
#ifdef _WIN32
    return "socket error " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

uint16_t PskReporterClient::senderTemplateFor(const PskReporterSpot& s) {
    if (s.has_snr) return hasLocator(s) ? kTplSenderSnrLoc : kTplSenderSnr;
    return hasLocator(s) ? kTplSenderLoc : kTplSender;
}

PskReporterClient::Encoded PskReporterClient::encodeDetailed(
    const PskReporterReceiver& rx, const std::vector<PskReporterSpot>& spots, uint32_t sequence,
    uint32_t exportTimeUnix, uint32_t observationDomainId, bool includeTemplates,
    size_t maxDatagramBytes) {
    Encoded out;
    maxDatagramBytes = std::min<size_t>(maxDatagramBytes, 0xFFFF);
    const bool antenna = !rx.antenna.empty();
    const size_t rx_set = pad4(4 + strBytes(rx.callsign) + strBytes(rx.locator) +
                               strBytes(rx.software) + (antenna ? strBytes(rx.antenna) : 0));
    const size_t base = kHeaderBytes + rx_set + (includeTemplates ? receiverTemplateBytes(antenna) : 0);
    if (base > maxDatagramBytes) {
        for (size_t i = 0; i < spots.size(); ++i) out.skipped.push_back(i);
        return out;
    }

    // Greedy packing: each datagram holds one sender set per template variant
    // used (in order of first appearance).
    struct Group {
        uint16_t tpl;
        size_t raw;  // 4-byte set header + records, before padding
        std::vector<size_t> idx;
    };
    auto sizeOf = [&](const std::vector<Group>& groups) {
        size_t n = base;
        for (const auto& g : groups) {
            n += pad4(g.raw);
            if (includeTemplates) n += senderTemplateBytes(g.tpl);
        }
        return n;
    };

    std::vector<std::vector<Group>> packed;
    std::vector<Group> cur;
    for (size_t i = 0; i < spots.size(); ++i) {
        const uint16_t tpl = senderTemplateFor(spots[i]);
        const size_t rec = senderRecordBytes(spots[i]);
        auto tryAdd = [&](std::vector<Group>& groups) {
            std::vector<Group> trial = groups;
            auto it = std::find_if(trial.begin(), trial.end(),
                                   [&](const Group& g) { return g.tpl == tpl; });
            if (it == trial.end()) trial.push_back({tpl, 4 + rec, {i}});
            else { it->raw += rec; it->idx.push_back(i); }
            if (sizeOf(trial) > maxDatagramBytes) return false;
            groups = std::move(trial);
            return true;
        };
        if (tryAdd(cur)) continue;
        if (!cur.empty()) {
            packed.push_back(std::move(cur));
            cur.clear();
            if (tryAdd(cur)) continue;
        }
        out.skipped.push_back(i);  // does not fit even in an empty datagram
    }
    if (!cur.empty() || packed.empty()) packed.push_back(std::move(cur));

    uint32_t seq = sequence;
    for (const auto& groups : packed) {
        std::vector<uint8_t> d;
        d.reserve(sizeOf(groups));
        put16(d, kIpfixVersion);
        put16(d, 0);  // length, patched below
        put32(d, exportTimeUnix);
        put32(d, seq);
        put32(d, observationDomainId);

        std::vector<uint16_t> tpls;
        if (includeTemplates) {
            putReceiverTemplate(d, antenna);
            tpls.push_back(antenna ? kTplReceiverAntenna : kTplReceiver);
            for (const auto& g : groups) {
                putSenderTemplate(d, g.tpl);
                tpls.push_back(g.tpl);
            }
        }

        // Receiver information record (always first in the data part)
        size_t start = d.size();
        put16(d, antenna ? kTplReceiverAntenna : kTplReceiver);
        put16(d, 0);
        putStr(d, rx.callsign);
        putStr(d, rx.locator);
        putStr(d, rx.software);
        if (antenna) putStr(d, rx.antenna);
        padTo4(d, start);
        set16(d, start + 2, d.size() - start);

        std::vector<size_t> indices;
        for (const auto& g : groups) {
            start = d.size();
            put16(d, g.tpl);
            put16(d, 0);
            for (size_t i : g.idx) {
                putSenderRecord(d, spots[i]);
                indices.push_back(i);
            }
            padTo4(d, start);
            set16(d, start + 2, d.size() - start);
        }
        set16(d, 2, d.size());
        seq += static_cast<uint32_t>(indices.size());
        out.datagrams.push_back(std::move(d));
        out.spot_indices.push_back(std::move(indices));
        out.templates.push_back(std::move(tpls));
    }
    return out;
}

std::vector<std::vector<uint8_t>> PskReporterClient::encode(
    const PskReporterReceiver& rx, const std::vector<PskReporterSpot>& spots, uint32_t sequence,
    uint32_t exportTimeUnix, uint32_t observationDomainId, bool includeTemplates,
    size_t maxDatagramBytes) {
    return encodeDetailed(rx, spots, sequence, exportTimeUnix, observationDomainId,
                          includeTemplates, maxDatagramBytes)
        .datagrams;
}

// ---------------------------------------------------------------------------
// Client
// ---------------------------------------------------------------------------

PskReporterClient::PskReporterClient(PskReporterReceiver rx, std::string host, uint16_t port)
    : rx_(std::move(rx)), host_(std::move(host)), port_(port) {
    std::random_device rd;
    std::mt19937_64 rng((static_cast<uint64_t>(rd()) << 32) ^ rd() ^
                        static_cast<uint64_t>(
                            std::chrono::steady_clock::now().time_since_epoch().count()));
    observation_domain_id_ = static_cast<uint32_t>(rng());
    start_jitter_ = rng() % (kMaxStartJitterSec + 1);
    worker_ = std::thread([this] { workerLoop(); });
}

PskReporterClient::~PskReporterClient() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    closeSocket();
}

void PskReporterClient::setReceiver(PskReporterReceiver rx) {
    std::lock_guard<std::mutex> lock(mutex_);
    rx_ = std::move(rx);
}

void PskReporterClient::pushQueue(QueuedSpot q, bool front) {
    if (queue_.size() >= kMaxQueuedSpots) {
        ++dropped_;
        last_error_ = "PSK Reporter queue full: dropped oldest spot (" +
                      std::to_string(dropped_) + " dropped so far)";
        if (front) return;  // the re-queued spot is the oldest one
        queue_.pop_front();
    }
    if (front) queue_.push_front(std::move(q));
    else queue_.push_back(std::move(q));
}

void PskReporterClient::pruneDedup(uint64_t now) {
    for (auto it = dedup_.begin(); it != dedup_.end();) {
        if (it->second.last_time + kUnchangedIntervalSec < now) it = dedup_.erase(it);
        else ++it;
    }
}

void PskReporterClient::addSpot(const PskReporterSpot& in) {
    PskReporterSpot s = in;
    s.callsign = trimUpper(s.callsign);
    std::lock_guard<std::mutex> lock(mutex_);
    if (s.callsign.empty()) {
        last_error_ = "PSK Reporter: spot without callsign ignored";
        return;
    }
    if (s.frequency_hz == 0 || s.frequency_hz > 0xFFFFFFFFull) {
        last_error_ = "PSK Reporter: spot of " + s.callsign + " has no valid frequency";
        return;
    }
    if (s.flow_start_unix == 0) s.flow_start_unix = static_cast<uint32_t>(std::time(nullptr));

    const uint64_t t = s.flow_start_unix;
    const int band = bandOf(s.frequency_hz);
    auto it = dedup_.find(s.callsign);
    if (it != dedup_.end()) {
        const DedupEntry& e = it->second;
        const uint64_t dt = t >= e.last_time ? t - e.last_time : 0;
        const bool unchanged = e.band == band && e.mode == s.mode;
        if (dt < kCallsignMinIntervalSec || (unchanged && dt < kUnchangedIntervalSec)) {
            ++duplicates_;
            return;
        }
    }
    dedup_[s.callsign] = DedupEntry{t, band, s.mode};
    if (dedup_.size() > 4096) pruneDedup(t);
    pushQueue(QueuedSpot{std::move(s), 0}, false);
}

bool PskReporterClient::templateNeeded(uint16_t id, uint64_t now) const {
    auto c = template_sent_count_.find(id);
    if (c == template_sent_count_.end() || c->second < kInitialTemplateDatagrams) return true;
    auto t = template_last_sent_.find(id);
    return t == template_last_sent_.end() || now >= t->second + kTemplateResendSec;
}

bool PskReporterClient::startUpload(uint64_t now, bool only_full_datagrams) {
    if (queue_.empty() || busy_) return false;

    std::vector<PskReporterSpot> spots;
    spots.reserve(queue_.size());
    for (const auto& q : queue_) spots.push_back(q.spot);

    const bool antenna = !rx_.antenna.empty();
    bool include = templateNeeded(antenna ? kTplReceiverAntenna : kTplReceiver, now);
    for (const auto& s : spots) {
        if (include) break;
        include = templateNeeded(senderTemplateFor(s), now);
    }

    Encoded enc = encodeDetailed(rx_, spots, sequence_, static_cast<uint32_t>(now),
                                 observation_domain_id_, include, max_datagram_bytes_);
    size_t take = enc.datagrams.size();
    if (only_full_datagrams) {
        if (take <= 1) return false;
        take -= 1;  // the last datagram is not full yet; it waits for the timer
    }

    auto job = std::make_unique<Job>();
    job->now = now;
    std::vector<bool> consumed(queue_.size(), false);
    for (size_t k = 0; k < take; ++k) {
        std::vector<QueuedSpot> qs;
        for (size_t i : enc.spot_indices[k]) {
            qs.push_back(queue_[i]);
            consumed[i] = true;
        }
        sequence_ += static_cast<uint32_t>(enc.spot_indices[k].size());
        job->datagrams.push_back(std::move(enc.datagrams[k]));
        job->spots.push_back(std::move(qs));
        job->templates.push_back(std::move(enc.templates[k]));
    }
    for (size_t i : enc.skipped) {
        consumed[i] = true;
        ++dropped_;
        last_error_ = "PSK Reporter: spot of " + queue_[i].spot.callsign +
                      " too large for a datagram, dropped";
    }
    std::deque<QueuedSpot> rest;
    for (size_t i = 0; i < queue_.size(); ++i)
        if (!consumed[i]) rest.push_back(std::move(queue_[i]));
    queue_ = std::move(rest);

    if (job->datagrams.empty()) return false;
    last_attempt_unix_ = now;
    job_ = std::move(job);
    busy_ = true;
    cv_.notify_all();
    return true;
}

void PskReporterClient::poll(uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Timer grid anchored at the first poll (program start) plus a random
    // offset - never synchronised to the wall clock.
    if (next_due_unix_ == 0) next_due_unix_ = now + kUploadIntervalSec + start_jitter_;
    if (busy_) return;

    if (rx_.callsign.empty()) {
        if (!queue_.empty()) last_error_ = "PSK Reporter: receiver callsign not set";
        return;
    }

    if (now >= next_due_unix_) {
        const bool rate_ok = last_attempt_unix_ == 0 ||
                             now >= last_attempt_unix_ + kUploadIntervalSec;
        if (rate_ok) startUpload(now, false);
        while (next_due_unix_ <= now) next_due_unix_ += kUploadIntervalSec;
        last_full_check_size_ = queue_.size();
        return;
    }

    // Spec: no more than one datagram per 5 minutes "unless the packet
    // becomes full" - send completely filled datagrams early.
    if (queue_.size() != last_full_check_size_) {
        last_full_check_size_ = queue_.size();
        if (startUpload(now, true)) last_full_check_size_ = queue_.size();
    }
}

bool PskReporterClient::flushNow(uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (next_due_unix_ == 0) next_due_unix_ = now + kUploadIntervalSec + start_jitter_;
    if (busy_) {
        last_error_ = "PSK Reporter: an upload is already in progress";
        return false;
    }
    if (rx_.callsign.empty()) {
        last_error_ = "PSK Reporter: receiver callsign not set";
        return false;
    }
    if (queue_.empty()) {
        last_error_ = "PSK Reporter: no spots queued";
        return false;
    }
    if (last_attempt_unix_ != 0 && now < last_attempt_unix_ + kUploadIntervalSec) {
        last_error_ = "PSK Reporter allows one upload per 5 minutes; next possible in " +
                      std::to_string(last_attempt_unix_ + kUploadIntervalSec - now) + " s";
        return false;
    }
    if (!startUpload(now, false)) return false;
    while (next_due_unix_ <= now) next_due_unix_ += kUploadIntervalSec;
    last_full_check_size_ = queue_.size();
    return true;
}

void PskReporterClient::workerLoop() {
    for (;;) {
        std::unique_ptr<Job> job;
        std::function<bool(const std::vector<uint8_t>&)> transport;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || job_; });
            if (stop_) {
                busy_ = false;
                cv_.notify_all();
                return;
            }
            job = std::move(job_);
            transport = test_transport_;
        }

        std::vector<bool> ok(job->datagrams.size(), false);
        std::string err;
        for (size_t k = 0; k < job->datagrams.size(); ++k) {
            if (transport) {
                ok[k] = transport(job->datagrams[k]);
                if (!ok[k]) err = "PSK Reporter: send failed (test transport)";
            } else {
                std::string e;
                ok[k] = sendDatagram(job->datagrams[k], e);
                if (!ok[k]) err = e;
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        bool all_ok = true;
        for (size_t k = 0; k < job->datagrams.size(); ++k) {
            if (ok[k]) {
                spots_sent_ += job->spots[k].size();
                ++datagrams_sent_;
                last_upload_unix_ = job->now;
                for (uint16_t id : job->templates[k]) {
                    ++template_sent_count_[id];
                    template_last_sent_[id] = job->now;
                }
            } else {
                all_ok = false;
                // Bounded re-queue (front, original order); give up after a
                // few attempts.
                for (auto it = job->spots[k].rbegin(); it != job->spots[k].rend(); ++it) {
                    QueuedSpot q = *it;
                    if (++q.attempts >= kMaxSendAttempts) {
                        ++dropped_;
                        continue;
                    }
                    pushQueue(std::move(q), true);
                }
            }
        }
        if (all_ok) last_error_.clear();
        else last_error_ = err;
        busy_ = false;
        cv_.notify_all();
    }
}

void PskReporterClient::closeSocket() {
    if (sock_ != -1) {
#ifdef _WIN32
        closesocket(static_cast<SOCKET>(sock_));
#else
        ::close(static_cast<int>(sock_));
#endif
        sock_ = -1;
        sock_family_ = 0;
    }
}

bool PskReporterClient::sendDatagram(const std::vector<uint8_t>& d, std::string& err) {
    const uint64_t now = steadySeconds();
    if (addr_.empty() || now >= resolved_at_ + 3600) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        addrinfo* res = nullptr;
        const std::string port = std::to_string(port_);
        int rc = getaddrinfo(host_.c_str(), port.c_str(), &hints, &res);
        if (rc != 0 || res == nullptr) {
            err = "PSK Reporter: cannot resolve " + host_ + ": " +
#ifdef _WIN32
                  std::to_string(rc);
#else
                  gai_strerror(rc);
#endif
            if (res) freeaddrinfo(res);
            return false;
        }
        // Prefer IPv4 (most residential setups), else the first result.
        addrinfo* pick = res;
        for (addrinfo* ai = res; ai; ai = ai->ai_next)
            if (ai->ai_family == AF_INET) { pick = ai; break; }
        const auto* p = reinterpret_cast<const uint8_t*>(pick->ai_addr);
        addr_.assign(p, p + pick->ai_addrlen);
        const int family = pick->ai_family;
        freeaddrinfo(res);
        resolved_at_ = now;
        if (family != sock_family_) closeSocket();
        sock_family_ = family;
    }

    if (sock_ == -1) {
        // One socket for the whole session: the spec asks for the same UDP
        // source port on every datagram (the server caches templates per
        // source address/port).
#ifdef _WIN32
        SOCKET s = ::socket(sock_family_, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) {
#else
        int s = ::socket(sock_family_, SOCK_DGRAM, IPPROTO_UDP);
        if (s < 0) {
#endif
            err = "PSK Reporter: cannot create UDP socket: " + socketErrorText();
            return false;
        }
        sock_ = static_cast<intptr_t>(s);
    }

#ifdef _WIN32
    int n = ::sendto(static_cast<SOCKET>(sock_), reinterpret_cast<const char*>(d.data()),
                     static_cast<int>(d.size()), 0,
                     reinterpret_cast<const sockaddr*>(addr_.data()),
                     static_cast<int>(addr_.size()));
#else
    ssize_t n = ::sendto(static_cast<int>(sock_), d.data(), d.size(), 0,
                         reinterpret_cast<const sockaddr*>(addr_.data()),
                         static_cast<socklen_t>(addr_.size()));
#endif
    if (n < 0 || static_cast<size_t>(n) != d.size()) {
        err = "PSK Reporter: UDP send to " + host_ + " failed: " + socketErrorText();
        addr_.clear();  // re-resolve on the next attempt
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Accessors / test seams
// ---------------------------------------------------------------------------

size_t PskReporterClient::queuedSpots() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}
std::string PskReporterClient::lastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}
uint64_t PskReporterClient::lastUploadUnix() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_upload_unix_;
}
uint64_t PskReporterClient::spotsSent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return spots_sent_;
}
uint64_t PskReporterClient::droppedSpots() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}
uint64_t PskReporterClient::duplicateSpots() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return duplicates_;
}
uint64_t PskReporterClient::datagramsSent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return datagrams_sent_;
}
uint32_t PskReporterClient::nextSequence() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sequence_;
}
uint64_t PskReporterClient::nextUploadDueUnix() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_due_unix_;
}
bool PskReporterClient::uploadInProgress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return busy_;
}

void PskReporterClient::setTransportForTest(std::function<bool(const std::vector<uint8_t>&)> send) {
    std::lock_guard<std::mutex> lock(mutex_);
    test_transport_ = std::move(send);
}
void PskReporterClient::setMaxDatagramBytesForTest(size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_datagram_bytes_ = bytes;
}
void PskReporterClient::setStartJitterForTest(uint64_t seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    start_jitter_ = seconds;
}
void PskReporterClient::waitIdleForTest() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !busy_; });
}

}  // namespace skylink
