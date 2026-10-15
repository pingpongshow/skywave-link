// skywave_link_probe - connect ModemClient to a running Skywave modem and
// report what came back.
//
// This is the fastest way to tell whether the app and the modem agree about
// the control protocol, without launching the GUI:
//
//   ./build/skywave_link_probe [host] [cmd_port] [data_port] [voice_port]
//
// Exit status is 0 when the modem answered every query.

#include "skywave_link/modem_client.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace skylink;

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    const uint16_t cmd_port =
        static_cast<uint16_t>(argc > 2 ? std::atoi(argv[2]) : kDefaultCommandPort);
    const uint16_t data_port =
        static_cast<uint16_t>(argc > 3 ? std::atoi(argv[3]) : kDefaultDataPort);
    const uint16_t voice_port =
        static_cast<uint16_t>(argc > 4 ? std::atoi(argv[4]) : kDefaultVoicePort);

    ModemClient modem;
    modem.onEvent([](const std::string& name, const std::string& args) {
        std::cout << "  EVT  " << name << (args.empty() ? "" : " " + args) << "\n";
    });
    modem.onMonitorFrame([](const MonitorFrame& f) {
        std::cout << "  MON  " << f.frame_type << " from "
                  << (f.src_callsign.empty() ? "?" : f.src_callsign)
                  << " (" << f.family << " M" << f.mode << ", "
                  << f.payload.size() << " body bytes"
                  << (f.truncated ? ", truncated" : "") << ")\n";
    });
    modem.onBroadcast([](const std::string& src, const std::vector<uint8_t>& p) {
        std::cout << "  BCAST " << p.size() << " bytes from "
                  << (src.empty() ? "?" : src) << "\n";
    });

    std::cout << "connecting to " << host << " (" << cmd_port << "/" << data_port
              << "/" << voice_port << ")\n";
    if (!modem.connect(host, cmd_port, data_port, voice_port)) {
        std::cerr << "FAILED: could not open all three ports. Is the modem "
                     "running, and is another client already attached? "
                     "(Skywave allows one client per port.)\n";
        return 1;
    }

    int failures = 0;
    auto require = [&failures](bool ok, const char* what) {
        std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
        if (!ok) ++failures;
    };

    const std::string version = modem.getVersion();
    require(!version.empty(), "VERSION");
    if (!version.empty()) std::cout << "       modem version: " << version << "\n";

    require(modem.refreshStatus(), "STATUS");
    const ModemStatus status = modem.getStatus();
    std::cout << "       call=" << (status.my_callsign.empty() ? "-" : status.my_callsign)
              << " family=" << status.family
              << " mode=" << status.mode
              << " net=" << status.net_bps << " bps"
              << " snr=" << status.snr_db << " dB"
              << " listen=" << (status.listening ? "1" : "0")
              << " busy=" << (status.busy_detected ? "1" : "0") << "\n";

    const auto noise = modem.getNoiseFloor();
    require(noise.has_value(), "NOISEFLOOR");
    if (noise) {
        std::cout << "       floor " << noise->noise_dbfs << " dBFS, busy="
                  << (noise->busy ? "1" : "0") << "\n";
    }

    const auto levels = modem.getAudioLevels();
    require(levels.has_value(), "AUDIOLEVEL");
    if (levels) {
        std::cout << "       rx " << levels->rx_dbfs << " dBFS, tx "
                  << levels->tx_dbfs << " dBFS\n";
    }

    const auto freq = modem.getFrequencyInfo();
    require(freq.has_value(), "FREQINFO");
    if (freq) {
        std::cout << "       center " << freq->center_hz << " Hz, shift "
                  << freq->shift_hz << " Hz, cfo " << freq->cfo_hz << " Hz\n";
    }

    require(modem.setMonitorMode(true), "MONITOR ON");
    require(modem.setListenMode(true), "LISTEN ON");
    require(modem.setMode("AUTO"), "MODE AUTO");
    require(modem.setCpMode("AUTO"), "CPMODE AUTO");

    // Voice and crypto are build-gated in the modem; ERROR:UNSUPPORTED is a
    // valid answer, so these are reported rather than required.
    std::cout << "  info VOICESTATUS "
              << (modem.queryVoiceStatus() ? "supported" : "unsupported")
              << "\n";

    std::cout << "listening for 5 s of notifications...\n";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        modem.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    modem.disconnect();
    std::cout << (failures == 0 ? "PROBE OK\n" : "PROBE FAILED\n");
    return failures == 0 ? 0 : 1;
}
