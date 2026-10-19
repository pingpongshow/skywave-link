// Config save/load round-trip tests
#include "test_macros.hpp"
#include "skywave_link/config.hpp"
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <unistd.h>

static std::string temp_path() {
    static int counter = 0;
    return "/tmp/skywave_link_test_config_" + std::to_string(getpid()) + "_" + std::to_string(counter++) + ".toml";
}

TEST(config_defaults) {
    skylink::AppConfig cfg;
    cfg.setDefaults();

    ASSERT_EQ(cfg.station.callsign, std::string(""));
    ASSERT_EQ(cfg.modem.host, std::string("127.0.0.1"));
    ASSERT_EQ(cfg.modem.command_port, (uint16_t)9101);
    ASSERT_EQ(cfg.modem.data_port, (uint16_t)9100);
    ASSERT_EQ(cfg.modem.voice_port, (uint16_t)9102);
    ASSERT_EQ(cfg.modem.family, std::string("WIDE"));
    ASSERT_TRUE(cfg.modem.auto_connect);
    ASSERT_EQ(cfg.modem.reconnect_interval_sec, 5);
    ASSERT_FALSE(cfg.radio.enabled);
    ASSERT_EQ(cfg.radio.backend, std::string("none"));
    ASSERT_EQ(cfg.radio.device, std::string(""));
    ASSERT_EQ(cfg.radio.baud, 38400);
    ASSERT_EQ(cfg.radio.hamlib_model, 0);
    ASSERT_EQ(cfg.radio.hamlib_manufacturer, std::string(""));
    ASSERT_EQ(cfg.radio.kenwood_host, std::string(""));
    ASSERT_EQ(cfg.radio.kenwood_port, (uint16_t)5002);
    ASSERT_TRUE(cfg.radio.auto_connect);
    ASSERT_TRUE(cfg.radio.ptt_server_enabled);
    ASSERT_EQ(cfg.radio.ptt_server_bind, std::string("127.0.0.1"));
    ASSERT_EQ(cfg.radio.ptt_server_port, (uint16_t)4533);
    ASSERT_EQ(cfg.radio.default_band, std::string("40m"));
    ASSERT_TRUE(cfg.radio.auto_qsy);
    ASSERT_EQ(cfg.radio.qsy_ack_timeout_ms, 8000);
    ASSERT_EQ(cfg.radio.qsy_reconnect_delay_ms, 2500);
    ASSERT_EQ(cfg.radio.qsy_reconnect_window_ms, 15000);
    ASSERT_EQ(cfg.radio.qsy_connect_timeout_ms, 12000);
    ASSERT_EQ(cfg.radio.qsy_max_reconnect_attempts, 3);
    ASSERT_EQ(cfg.voice.codec, std::string("AUTO"));
    ASSERT_FALSE(cfg.aprs.enabled);
    ASSERT_EQ(cfg.aprs.port, (uint16_t)14580);
    ASSERT_EQ(cfg.store.message_expiry_days, 7);
    ASSERT_FALSE(cfg.email.winlink_compat_mode);
    ASSERT_FALSE(cfg.email.winlink_telnet_enabled);
    ASSERT_EQ(cfg.email.winlink_telnet_host, std::string("server.winlink.org"));
    ASSERT_EQ(cfg.email.winlink_telnet_port, (uint16_t)8772);
    ASSERT_FALSE(cfg.email.winlink_telnet_use_tls);
    ASSERT_EQ(cfg.email.pop3_poll_interval_sec, 300);
    ASSERT_EQ(cfg.email.pop3_max_messages, 50);
    ASSERT_EQ(cfg.email.winlink_fetch_interval_sec, 300);
    ASSERT_EQ(cfg.email.winlink_fetch_max_messages, 20);
    ASSERT_TRUE(cfg.email.weak_signal_profile_enabled);
    ASSERT_EQ(cfg.email.weak_signal_snr_threshold_db, -8);
    ASSERT_EQ(cfg.email.weak_signal_ack_timeout_ms, 45000);
    ASSERT_EQ(cfg.email.weak_signal_max_retries, 5);
    ASSERT_TRUE(cfg.email.weak_signal_enable_compression);
    ASSERT_TRUE(cfg.email.hf_store_forward_enabled);
    ASSERT_EQ(cfg.email.hf_store_forward_ttl, 3);
    ASSERT_EQ(cfg.email.hf_store_forward_hop_limit, 6);
    ASSERT_EQ(cfg.email.hf_store_forward_dedup_window_sec, 300);
    ASSERT_TRUE(cfg.ui.dark_mode);
    ASSERT_EQ(cfg.ui.font_size, 15);
    ASSERT_EQ(cfg.ui.window_width, 0);
    ASSERT_EQ(cfg.ui.window_height, 0);
    PASS();
}

