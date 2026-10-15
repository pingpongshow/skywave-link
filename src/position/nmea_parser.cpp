// NMEA Parser - GPS data parsing

#include "skywave_link/nmea_parser.hpp"
#include <sstream>
#include <cstdlib>

namespace skylink {

GPSData NMEAParser::parse(const std::string& sentence) {
    GPSData data;

    if (sentence.empty() || sentence[0] != '$') {
        return data;
    }

    // Verify checksum
    size_t star = sentence.find('*');
    if (star != std::string::npos) {
        int expected = std::strtol(sentence.substr(star + 1, 2).c_str(), nullptr, 16);
        int actual = 0;
        for (size_t i = 1; i < star; ++i) {
            actual ^= sentence[i];
        }
        if (actual != expected) {
            return data;  // Checksum failed
        }
    }

    // Split into fields
    std::vector<std::string> fields;
    std::stringstream ss(sentence.substr(0, star));
    std::string field;
    while (std::getline(ss, field, ',')) {
        fields.push_back(field);
    }

    if (fields.empty()) return data;

    // Parse based on sentence type
    if (fields[0].find("GGA") != std::string::npos) {
        parseGGA(fields, data);
    } else if (fields[0].find("RMC") != std::string::npos) {
        parseRMC(fields, data);
    } else if (fields[0].find("GLL") != std::string::npos) {
        parseGLL(fields, data);
    }

    return data;
}

void NMEAParser::parseGGA(const std::vector<std::string>& fields, GPSData& data) {
    // $GPGGA,hhmmss.ss,ddmm.mmmm,N,dddmm.mmmm,W,q,nn,h.h,a.a,M,g.g,M,t.t,xxxx*hh
    if (fields.size() < 15) return;

    // Quality indicator (0 = invalid)
    int quality = std::atoi(fields[6].c_str());
    if (quality == 0) return;

    // Latitude
    if (!fields[2].empty() && !fields[3].empty()) {
        data.latitude = parseLatLon(fields[2], fields[3][0]);
    }

    // Longitude
    if (!fields[4].empty() && !fields[5].empty()) {
        data.longitude = parseLatLon(fields[4], fields[5][0]);
    }

    // Satellites
    data.satellites = std::atoi(fields[7].c_str());

    // Altitude
    if (!fields[9].empty()) {
        data.altitude_m = std::atof(fields[9].c_str());
    }

    data.valid = true;
}

void NMEAParser::parseRMC(const std::vector<std::string>& fields, GPSData& data) {
    // $GPRMC,hhmmss.ss,A,ddmm.mmmm,N,dddmm.mmmm,W,k.k,c.c,ddmmyy,v.v,E*hh
    if (fields.size() < 12) return;

    // Status (A = valid, V = void)
    if (fields[2] != "A") return;

    // Latitude
    if (!fields[3].empty() && !fields[4].empty()) {
        data.latitude = parseLatLon(fields[3], fields[4][0]);
    }

    // Longitude
    if (!fields[5].empty() && !fields[6].empty()) {
        data.longitude = parseLatLon(fields[5], fields[6][0]);
    }

    // Speed (knots)
    if (!fields[7].empty()) {
        data.speed_knots = std::atof(fields[7].c_str());
    }

    // Course
    if (!fields[8].empty()) {
        data.course_deg = std::atof(fields[8].c_str());
    }

    data.valid = true;
}

void NMEAParser::parseGLL(const std::vector<std::string>& fields, GPSData& data) {
    // $GPGLL,ddmm.mmmm,N,dddmm.mmmm,W,hhmmss.ss,A*hh
    if (fields.size() < 7) return;

    // Status
    if (fields[6] != "A") return;

    // Latitude
    if (!fields[1].empty() && !fields[2].empty()) {
        data.latitude = parseLatLon(fields[1], fields[2][0]);
    }

    // Longitude
    if (!fields[3].empty() && !fields[4].empty()) {
        data.longitude = parseLatLon(fields[3], fields[4][0]);
    }

    data.valid = true;
}

double NMEAParser::parseLatLon(const std::string& value, char direction) {
    // Format: ddmm.mmmm or dddmm.mmmm
    if (value.empty()) return 0.0;

    size_t dot = value.find('.');
    if (dot == std::string::npos || dot < 2) return 0.0;

    // Degrees are everything except last 2 digits before decimal
    int deg_len = dot - 2;
    int degrees = std::atoi(value.substr(0, deg_len).c_str());
    double minutes = std::atof(value.substr(deg_len).c_str());

    double result = degrees + minutes / 60.0;

    if (direction == 'S' || direction == 'W') {
        result = -result;
    }

    return result;
}

} // namespace skylink
