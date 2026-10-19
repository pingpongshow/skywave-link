// Live voice control decisions; see voice_control.hpp.

#include "skywave_link/voice_control.hpp"

#include <algorithm>
#include <cctype>

namespace skylink {

namespace {

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return s;
}

bool placeholderCallsign(const std::string& call) {
    const std::string u = upper(call);
    return u.empty() || u == "N0CALL" || u == "-";
}

// The slowest ladder mode that carries real-time voice in `family`
// (VOICE.md AUTO table); -1 when the family has none.
int voiceFloorMode(const std::string& family) {
    const std::string f = upper(family);
    if (f == "NARROW") return -1;
    if (f == "MID") return 12;
    return 8;  // WIDE, ULTRA, VHF
}

// Why a CONNECTED session's current mode cannot carry voice; "" when it can
// (or the mode is unknown).
std::string linkModeProblem(const VoiceContext& c) {
    if (c.link_mode_index < 0) return "";
    const int floor = voiceFloorMode(c.family);
    if (floor < 0) {
        return "the " + upper(c.family) +
               " family has no real-time voice; switch to WIDE or ULTRA";
    }
    if (c.link_mode_index < floor) {
        return "the link is at M" + std::to_string(c.link_mode_index) + "; voice needs M" +
               std::to_string(floor) +
               " or faster (it starts once the link speeds up)";
    }
    return "";
}

}  // namespace

bool voiceSessionOpen(const VoiceStatus& voice) {
    return voice.state != VoiceState::Idle && voice.state != VoiceState::Closing;
}

bool voiceSessionReadyForPtt(const VoiceStatus& voice) {
    return voice.state == VoiceState::Ready || voice.state == VoiceState::TxRequested ||
           voice.state == VoiceState::TxActive || voice.state == VoiceState::RxActive;
}

std::string voiceStartBlocker(const VoiceContext& c) {
    if (!c.modem_connected) {
        return "Voice: the modem is not connected (Settings > Modem)";
    }
    if (c.modem_voice_unsupported) {
        return "Voice: this modem was built without voice (ERROR:UNSUPPORTED)";
    }
    if (c.configured_mode == VoiceMode::Off) {
        return "Voice is off: choose CONNECTED or BROADCAST in Settings > Voice > Usage";
    }
    if (!c.voice_media) {
        return "Voice: the modem's voice port is closed, so no audio can flow "
               "(check the modem's voice port in Settings > Modem)";
    }
    if (placeholderCallsign(c.my_callsign) && c.link_state != LinkState::Connected) {
        return "Voice: set your callsign first (Settings > Station); the modem "
               "refuses to transmit as N0CALL";
    }
    if (c.configured_mode == VoiceMode::Connected) {
        if (c.link_state != LinkState::Connected) {
            return "Voice (CONNECTED) needs a station link: CONNECT to a station first, "
                   "or use BROADCAST in Settings > Voice";
        }
        const std::string mode_problem = linkModeProblem(c);
        if (!mode_problem.empty()) return "Voice cannot start: " + mode_problem;
        return "";
    }
    // Broadcast
    if (c.link_state != LinkState::Idle) {
        return "Voice (BROADCAST) needs an idle station: disconnect the link first, "
               "or use CONNECTED in Settings > Voice";
    }
    if (voiceFloorMode(c.family) < 0) {
        return "Voice cannot start: the " + upper(c.family) +
               " family has no real-time voice; switch to WIDE or ULTRA";
    }
    return "";
}