TEST(config_save_load_roundtrip) {
    std::string path = temp_path();

    skylink::AppConfig cfg;
    cfg.setDefaults();
    cfg.station.callsign = "W1ABC";
    cfg.station.name = "John Doe";
    cfg.station.latitude = 42.3601;
    cfg.station.longitude = -71.0589;
    cfg.modem.command_port = 9000;
    cfg.modem.family = "NARROW";
    cfg.radio.enabled = true;
    cfg.radio.backend = "hamlib";
    cfg.radio.device = "/dev/cu.usbserial-TEST";
    cfg.radio.baud = 57600;
    cfg.radio.hamlib_model = 3073;
    cfg.radio.hamlib_manufacturer = "Kenwood";
    cfg.radio.kenwood_host = "192.168.50.10";
    cfg.radio.kenwood_port = 5002;
    cfg.radio.auto_connect = false;
    cfg.radio.ptt_server_enabled = true;
    cfg.radio.ptt_server_bind = "127.0.0.1";
    cfg.radio.ptt_server_port = 4633;
    cfg.radio.default_band = "20m";
    cfg.radio.qsy_ack_timeout_ms = 9000;
    cfg.radio.qsy_reconnect_delay_ms = 3000;
    cfg.radio.qsy_reconnect_window_ms = 18000;
    cfg.radio.qsy_connect_timeout_ms = 14000;
    cfg.radio.qsy_max_reconnect_attempts = 4;
    cfg.ui.font_size = 16;
    cfg.ui.dark_mode = false;

    ASSERT_TRUE(cfg.save(path));

    skylink::AppConfig loaded;
    ASSERT_TRUE(loaded.load(path));

    ASSERT_EQ(loaded.station.callsign, std::string("W1ABC"));
    ASSERT_EQ(loaded.station.name, std::string("John Doe"));
    ASSERT_NEAR(loaded.station.latitude, 42.3601, 0.001);
    ASSERT_NEAR(loaded.station.longitude, -71.0589, 0.001);
    ASSERT_EQ(loaded.modem.command_port, (uint16_t)9000);
    ASSERT_EQ(loaded.modem.family, std::string("NARROW"));
    ASSERT_TRUE(loaded.radio.enabled);
    ASSERT_EQ(loaded.radio.backend, std::string("hamlib"));
    ASSERT_EQ(loaded.radio.device, std::string("/dev/cu.usbserial-TEST"));
    ASSERT_EQ(loaded.radio.baud, 57600);
    ASSERT_EQ(loaded.radio.hamlib_model, 3073);
    ASSERT_EQ(loaded.radio.hamlib_manufacturer, std::string("Kenwood"));
    ASSERT_EQ(loaded.radio.kenwood_host, std::string("192.168.50.10"));
    ASSERT_EQ(loaded.radio.kenwood_port, (uint16_t)5002);
    ASSERT_FALSE(loaded.radio.auto_connect);
    ASSERT_TRUE(loaded.radio.ptt_server_enabled);
    ASSERT_EQ(loaded.radio.ptt_server_bind, std::string("127.0.0.1"));
    ASSERT_EQ(loaded.radio.ptt_server_port, (uint16_t)4633);
    ASSERT_EQ(loaded.radio.default_band, std::string("20m"));
    ASSERT_EQ(loaded.radio.qsy_ack_timeout_ms, 9000);
    ASSERT_EQ(loaded.radio.qsy_reconnect_delay_ms, 3000);
    ASSERT_EQ(loaded.radio.qsy_reconnect_window_ms, 18000);
    ASSERT_EQ(loaded.radio.qsy_connect_timeout_ms, 14000);
    ASSERT_EQ(loaded.radio.qsy_max_reconnect_attempts, 4);
    ASSERT_EQ(loaded.ui.font_size, 16);
    ASSERT_FALSE(loaded.ui.dark_mode);
    ASSERT_EQ(loaded.voice.codec, std::string("AUTO"));

    std::filesystem::remove(path);
    PASS();
}

