// PSK Reporter uploader - IPFIX (RFC 7011 / RFC 5101) reception reports sent
// over UDP to report.pskreporter.info:4739, as documented at
// https://pskreporter.info/pskdev.html ("Data Reporting" / "Cookie Cutter").
//
// Layout of each datagram (all integers big-endian):
//   message header (version 10, length, export time, sequence, observation id)
//   [template sets: receiver options template + the sender templates used]
//   receiver data set (one record: callsign, locator, software[, antenna])
//   sender data set(s) (one record per spot), each set NUL-padded to 4 bytes
//
// Cadence / dedup rules implemented (all from the spec):
//   - at most one timed upload per 5 minutes, timer based on start-up time plus
//     a random offset (never aligned to the wall clock); a datagram that is
//     already full may go early;
//   - a callsign is reported at most once per 5 minutes, and at most once per
//     hour while unchanged (same band and mode);
//   - templates are sent in the first three datagrams and then hourly;
//   - the sequence number counts reports (sender records), not packets;
//   - one random observation domain id per client (session), one UDP socket
//     (stable source port) for the whole session.
//
// Thread model: addSpot/poll/flushNow are called from one (GUI) thread and
// never block on the network; DNS resolution and the UDP send run on a helper
// thread owned by the client.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace skylink {

struct PskReporterReceiver {
    std::string callsign;   // receiverCallsign  (30351.2)
    std::string locator;    // receiverLocator   (30351.4)
    std::string software;   // decoderSoftware   (30351.8), name + version
    std::string antenna;    // antennaInformation (30351.9), optional
};

struct PskReporterSpot {
    std::string callsign;          // senderCallsign (30351.1)
    uint64_t frequency_hz = 0;     // frequency (30351.5), 4-byte field
    bool has_snr = false;
    int snr_db = 0;                // sNR (30351.6), 1 byte, clamped to -128..127
    std::string mode;              // mode (30351.10)
    std::string locator;           // senderLocator (30351.3), optional
    uint32_t flow_start_unix = 0;  // flowStartSeconds (IANA 150); 0 = time of addSpot
};

class PskReporterClient {
public:
    // Spec constants
    static constexpr uint32_t kEnterpriseNumber = 30351;
    static constexpr uint64_t kUploadIntervalSec = 300;     // >= 5 min between datagrams
    static constexpr uint64_t kMaxStartJitterSec = 60;      // random offset of the timer
    static constexpr uint64_t kCallsignMinIntervalSec = 300;  // once per 5 min per call
    static constexpr uint64_t kUnchangedIntervalSec = 3600;   // once per hour if unchanged
    static constexpr uint64_t kTemplateResendSec = 3600;      // templates hourly
    static constexpr int kInitialTemplateDatagrams = 3;       // + in the first three
    static constexpr size_t kMaxQueuedSpots = 2000;
    static constexpr int kMaxSendAttempts = 3;
    static constexpr size_t kDefaultMaxDatagramBytes = 1400;

    // Template (set) ids. 0x9992/0x9993 are the spec's ids for the receiver
    // (no antenna) and the basic sender template; the other variants get
    // their own ids (the spec asks for distinct ids for distinct templates).
    static constexpr uint16_t kTplReceiver = 0x9992;          // call, loc, software
    static constexpr uint16_t kTplReceiverAntenna = 0x9994;   // + antennaInformation
    static constexpr uint16_t kTplSender = 0x9993;            // call, freq, mode, src, time
    static constexpr uint16_t kTplSenderLoc = 0x9995;         // + senderLocator
    static constexpr uint16_t kTplSenderSnr = 0x9996;         // + sNR
    static constexpr uint16_t kTplSenderSnrLoc = 0x9997;      // + sNR + senderLocator

    explicit PskReporterClient(PskReporterReceiver rx,
                               std::string host = "report.pskreporter.info",
                               uint16_t port = 4739);
    ~PskReporterClient();

    PskReporterClient(const PskReporterClient&) = delete;
    PskReporterClient& operator=(const PskReporterClient&) = delete;

    void setReceiver(PskReporterReceiver rx);

    // Queue one decoded spot. Dedup: a callsign already reported/queued in the
    // last 5 minutes is dropped, and within the hour if band and mode are
    // unchanged. Spots without a callsign or frequency are rejected.
    void addSpot(const PskReporterSpot& s);

    // Call regularly (e.g. once per frame / second). Starts an upload on the
    // helper thread when the 5-minute timer is due (or a datagram is full).
    // Never blocks on DNS or the network.
    void poll(uint64_t now_unix);

    // "Upload Now": sends the queue immediately unless that would violate the
    // 5-minute minimum interval, an upload is still in progress, nothing is
    // queued or the receiver callsign is empty. Returns false + lastError().
    bool flushNow(uint64_t now_unix);

