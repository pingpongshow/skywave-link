// Live voice control decisions (VOICE button, PTT, hotkey, Settings Voice
// tab): whether a voice session can start, why not, what the modem's answer
// to VOICESTART / VOICEPTT means to the operator, and what the VOICE button
// shows. Pure logic, so it is unit-tested without SDL or a modem.
#pragma once

#include "skywave_link/modem_client.hpp"

#include <chrono>
#include <cstdint>
#include <string>

namespace skylink {

// What the App knows when deciding about voice.
struct VoiceContext {
    bool modem_connected = false;
    // The modem answered a voice command with ERROR:UNSUPPORTED (a build
    // without voice).
    bool modem_voice_unsupported = false;
    // The voice media socket (port 9102 by default) is open.
    bool voice_media = false;
    VoiceMode configured_mode = VoiceMode::Off;  // Settings > Voice > Usage
    LinkState link_state = LinkState::Idle;
    int link_mode_index = -1;                    // ladder index while linked
    std::string family = "WIDE";
    std::string my_callsign;                     // the modem's MYCALL
    VoiceStatus voice;                           // last VOICESTATUS / EVT:VOICE
};

// A voice service is open on the modem (OPENING..RECEIVING; not CLOSED or
// CLOSING).
bool voiceSessionOpen(const VoiceStatus& voice);
// The open service can take VOICEPTT ON now (OPEN, REQUESTING, TALKING,
// RECEIVING).
bool voiceSessionReadyForPtt(const VoiceStatus& voice);

// "" when VOICESTART may be sent now; otherwise the operator-facing reason
// voice cannot start (no modem, voice off, CONNECTED needs a station link,
// BROADCAST needs an idle link, voice port closed, modem without voice,
// placeholder callsign, link mode too slow for voice).
std::string voiceStartBlocker(const VoiceContext& c);

// Why the modem refused `verb` (VOICESTART, VOICEPTT, VOICE, VOICECODEC)
// with `response` (ERROR:...), in operator terms. "" when the response is
// not an error.
std::string explainVoiceCommandError(const std::string& verb, const std::string& response,
                                     const VoiceContext& c);

enum class VoiceIndicator {
    Unavailable,  // no modem / voice off / unsupported
    Idle,         // voice possible, no session
    Starting,     // VOICESTART sent / OPENING
    On,           // session open, nobody talking
    Requesting,   // our PTT request waits for the grant
    Tx,           // we are talking
    Rx,           // the peer (or a broadcast) is talking
};

struct VoiceButtonView {
    std::string label;
    std::string tooltip;
    VoiceIndicator kind = VoiceIndicator::Unavailable;
    bool session_open = false;  // a click stops the session
};

// The main panel VOICE button: label, state and tooltip. `start_pending`:
// a VOICESTART is on its way; `last_error`: the last voice problem ("" for
// none), shown in the tooltip.
VoiceButtonView voiceButtonView(const VoiceContext& c, bool start_pending,
                                const std::string& last_error);

// Suppresses repeats of the same message within `gap` (hotkey presses, a
// held PTT button), so a refused PTT reports once, not per press.
class VoiceNoticeLimiter {
public:
    explicit VoiceNoticeLimiter(std::chrono::milliseconds gap = std::chrono::milliseconds(3000))
        : gap_(gap) {}
    bool allow(const std::string& text, std::chrono::steady_clock::time_point now);

private:
    std::chrono::milliseconds gap_;
    std::string last_text_;
    std::chrono::steady_clock::time_point last_at_{};
};

// Real-time pacing of filler silence for the uplink: how many samples are
// owed to keep `elapsed` worth of 8 kHz audio flowing when `sent` samples
// went out since the talkspurt started.
inline uint64_t voiceSamplesOwed(std::chrono::steady_clock::duration elapsed, uint64_t sent) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    const uint64_t due = ms > 0 ? static_cast<uint64_t>(ms) * 8u : 0u;
    return due > sent ? due - sent : 0u;
}

// Where a voice action came from; feedback goes to that window's status
// line (and always to the log).
enum class VoiceUiSource { MainPanel, Settings, Hotkey, Script, Auto };

// What the windows show about voice (App fills it every frame).
struct VoiceUiState {
    VoiceButtonView button;
    // PTT is clickable whenever the modem is connected and voice is not
    // OFF; a press that cannot key says why.
    bool ptt_available = false;
    bool ptt_wanted = false;        // a PTT button / hotkey is held
    std::string ptt_tooltip;
    bool audio_ready = false;       // capture + playback devices open
    bool audio_opening = false;
    std::string audio_error;        // last device-open failure
    std::string last_error;         // last voice problem reported
    uint64_t frames_sent = 0;       // uplink records sent to the voice port
    uint64_t samples_sent = 0;
    uint64_t samples_received = 0;  // downlink PCM from the voice port
    uint64_t samples_played = 0;    // queued to the playback device
    uint64_t tx_dropped = 0;
    int capture_peak = 0;           // peak |sample| of the last capture block
};

// Operator-facing label for VoiceState.
const char* voiceStateName(VoiceState state);

}  // namespace skylink