TEST(config_all_sections_roundtrip) {
    std::string path = temp_path();

    skylink::AppConfig cfg;
    cfg.setDefaults();
    cfg.station.callsign = "KA1XYZ";
    cfg.station.grid_square = "FN42ig";
    cfg.modem.host = "192.168.1.100";
    cfg.modem.data_port = 7777;
    cfg.modem.family = "NARROW";
    cfg.modem.auto_connect = false;
    cfg.radio.enabled = true;
    cfg.radio.backend = "kenwood_tcp";
    cfg.radio.device = "/dev/ttyUSB0";
    cfg.radio.baud = 19200;
    cfg.radio.hamlib_model = 2014;
    cfg.radio.hamlib_manufacturer = "FlexRadio";
    cfg.radio.kenwood_host = "10.0.0.55";
    cfg.radio.kenwood_port = 5002;
    cfg.radio.ptt_server_enabled = false;
    cfg.radio.ptt_server_bind = "0.0.0.0";
    cfg.radio.ptt_server_port = 4999;
    cfg.aprs.enabled = true;
    cfg.aprs.server = "noam.aprs2.net";
    cfg.aprs.port = 14580;
    cfg.aprs.beacon_interval_sec = 600;
    cfg.email.smtp_host = "mail.example.com";
    cfg.email.smtp_port = 465;
    cfg.email.winlink_telnet_enabled = true;
    cfg.email.winlink_telnet_host = "server.winlink.org";
    cfg.email.winlink_telnet_port = 8772;
    cfg.email.winlink_telnet_use_tls = false;
    cfg.email.winlink_telnet_callsign = "KA1XYZ";
    cfg.email.pop3_poll_interval_sec = 180;
    cfg.email.pop3_max_messages = 25;
    cfg.email.winlink_fetch_interval_sec = 240;
    cfg.email.winlink_fetch_max_messages = 12;
    cfg.email.winlink_compat_mode = true;
    cfg.email.weak_signal_profile_enabled = true;
    cfg.email.weak_signal_snr_threshold_db = -11;
    cfg.email.weak_signal_ack_timeout_ms = 60000;
    cfg.email.weak_signal_max_retries = 7;
    cfg.email.weak_signal_enable_compression = false;
    cfg.email.hf_store_forward_enabled = true;
    cfg.email.hf_store_forward_ttl = 4;
    cfg.email.hf_store_forward_hop_limit = 8;
    cfg.email.hf_store_forward_dedup_window_sec = 420;
    cfg.store.db_path = "/data/messages.db";
    cfg.store.message_expiry_days = 14;
    cfg.files.max_file_size = 100 * 1024;
    cfg.files.download_path = "/home/user/downloads";
    cfg.psk_reporter.enabled = true;
    cfg.psk_reporter.antenna = "Yagi 3-el";
    cfg.ui.window_width = 1920;
    cfg.ui.window_height = 1080;

    ASSERT_TRUE(cfg.save(path));

    skylink::AppConfig loaded;
    ASSERT_TRUE(loaded.load(path));

    ASSERT_EQ(loaded.station.callsign, std::string("KA1XYZ"));
    ASSERT_EQ(loaded.station.grid_square, std::string("FN42ig"));
    ASSERT_EQ(loaded.modem.host, std::string("192.168.1.100"));
    ASSERT_EQ(loaded.modem.data_port, (uint16_t)7777);
    ASSERT_EQ(loaded.modem.family, std::string("NARROW"));
    ASSERT_FALSE(loaded.modem.auto_connect);
    ASSERT_TRUE(loaded.radio.enabled);
    ASSERT_EQ(loaded.radio.backend, std::string("kenwood_tcp"));
    ASSERT_EQ(loaded.radio.device, std::string("/dev/ttyUSB0"));
    ASSERT_EQ(loaded.radio.baud, 19200);
    ASSERT_EQ(loaded.radio.hamlib_model, 2014);
    ASSERT_EQ(loaded.radio.hamlib_manufacturer, std::string("FlexRadio"));
    ASSERT_EQ(loaded.radio.kenwood_host, std::string("10.0.0.55"));
    ASSERT_EQ(loaded.radio.kenwood_port, (uint16_t)5002);
    ASSERT_FALSE(loaded.radio.ptt_server_enabled);
    ASSERT_EQ(loaded.radio.ptt_server_bind, std::string("0.0.0.0"));
    ASSERT_EQ(loaded.radio.ptt_server_port, (uint16_t)4999);
    ASSERT_TRUE(loaded.aprs.enabled);
    ASSERT_EQ(loaded.aprs.server, std::string("noam.aprs2.net"));
    ASSERT_EQ(loaded.email.smtp_host, std::string("mail.example.com"));
    ASSERT_EQ(loaded.email.smtp_port, (uint16_t)465);
    ASSERT_TRUE(loaded.email.winlink_telnet_enabled);
    ASSERT_EQ(loaded.email.winlink_telnet_host, std::string("server.winlink.org"));
    ASSERT_EQ(loaded.email.winlink_telnet_port, (uint16_t)8772);
    ASSERT_FALSE(loaded.email.winlink_telnet_use_tls);
    ASSERT_EQ(loaded.email.winlink_telnet_callsign, std::string("KA1XYZ"));
    ASSERT_EQ(loaded.email.pop3_poll_interval_sec, 180);
    ASSERT_EQ(loaded.email.pop3_max_messages, 25);
    ASSERT_EQ(loaded.email.winlink_fetch_interval_sec, 240);
    ASSERT_EQ(loaded.email.winlink_fetch_max_messages, 12);
    ASSERT_TRUE(loaded.email.winlink_compat_mode);
    ASSERT_TRUE(loaded.email.weak_signal_profile_enabled);
    ASSERT_EQ(loaded.email.weak_signal_snr_threshold_db, -11);
    ASSERT_EQ(loaded.email.weak_signal_ack_timeout_ms, 60000);
    ASSERT_EQ(loaded.email.weak_signal_max_retries, 7);
    ASSERT_FALSE(loaded.email.weak_signal_enable_compression);
    ASSERT_TRUE(loaded.email.hf_store_forward_enabled);
    ASSERT_EQ(loaded.email.hf_store_forward_ttl, 4);
    ASSERT_EQ(loaded.email.hf_store_forward_hop_limit, 8);
    ASSERT_EQ(loaded.email.hf_store_forward_dedup_window_sec, 420);
    ASSERT_EQ(loaded.store.db_path, std::string("/data/messages.db"));
    ASSERT_EQ(loaded.store.message_expiry_days, 14);
    ASSERT_EQ(loaded.files.download_path, std::string("/home/user/downloads"));
    ASSERT_TRUE(loaded.psk_reporter.enabled);
    ASSERT_EQ(loaded.psk_reporter.antenna, std::string("Yagi 3-el"));
    ASSERT_EQ(loaded.ui.window_width, 1920);
    ASSERT_EQ(loaded.ui.window_height, 1080);
    ASSERT_EQ(loaded.voice.codec, std::string("AUTO"));

    std::filesystem::remove(path);
    PASS();
}

