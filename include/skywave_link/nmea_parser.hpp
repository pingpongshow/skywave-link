// NMEA 0183 parser - the one GPS sentence parser (GGA, RMC, GLL).
//
// Every sentence must carry a valid `*hh` checksum. Fields are split
// keeping empty ones (a standard GGA ends in empty DGPS fields), and every
// numeric field is checked, so a garbled line never becomes a position.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace skylink {

enum class NmeaStatus {
    Fix,            // a position (valid == true)
    NoFix,          // well-formed, but the receiver has no fix
    BadChecksum,    // missing, malformed or wrong `*hh`
    Malformed,      // wrong field count or a field that does not parse
    Unsupported,    // a valid sentence of another type (GSV, VTG, ...)
};

struct GPSData {
    NmeaStatus status = NmeaStatus::Malformed;
    std::string sentence_type;   // "GGA", "RMC", "GLL" (talker id stripped)
    double latitude = 0.0;
    double longitude = 0.0;
    bool has_altitude = false;   // GGA only
    double altitude_m = 0.0;
    bool has_motion = false;     // RMC only (speed/course present)
    double speed_knots = 0.0;
    double course_deg = 0.0;
    int satellites = 0;          // GGA only
    int fix_quality = 0;         // GGA: 1 GPS, 2 DGPS, ...
    bool valid = false;          // status == Fix
};

class NMEAParser {
public:
    // One sentence, with or without its trailing CR/LF.
    GPSData parse(const std::string& sentence) const;

    // XOR of the bytes between '$' and '*' (exposed for testing).
    static unsigned char checksum(std::string_view body);
};

// Splits a serial byte stream into NMEA sentences. Partial lines are kept
// until their newline arrives; a line longer than kMaxLine (NMEA allows 82)
// is discarded, as is a stream that never sends a newline.
class NMEALineBuffer {
public:
    static constexpr size_t kMaxLine = 120;
    std::vector<std::string> feed(std::string_view bytes);
    void clear() { partial_.clear(); }

private:
    std::string partial_;
    bool discarding_ = false;
};

} // namespace skylink