    size_t queuedSpots() const;
    std::string lastError() const;
    uint64_t lastUploadUnix() const;   // time of the last successful datagram
    uint64_t spotsSent() const;        // spots in successfully sent datagrams

    // Extra status
    uint64_t droppedSpots() const;     // queue overflow / repeated send failure
    uint64_t duplicateSpots() const;   // rejected by the dedup rules
    uint64_t datagramsSent() const;
    uint32_t observationDomainId() const { return observation_domain_id_; }
    uint32_t nextSequence() const;
    uint64_t nextUploadDueUnix() const;  // 0 until the first poll()
    bool uploadInProgress() const;

    // Pure encoder. Produces as many datagrams (each <= maxDatagramBytes) as
    // needed; every datagram carries the receiver record, and when
    // includeTemplates is set, the receiver template plus the sender
    // templates used in that datagram. Datagram k's sequence number is
    // `sequence` + number of sender records in datagrams 0..k-1.
    struct Encoded {
        std::vector<std::vector<uint8_t>> datagrams;
        std::vector<std::vector<size_t>> spot_indices;     // input indices per datagram
        std::vector<std::vector<uint16_t>> templates;      // template ids per datagram
        std::vector<size_t> skipped;                       // spots that cannot fit at all
    };
    static Encoded encodeDetailed(const PskReporterReceiver& rx,
                                  const std::vector<PskReporterSpot>& spots,
                                  uint32_t sequence, uint32_t exportTimeUnix,
                                  uint32_t observationDomainId, bool includeTemplates,
                                  size_t maxDatagramBytes = kDefaultMaxDatagramBytes);
    static std::vector<std::vector<uint8_t>> encode(
        const PskReporterReceiver& rx, const std::vector<PskReporterSpot>& spots,
        uint32_t sequence, uint32_t exportTimeUnix, uint32_t observationDomainId,
        bool includeTemplates, size_t maxDatagramBytes = kDefaultMaxDatagramBytes);

    // Sender template id used for a spot (depends on has_snr / locator).
    static uint16_t senderTemplateFor(const PskReporterSpot& s);

    // Test seams
    void setTransportForTest(std::function<bool(const std::vector<uint8_t>&)> send);
    void setMaxDatagramBytesForTest(size_t bytes);
    void setStartJitterForTest(uint64_t seconds);
    // Blocks until the helper thread is idle (tests only).
    void waitIdleForTest();

private:
    struct QueuedSpot {
        PskReporterSpot spot;
        int attempts = 0;
    };
    struct DedupEntry {
        uint64_t last_time = 0;
        int band = 0;
        std::string mode;
    };
    struct Job {
        std::vector<std::vector<uint8_t>> datagrams;
        std::vector<std::vector<QueuedSpot>> spots;   // per datagram
        std::vector<std::vector<uint16_t>> templates; // per datagram
        uint64_t now = 0;
    };

    bool startUpload(uint64_t now_unix, bool only_full_datagrams);  // mutex held
    void workerLoop();
    bool sendDatagram(const std::vector<uint8_t>& d, std::string& err);  // worker
    void closeSocket();
    bool templateNeeded(uint16_t id, uint64_t now) const;  // mutex held
    void pruneDedup(uint64_t now);                          // mutex held
    void pushQueue(QueuedSpot q, bool front);               // mutex held

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    PskReporterReceiver rx_;
    std::string host_;
    uint16_t port_;
    uint32_t observation_domain_id_ = 0;
    uint32_t sequence_ = 0;
    std::deque<QueuedSpot> queue_;
    std::map<std::string, DedupEntry> dedup_;
    std::map<uint16_t, int> template_sent_count_;
    std::map<uint16_t, uint64_t> template_last_sent_;
    std::string last_error_;
    uint64_t last_upload_unix_ = 0;
    uint64_t last_attempt_unix_ = 0;
    uint64_t next_due_unix_ = 0;
    uint64_t start_jitter_ = 0;
    uint64_t spots_sent_ = 0;
    uint64_t datagrams_sent_ = 0;
    uint64_t dropped_ = 0;
    uint64_t duplicates_ = 0;
    size_t max_datagram_bytes_ = kDefaultMaxDatagramBytes;
    size_t last_full_check_size_ = 0;
    std::function<bool(const std::vector<uint8_t>&)> test_transport_;

    std::unique_ptr<Job> job_;   // pending job for the worker
    bool busy_ = false;          // job queued or running
    bool stop_ = false;
    std::thread worker_;

    // Worker-owned socket state
    intptr_t sock_ = -1;
    int sock_family_ = 0;
    uint64_t resolved_at_ = 0;
    std::vector<uint8_t> addr_;  // sockaddr storage
};

}  // namespace skylink
