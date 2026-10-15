// NMEA Parser - GPS data parsing

#pragma once

#include <string>
#include <vector>

namespace skylink {

struct GPSData {
    double latitude = 0.0;
    double longitude = 0.0;
    double altitude_m = 0.0;
    double speed_knots = 0.0;
    double course_deg = 0.0;
    int satellites = 0;
    bool valid = false;
};

class NMEAParser {
public:
    GPSData parse(const std::string& sentence);

private:
    void parseGGA(const std::vector<std::string>& fields, GPSData& data);
    void parseRMC(const std::vector<std::string>& fields, GPSData& data);
    void parseGLL(const std::vector<std::string>& fields, GPSData& data);
    double parseLatLon(const std::string& value, char direction);
};

} // namespace skylink