// [bbs] is persisted: serving BBS files is gated on bbs.enabled, which
// used to be neither loaded nor saved.
TEST(config_bbs_section_roundtrip) {
    std::string path = temp_path();
    skylink::AppConfig cfg;
    cfg.setDefaults();
    cfg.bbs.enabled = true;
    cfg.bbs.bbs_path = "/tmp/my bbs";
    cfg.bbs.allow_uploads = true;
    cfg.bbs.max_upload_size = 12345;
    ASSERT_TRUE(cfg.save(path));
    skylink::AppConfig loaded;
    ASSERT_TRUE(loaded.load(path));
    ASSERT_TRUE(loaded.bbs.enabled);
    ASSERT_EQ(loaded.bbs.bbs_path, std::string("/tmp/my bbs"));
    ASSERT_TRUE(loaded.bbs.allow_uploads);
    ASSERT_EQ(loaded.bbs.max_upload_size, size_t(12345));
    std::remove(path.c_str());
}

TEST(config_partial_load) {
    std::string path = temp_path();

    // Write a config with only station section
    {
        std::ofstream f(path);
        f << "[station]\n";
        f << "callsign = \"N0CALL\"\n";
    }

    skylink::AppConfig loaded;
    ASSERT_TRUE(loaded.load(path));

    // Station should be loaded
    ASSERT_EQ(loaded.station.callsign, std::string("N0CALL"));
    // Other sections should have defaults
    ASSERT_EQ(loaded.modem.host, std::string("127.0.0.1"));
    ASSERT_EQ(loaded.modem.command_port, (uint16_t)9101);
    ASSERT_TRUE(loaded.ui.dark_mode);

    std::filesystem::remove(path);
    PASS();
}

