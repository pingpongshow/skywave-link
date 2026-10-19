// NMEA 0183 parser - GGA, RMC and GLL with checksum validation.

#include "skywave_link/nmea_parser.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>

namespace skylink {

namespace {

// Split on ',' keeping empty fields, including trailing ones
// ("a,b," -> {"a", "b", ""}); std::getline dropped the last empty field,
// which made a standard GGA one field short.
std::vector<std::string> splitFields(std::string_view s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t comma = s.find(',', start);
        if (comma == std::string_view::npos) {
            out.emplace_back(s.substr(start));
            break;
        }
        out.emplace_back(s.substr(start, comma - start));
        start = comma + 1;
    }
    return out;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// A strictly numeric field: digits with at most one '.', optional sign.
bool parseNumber(const std::string& field, double* out) {
    if (field.empty()) return false;
    size_t i = (field[0] == '-' || field[0] == '+') ? 1 : 0;
    bool digit = false, dot = false;
    for (; i < field.size(); ++i) {
        const char c = field[i];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            digit = true;
        } else if (c == '.' && !dot) {
            dot = true;
        } else {
            return false;
        }
    }
    if (!digit) return false;
    errno = 0;
    char* end = nullptr;
    const double v = std::strtod(field.c_str(), &end);
    if (errno != 0 || end != field.c_str() + field.size()) return false;
    *out = v;
    return true;
}

bool parseInt(const std::string& field, int* out) {
    if (field.empty() || field.size() > 6) return false;
    for (char c : field) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    *out = std::atoi(field.c_str());
    return true;
}

// "ddmm.mmmm" / "dddmm.mmmm" plus hemisphere -> signed decimal degrees.
bool parseLatLon(const std::string& value, const std::string& hemi, int deg_digits,
                 char pos, char neg, double max_deg, double* out) {
    if (hemi.size() != 1 || (hemi[0] != pos && hemi[0] != neg)) return false;
    const size_t dot = value.find('.');
    const size_t int_len = dot == std::string::npos ? value.size() : dot;
    if (int_len != static_cast<size_t>(deg_digits) + 2) return false;
    double raw = 0.0;
    if (!parseNumber(value, &raw) || raw < 0.0) return false;
    const double deg = static_cast<double>(static_cast<int>(raw / 100.0));
    const double min = raw - deg * 100.0;
    if (min >= 60.0) return false;
    const double result = deg + min / 60.0;
    if (result > max_deg) return false;
    *out = (hemi[0] == neg) ? -result : result;
    return true;
}

bool parsePosition(const std::vector<std::string>& f, size_t lat_at, GPSData& data) {
    return parseLatLon(f[lat_at], f[lat_at + 1], 2, 'N', 'S', 90.0, &data.latitude) &&
           parseLatLon(f[lat_at + 2], f[lat_at + 3], 3, 'E', 'W', 180.0, &data.longitude);
}

// $--GGA,time,lat,N,lon,W,quality,sats,hdop,alt,M,geoid,M,age,station
void parseGGA(const std::vector<std::string>& f, GPSData& data) {
    if (f.size() != 15) return;  // Malformed
    int quality = 0;
    if (!parseInt(f[6], &quality)) return;
    data.fix_quality = quality;
    if (quality == 0) {
        data.status = NmeaStatus::NoFix;
        return;
    }
    if (!parsePosition(f, 2, data)) return;
    int sats = 0;
    if (!f[7].empty() && parseInt(f[7], &sats)) data.satellites = sats;
    double alt = 0.0;
    if (!f[9].empty() && parseNumber(f[9], &alt)) {
        data.altitude_m = alt;
        data.has_altitude = true;
    }
    data.status = NmeaStatus::Fix;
}

// $--RMC,time,status,lat,N,lon,W,speed,course,date,magvar,E[,mode[,navstatus]]
void parseRMC(const std::vector<std::string>& f, GPSData& data) {
    if (f.size() < 12 || f.size() > 14) return;
    if (f[2] == "V") {
        data.status = NmeaStatus::NoFix;
        return;
    }
    if (f[2] != "A") return;
    // NMEA 2.3+: mode 'N' (not valid) overrides an 'A' status.
    if (f.size() >= 13 && f[12] == "N") {
        data.status = NmeaStatus::NoFix;
        return;
    }
    if (!parsePosition(f, 3, data)) return;
    double speed = 0.0, course = 0.0;
    const bool has_speed = !f[7].empty() && parseNumber(f[7], &speed);
    const bool has_course = !f[8].empty() && parseNumber(f[8], &course);
    if (has_speed) data.speed_knots = speed;
    if (has_course) data.course_deg = course;
    data.has_motion = has_speed && has_course;
    data.status = NmeaStatus::Fix;
}

// $--GLL,lat,N,lon,W,time,status[,mode]
void parseGLL(const std::vector<std::string>& f, GPSData& data) {
    if (f.size() != 7 && f.size() != 8) return;
    if (f[6] == "V" || (f.size() == 8 && f[7] == "N")) {
        data.status = NmeaStatus::NoFix;
        return;
    }
    if (f[6] != "A") return;
    if (!parsePosition(f, 1, data)) return;
    data.status = NmeaStatus::Fix;
}

} // namespace

unsigned char NMEAParser::checksum(std::string_view body) {
    unsigned char sum = 0;
    for (char c : body) sum ^= static_cast<unsigned char>(c);
    return sum;
}

GPSData NMEAParser::parse(const std::string& raw) const {
    GPSData data;
    std::string_view s(raw);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    if (s.size() < 7 || s[0] != '$') return data;

    // Checksum is mandatory: "*hh" at the very end.
    const size_t star = s.rfind('*');
    if (star == std::string_view::npos || star + 3 != s.size()) {
        data.status = NmeaStatus::BadChecksum;
        return data;
    }
    const int hi = hexValue(s[star + 1]);
    const int lo = hexValue(s[star + 2]);
    const std::string_view body = s.substr(1, star - 1);
    if (hi < 0 || lo < 0 || checksum(body) != static_cast<unsigned char>(hi * 16 + lo)) {
        data.status = NmeaStatus::BadChecksum;
        return data;
    }
    for (char c : body) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc > 0x7E) return data;  // Malformed
    }

    const std::vector<std::string> fields = splitFields(body);
    // Address field: 2-char talker (GP, GN, GL, GA, BD, ...) + 3-char type.
    if (fields[0].size() != 5) return data;
    data.sentence_type = fields[0].substr(2);
    if (data.sentence_type == "GGA") {
        parseGGA(fields, data);
    } else if (data.sentence_type == "RMC") {
        parseRMC(fields, data);
    } else if (data.sentence_type == "GLL") {
        parseGLL(fields, data);
    } else {
        data.status = NmeaStatus::Unsupported;
    }
    data.valid = (data.status == NmeaStatus::Fix);
    if (!data.valid) {
        data.latitude = data.longitude = 0.0;
    }
    return data;
}

std::vector<std::string> NMEALineBuffer::feed(std::string_view bytes) {
    std::vector<std::string> lines;
    for (char c : bytes) {
        if (c == '\n' || c == '\r') {
            if (!discarding_ && !partial_.empty()) lines.push_back(partial_);
            partial_.clear();
            discarding_ = false;
            continue;
        }
        if (discarding_) continue;
        if (c == '$') partial_.clear();  // a new sentence starts: resynchronise
        partial_.push_back(c);
        if (partial_.size() > kMaxLine) {
            partial_.clear();
            discarding_ = true;
        }
    }
    return lines;
}

} // namespace skylink
