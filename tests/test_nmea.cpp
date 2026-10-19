// NMEAParser tests: standard sentences (including GGA's trailing empty
// fields, which the old getline split dropped), checksum enforcement,
// no-fix and malformed input, and the serial line splitter.

#include "test_macros.hpp"
#include "skywave_link/nmea_parser.hpp"

#include <cmath>
#include <cstdio>
#include <string>

using namespace skylink;

namespace {

bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

// "$<body>*hh" with the checksum computed.
std::string sentence(const std::string& body) {
    char hex[4];
    std::snprintf(hex, sizeof(hex), "%02X", NMEAParser::checksum(body));
    return "$" + body + "*" + hex;
}

} // namespace

TEST(gga_standard_sentence_with_trailing_empty_fields) {
    // The textbook GGA: the DGPS age/station fields at the end are empty.
    NMEAParser p;
    const GPSData d = p.parse("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n");
    ASSERT_TRUE(d.valid);
    ASSERT_TRUE(d.status == NmeaStatus::Fix);
    ASSERT_EQ(d.sentence_type, std::string("GGA"));
    ASSERT_TRUE(near(d.latitude, 48.0 + 7.038 / 60.0));
    ASSERT_TRUE(near(d.longitude, 11.0 + 31.0 / 60.0));
    ASSERT_EQ(d.satellites, 8);
    ASSERT_EQ(d.fix_quality, 1);
    ASSERT_TRUE(d.has_altitude);
    ASSERT_TRUE(near(d.altitude_m, 545.4));
    PASS();
}

TEST(rmc_standard_sentence) {
    NMEAParser p;
    const GPSData d = p.parse("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A");
    ASSERT_TRUE(d.valid);
    ASSERT_EQ(d.sentence_type, std::string("RMC"));
    ASSERT_TRUE(near(d.latitude, 48.0 + 7.038 / 60.0));
    ASSERT_TRUE(d.has_motion);
    ASSERT_TRUE(near(d.speed_knots, 22.4));
    ASSERT_TRUE(near(d.course_deg, 84.4));
    ASSERT_FALSE(d.has_altitude);
    PASS();
}

TEST(rmc_nmea23_mode_field_and_south_west) {
    NMEAParser p;
    // GNSS talker, mode indicator present, empty magnetic variation.
    GPSData d = p.parse(sentence("GNRMC,001031.00,A,3352.12345,S,15112.54321,W,0.0,,010125,,,D"));
    ASSERT_TRUE(d.valid);
    ASSERT_TRUE(d.latitude < -33.86 && d.latitude > -33.87);
    ASSERT_TRUE(d.longitude < -151.20 && d.longitude > -151.21);
    ASSERT_FALSE(d.has_motion);  // course empty
    // Mode N = not valid, whatever the status says.
    d = p.parse(sentence("GNRMC,001031.00,A,3352.12345,S,15112.54321,W,0.0,,010125,,,N"));
    ASSERT_FALSE(d.valid);
    ASSERT_TRUE(d.status == NmeaStatus::NoFix);
    PASS();
}

TEST(gll_sentence) {
    NMEAParser p;
    const GPSData d = p.parse("$GPGLL,4916.45,N,12311.12,W,225444,A,*1D");
    ASSERT_TRUE(d.valid);
    ASSERT_EQ(d.sentence_type, std::string("GLL"));
    ASSERT_TRUE(near(d.latitude, 49.0 + 16.45 / 60.0));
    ASSERT_TRUE(near(d.longitude, -(123.0 + 11.12 / 60.0)));
    PASS();
}

TEST(no_fix_sentences) {
    NMEAParser p;
    GPSData d = p.parse(sentence("GPGGA,123519,,,,,0,00,99.9,,M,,M,,"));
    ASSERT_FALSE(d.valid);
    ASSERT_TRUE(d.status == NmeaStatus::NoFix);
    d = p.parse(sentence("GPRMC,123519,V,,,,,,,230394,,,N"));
    ASSERT_FALSE(d.valid);
    ASSERT_TRUE(d.status == NmeaStatus::NoFix);
    d = p.parse(sentence("GPGLL,,,,,225444,V,N"));
    ASSERT_TRUE(d.status == NmeaStatus::NoFix);
    PASS();
}

TEST(checksum_enforced) {
    NMEAParser p;
    // Wrong checksum.
    GPSData d = p.parse("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48");
    ASSERT_TRUE(d.status == NmeaStatus::BadChecksum);
    ASSERT_FALSE(d.valid);
    // Missing checksum.
    d = p.parse("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
    ASSERT_TRUE(d.status == NmeaStatus::BadChecksum);
    // Non-hex checksum digits / junk after them.
    d = p.parse("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*4G");
    ASSERT_TRUE(d.status == NmeaStatus::BadChecksum);
    d = p.parse("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47X");
    ASSERT_TRUE(d.status == NmeaStatus::BadChecksum);
    // A flipped digit in the body.
    d = p.parse("$GPGGA,123519,4807.039,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47");
    ASSERT_TRUE(d.status == NmeaStatus::BadChecksum);
    PASS();
}

TEST(malformed_fields_rejected) {
    NMEAParser p;
    // Minutes >= 60, letters in a number, wrong hemisphere, too few fields.
    ASSERT_TRUE(p.parse(sentence("GPGGA,123519,4867.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"))
                    .status == NmeaStatus::Malformed);
    ASSERT_TRUE(p.parse(sentence("GPGGA,123519,48O7.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"))
                    .status == NmeaStatus::Malformed);
    ASSERT_TRUE(p.parse(sentence("GPGGA,123519,4807.038,E,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"))
                    .status == NmeaStatus::Malformed);
    ASSERT_TRUE(p.parse(sentence("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M"))
                    .status == NmeaStatus::Malformed);
    ASSERT_TRUE(p.parse(sentence("GPGGA,123519,4807.038,N,1131.000,E,1,08,0.9,545.4,M,46.9,M,,"))
                    .status == NmeaStatus::Malformed);  // 2-digit longitude degrees
    ASSERT_FALSE(p.parse("").valid);
    ASSERT_FALSE(p.parse("GPGGA,no dollar*00").valid);
    // A valid sentence of another type.
    ASSERT_TRUE(p.parse(sentence("GPVTG,054.7,T,034.4,M,005.5,N,010.2,K,A")).status ==
                NmeaStatus::Unsupported);
    PASS();
}

TEST(line_buffer_splits_stream) {
    NMEALineBuffer buf;
    auto lines = buf.feed("$GPGLL,4916.45,N,12311.12,W,225444,A,*1D\r\n$GPRMC,1");
    ASSERT_EQ(lines.size(), size_t(1));
    ASSERT_EQ(lines[0], std::string("$GPGLL,4916.45,N,12311.12,W,225444,A,*1D"));
    lines = buf.feed("23519,A\r\n");
    ASSERT_EQ(lines.size(), size_t(1));
    ASSERT_EQ(lines[0], std::string("$GPRMC,123519,A"));
    // Garbage before '$' is dropped; an endless line is discarded whole.
    lines = buf.feed("xx$GPGGA,1\n");
    ASSERT_EQ(lines[0], std::string("$GPGGA,1"));
    lines = buf.feed(std::string(500, 'A') + "\n$GPGLL\n");
    ASSERT_EQ(lines.size(), size_t(1));
    ASSERT_EQ(lines[0], std::string("$GPGLL"));
    PASS();
}

TEST_MAIN()