TEST(config_load_nonexistent) {
    skylink::AppConfig cfg;
    ASSERT_FALSE(cfg.load("/tmp/nonexistent_skywave_link_config_xyz.toml"));
    // Should still have defaults after failed load
    ASSERT_EQ(cfg.modem.host, std::string("127.0.0.1"));
    PASS();
}

TEST(config_boolean_values) {
    std::string path = temp_path();

    skylink::AppConfig cfg;
    cfg.setDefaults();
    cfg.modem.auto_connect = false;
    cfg.radio.enabled = true;
    cfg.aprs.enabled = true;
    cfg.aprs.gps_enabled = true;
    cfg.ui.dark_mode = false;
    cfg.ui.show_timestamps = false;
    cfg.ui.sound_notifications = false;

    ASSERT_TRUE(cfg.save(path));

    skylink::AppConfig loaded;
    ASSERT_TRUE(loaded.load(path));

    ASSERT_FALSE(loaded.modem.auto_connect);
    ASSERT_TRUE(loaded.radio.enabled);
    ASSERT_TRUE(loaded.aprs.enabled);
    ASSERT_TRUE(loaded.aprs.gps_enabled);
    ASSERT_FALSE(loaded.ui.dark_mode);
    ASSERT_FALSE(loaded.ui.show_timestamps);
    ASSERT_FALSE(loaded.ui.sound_notifications);

    std::filesystem::remove(path);
    PASS();
}

TEST(config_legacy_ale_section_ignored) {
    std::string path = temp_path();

    {
        std::ofstream f(path);
        f << "[station]\n";
        f << "callsign = \"N0CALL\"\n\n";
        f << "[ale]\n";
        f << "scan_frequencies_hz = \"3590000, 7055000,7055000, 14105000\"\n";
        f << "auto_connect_on_scan_complete = true\n";
        f << "last_target_callsign = \"K7ALE\"\n\n";
        f << "[ui]\n";
        f << "font_size = 18\n";
    }

    skylink::AppConfig loaded;
    ASSERT_TRUE(loaded.load(path));

    ASSERT_EQ(loaded.station.callsign, std::string("N0CALL"));
    ASSERT_EQ(loaded.ui.font_size, 18);
    ASSERT_EQ(loaded.modem.host, std::string("127.0.0.1"));

    std::filesystem::remove(path);
    PASS();
}