std::string explainVoiceCommandError(const std::string& verb, const std::string& response,
                                     const VoiceContext& c) {
    const std::string r = upper(response);
    if (r.rfind("ERROR:", 0) != 0) {
        if (response.empty()) return verb + ": no answer from the modem";
        return "";
    }
    if (r == "ERROR:UNSUPPORTED") {
        return "Voice: this modem was built without voice (" + verb + " -> ERROR:UNSUPPORTED)";
    }
    if (r == "ERROR:STATE") {
        if (placeholderCallsign(c.my_callsign)) {
            return "Voice: the modem refuses to transmit as N0CALL; set your callsign "
                   "(Settings > Station)";
        }
        if (verb == "VOICEPTT") {
            return "Voice PTT refused: the voice session is not open (the modem "
                   "closed it or the peer did not answer)";
        }
        if (c.configured_mode == VoiceMode::Connected) {
            if (c.link_state != LinkState::Connected) {
                return "Voice (CONNECTED) needs a station link: CONNECT first";
            }
            const std::string mode_problem = linkModeProblem(c);
            if (!mode_problem.empty()) return "Voice refused: " + mode_problem;
            return "Voice refused by the modem: the peer may not support voice, or "
                   "voice is already open in another mode";
        }
        if (c.configured_mode == VoiceMode::Broadcast) {
            return "Voice (BROADCAST) refused: the station must be idle (no link) and "
                   "the family must carry voice";
        }
        return "Voice refused: voice is off on the modem";
    }
    return "Voice: " + verb + " -> " + response;
}

const char* voiceStateName(VoiceState state) {
    switch (state) {
        case VoiceState::Opening: return "OPENING";
        case VoiceState::Ready: return "OPEN";
        case VoiceState::TxRequested: return "REQUESTING";
        case VoiceState::TxActive: return "TALKING";
        case VoiceState::RxActive: return "RECEIVING";
        case VoiceState::Closing: return "CLOSING";
        case VoiceState::Idle:
        default: return "CLOSED";
    }
}

VoiceButtonView voiceButtonView(const VoiceContext& c, bool start_pending,
                                const std::string& last_error) {
    VoiceButtonView v;
    const VoiceStatus& s = c.voice;
    v.session_open = c.modem_connected && voiceSessionOpen(s);
    std::string detail;
    if (!c.modem_connected || c.modem_voice_unsupported ||
        c.configured_mode == VoiceMode::Off) {
        v.kind = VoiceIndicator::Unavailable;
        v.label = "VOICE";
        detail = voiceStartBlocker(c);
    } else if (s.state == VoiceState::TxActive) {
        v.kind = VoiceIndicator::Tx;
        v.label = "VOICE TX";
        detail = "Talking (" + s.codec + "). Click to close voice.";
    } else if (s.state == VoiceState::TxRequested) {
        v.kind = VoiceIndicator::Requesting;
        v.label = "VOICE TX...";
        detail = "Waiting for the peer to grant the channel. Click to close voice.";
    } else if (s.state == VoiceState::RxActive || s.broadcast_rx) {
        v.kind = VoiceIndicator::Rx;
        v.label = "VOICE RX";
        detail = s.broadcast_rx ? std::string("Playing a voice broadcast")
                                : "The peer is talking (" + s.codec + ")";
        if (v.session_open) detail += ". Click to close voice.";
    } else if (s.state == VoiceState::Opening || (start_pending && !v.session_open)) {
        v.kind = VoiceIndicator::Starting;
        v.label = "VOICE...";
        detail = "Opening voice. Click to cancel.";
        v.session_open = true;  // a click stops/cancels
    } else if (v.session_open) {
        v.kind = VoiceIndicator::On;
        v.label = "VOICE ON";
        detail = std::string(s.mode == VoiceMode::Broadcast ? "Broadcast" : "Connected") +
                 " voice open (" + s.codec + "). Hold PTT (or the PTT hotkey) to talk; "
                 "click to close voice.";
    } else {
        v.kind = VoiceIndicator::Idle;
        v.label = "VOICE";
        const std::string blocker = voiceStartBlocker(c);
        detail = blocker.empty()
                     ? std::string("Click to open ") +
                           (c.configured_mode == VoiceMode::Broadcast ? "broadcast" : "connected") +
                           " voice (PTT also opens it)."
                     : blocker;
    }
    v.tooltip = detail;
    if (!last_error.empty() && last_error != detail && v.kind != VoiceIndicator::Tx &&
        v.kind != VoiceIndicator::Rx) {
        v.tooltip += "\nLast problem: " + last_error;
    }
    return v;
}

bool VoiceNoticeLimiter::allow(const std::string& text,
                               std::chrono::steady_clock::time_point now) {
    if (text == last_text_ && last_at_.time_since_epoch().count() != 0 && now - last_at_ < gap_) {
        return false;
    }
    last_text_ = text;
    last_at_ = now;
    return true;
}

}  // namespace skylink
