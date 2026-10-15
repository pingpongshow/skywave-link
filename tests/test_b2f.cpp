// B2F Message Codec tests
#include "test_macros.hpp"
#include "skywave_link/b2f.hpp"

TEST(b2f_encode_decode_basic) {
    skylink::B2FCodec codec;

    skylink::B2FHeader header;
    header.message_id = "W1ABC_001";
    header.type = skylink::B2FMessageType::Private;
    header.from_callsign = "W1ABC";
    header.to_callsign = "K2DEF";
    header.subject = "Test Message";
    header.date_time = "2024-01-15 12:00:00";

    std::string body = "Hello from W1ABC!";
    std::vector<std::pair<std::string, std::vector<uint8_t>>> attachments;

    auto encoded = codec.encode(header, body, attachments);
    ASSERT_FALSE(encoded.empty());

    skylink::B2FHeader dec_header;
    std::string dec_body;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> dec_attachments;

    ASSERT_TRUE(codec.decode(encoded, dec_header, dec_body, dec_attachments));
    ASSERT_EQ(dec_header.message_id, std::string("W1ABC_001"));
    ASSERT_EQ(dec_header.from_callsign, std::string("W1ABC"));
    ASSERT_EQ(dec_header.to_callsign, std::string("K2DEF"));
    ASSERT_EQ(dec_header.subject, std::string("Test Message"));
    ASSERT_EQ(dec_body, body);
    ASSERT_TRUE(dec_attachments.empty());
    PASS();
}

TEST(b2f_with_addresses) {
    skylink::B2FCodec codec;

    skylink::B2FHeader header;
    header.message_id = "W1ABC_002";
    header.from_callsign = "W1ABC";
    header.from_address = "winlink.org";
    header.to_callsign = "K2DEF";
    header.to_address = "ampr.net";
    header.subject = "Email via radio";
    header.date_time = "2024-01-15 13:00:00";

    auto encoded = codec.encode(header, "Test body", {});

    skylink::B2FHeader dec_header;
    std::string dec_body;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> dec_att;

    ASSERT_TRUE(codec.decode(encoded, dec_header, dec_body, dec_att));
    ASSERT_EQ(dec_header.from_callsign, std::string("W1ABC"));
    ASSERT_EQ(dec_header.from_address, std::string("winlink.org"));
    ASSERT_EQ(dec_header.to_callsign, std::string("K2DEF"));
    ASSERT_EQ(dec_header.to_address, std::string("ampr.net"));
    PASS();
}

TEST(b2f_with_attachments) {
    skylink::B2FCodec codec;

    skylink::B2FHeader header;
    header.message_id = "W1ABC_003";
    header.from_callsign = "W1ABC";
    header.to_callsign = "K2DEF";
    header.subject = "File transfer";
    header.date_time = "2024-01-15 14:00:00";

    std::string body = "See attached file.";
    std::vector<uint8_t> file_data = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::pair<std::string, std::vector<uint8_t>>> attachments;
    attachments.push_back({"test.png", file_data});

    auto encoded = codec.encode(header, body, attachments);

    skylink::B2FHeader dec_header;
    std::string dec_body;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> dec_att;

    ASSERT_TRUE(codec.decode(encoded, dec_header, dec_body, dec_att));
    ASSERT_EQ(dec_body, body);
    ASSERT_EQ(dec_att.size(), (size_t)1);
    ASSERT_EQ(dec_att[0].first, std::string("test.png"));
    ASSERT_EQ(dec_att[0].second.size(), file_data.size());
    for (size_t i = 0; i < file_data.size(); i++) {
        ASSERT_EQ(dec_att[0].second[i], file_data[i]);
    }
    PASS();
}

TEST(b2f_message_types) {
    skylink::B2FCodec codec;

    for (auto type : {skylink::B2FMessageType::Private,
                      skylink::B2FMessageType::Bulletin,
                      skylink::B2FMessageType::Traffic}) {
        skylink::B2FHeader header;
        header.message_id = "TEST_TYPE";
        header.type = type;
        header.from_callsign = "W1ABC";
        header.to_callsign = "K2DEF";
        header.subject = "Type test";
        header.date_time = "2024-01-15";

        auto encoded = codec.encode(header, "body", {});

        skylink::B2FHeader dec_header;
        std::string dec_body;
        std::vector<std::pair<std::string, std::vector<uint8_t>>> dec_att;

        ASSERT_TRUE(codec.decode(encoded, dec_header, dec_body, dec_att));
        ASSERT_EQ(static_cast<char>(dec_header.type), static_cast<char>(type));
    }
    PASS();
}

TEST(b2f_empty_body) {
    skylink::B2FCodec codec;

    skylink::B2FHeader header;
    header.message_id = "EMPTY_001";
    header.from_callsign = "W1ABC";
    header.to_callsign = "K2DEF";
    header.subject = "Empty";
    header.date_time = "2024-01-15";

    auto encoded = codec.encode(header, "", {});

    skylink::B2FHeader dec_header;
    std::string dec_body;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> dec_att;

    ASSERT_TRUE(codec.decode(encoded, dec_header, dec_body, dec_att));
    ASSERT_TRUE(dec_body.empty());
    PASS();
}

TEST(b2f_malformed_no_header_end) {
    skylink::B2FCodec codec;
    // Data without \r\n\r\n separator
    std::string bad = "MID: test\r\nFrom: W1ABC\r\nno blank line here";
    std::vector<uint8_t> data(bad.begin(), bad.end());

    skylink::B2FHeader dec_header;
    std::string dec_body;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> dec_att;

    ASSERT_FALSE(codec.decode(data, dec_header, dec_body, dec_att));
    PASS();
}

TEST(b2f_decode_empty) {
    skylink::B2FCodec codec;
    std::vector<uint8_t> empty;

    skylink::B2FHeader dec_header;
    std::string dec_body;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> dec_att;

    ASSERT_FALSE(codec.decode(empty, dec_header, dec_body, dec_att));
    PASS();
}

TEST_MAIN()