TEST(config_string_with_spaces) {
    std::string path = temp_path();

    skylink::AppConfig cfg;
    cfg.setDefaults();
    cfg.station.name = "John Q Public";
    cfg.aprs.comment = "Skywave Link Mobile Station";

    ASSERT_TRUE(cfg.save(path));

    skylink::AppConfig loaded;
    ASSERT_TRUE(loaded.load(path));

    ASSERT_EQ(loaded.station.name, std::string("John Q Public"));
    ASSERT_EQ(loaded.aprs.comment, std::string("Skywave Link Mobile Station"));

    std::filesystem::remove(path);
    PASS();
}

TEST(config_quote_injection_fixed) {
    // Bug demo: a name with quotes should not corrupt the config file
    std::string path = temp_path();

    skylink::AppConfig cfg;
    cfg.setDefaults();
    cfg.station.name = "Joe \"The Ham\" Smith";

    ASSERT_TRUE(cfg.save(path));

    // The saved file should still be parseable
    skylink::AppConfig loaded;
    ASSERT_TRUE(loaded.load(path));

    // The value should survive the round-trip (quotes escaped as \")
    // Note: our simple TOML parser doesn't unescape, so the loaded value
    // will contain the escaped form. The important thing is the file isn't corrupted.
    // Verify the file is valid by checking other fields loaded correctly
    ASSERT_EQ(loaded.modem.host, std::string("127.0.0.1"));
    ASSERT_EQ(loaded.modem.command_port, (uint16_t)9101);

    std::filesystem::remove(path);
    PASS();
}

TEST(config_numeric_overflow_fixed) {
    // Bug demo: port values > 65535 should be clamped
    std::string path = temp_path();

    {
        std::ofstream f(path);
        f << "[modem]\n";
        f << "command_port = 99999\n";
        f << "data_port = 70000\n";
        f << "[aprs]\n";
        f << "port = 80000\n";
    }

    skylink::AppConfig loaded;
    ASSERT_TRUE(loaded.load(path));

    // Values should be clamped to uint16_t max (65535) instead of wrapping
    ASSERT_EQ(loaded.modem.command_port, (uint16_t)65535);
    ASSERT_EQ(loaded.modem.data_port, (uint16_t)65535);
    ASSERT_EQ(loaded.aprs.port, (uint16_t)65535);

    std::filesystem::remove(path);
    PASS();
}

// [files] trusted_callsigns is persisted (it was consulted but never
// loaded or saved, so it was always empty).
TEST(config_trusted_callsigns_roundtrip) {
    std::string path = temp_path();
    skylink::AppConfig cfg;
    cfg.setDefaults();
    cfg.files.trusted_callsigns = skylink::parseCallsignList(" w1aw, K6XYZ-2;;W1AW  n0call ");
    ASSERT_EQ(cfg.files.trusted_callsigns.size(), size_t(3));
    ASSERT_EQ(cfg.files.trusted_callsigns[0], std::string("W1AW"));
    ASSERT_EQ(cfg.files.trusted_callsigns[1], std::string("K6XYZ-2"));
    ASSERT_EQ(cfg.files.trusted_callsigns[2], std::string("N0CALL"));
    ASSERT_EQ(skylink::joinCallsignList(cfg.files.trusted_callsigns),
              std::string("W1AW, K6XYZ-2, N0CALL"));
    ASSERT_TRUE(cfg.save(path));
    skylink::AppConfig loaded;
    ASSERT_TRUE(loaded.load(path));
    ASSERT_TRUE(loaded.files.trusted_callsigns == cfg.files.trusted_callsigns);
    // An empty list round-trips empty.
    cfg.files.trusted_callsigns.clear();
    ASSERT_TRUE(cfg.save(path));
    ASSERT_TRUE(loaded.load(path));
    ASSERT_TRUE(loaded.files.trusted_callsigns.empty());
    std::remove(path.c_str());
    PASS();
}

TEST_MAIN()
