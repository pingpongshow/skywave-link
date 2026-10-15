// POP3 client tests - message parsing, dot-unstuffing, header folding

#include "test_macros.hpp"
#include "skywave_link/pop3_client.hpp"

using namespace skylink;

TEST(parse_simple_message) {
    std::string raw =
        "From: user@example.com\r\n"
        "To: dest@example.com\r\n"
        "Subject: Test\r\n"
        "\r\n"
        "Hello, world!";

    auto msg = parseEmailMessage(raw);
    ASSERT_EQ(msg.from, std::string("user@example.com"));
    ASSERT_EQ(msg.to, std::string("dest@example.com"));
    ASSERT_EQ(msg.subject, std::string("Test"));
    ASSERT_EQ(msg.body, std::string("Hello, world!"));
    PASS();
}

TEST(parse_lf_only) {
    std::string raw =
        "From: alice@test.com\n"
        "To: bob@test.com\n"
        "Subject: LF test\n"
        "\n"
        "Line 1\nLine 2";

    auto msg = parseEmailMessage(raw);
    ASSERT_EQ(msg.from, std::string("alice@test.com"));
    ASSERT_EQ(msg.subject, std::string("LF test"));
    ASSERT_EQ(msg.body, std::string("Line 1\nLine 2"));
    PASS();
}

TEST(parse_folded_headers) {
    // RFC 5322 folded header: continuation line starts with whitespace
    std::string raw =
        "From: sender@example.com\r\n"
        "Subject: This is a very long\r\n"
        " subject line that was folded\r\n"
        "To: receiver@example.com\r\n"
        "\r\n"
        "Body here.";

    auto msg = parseEmailMessage(raw);
    ASSERT_EQ(msg.subject, std::string("This is a very long subject line that was folded"));
    ASSERT_EQ(msg.from, std::string("sender@example.com"));
    ASSERT_EQ(msg.to, std::string("receiver@example.com"));
    ASSERT_EQ(msg.body, std::string("Body here."));
    PASS();
}

TEST(parse_empty_body) {
    std::string raw =
        "From: a@b.com\r\n"
        "To: c@d.com\r\n"
        "Subject: No body\r\n"
        "\r\n";

    auto msg = parseEmailMessage(raw);
    ASSERT_EQ(msg.from, std::string("a@b.com"));
    ASSERT_EQ(msg.subject, std::string("No body"));
    ASSERT_TRUE(msg.body.empty());
    PASS();
}

TEST(parse_no_headers_match) {
    std::string raw =
        "X-Custom: value\r\n"
        "Date: Mon, 1 Jan 2024\r\n"
        "\r\n"
        "Just body.";

    auto msg = parseEmailMessage(raw);
    ASSERT_TRUE(msg.from.empty());
    ASSERT_TRUE(msg.to.empty());
    ASSERT_TRUE(msg.subject.empty());
    ASSERT_EQ(msg.body, std::string("Just body."));
    PASS();
}

TEST(parse_case_insensitive_headers) {
    std::string raw =
        "FROM: Upper@case.com\r\n"
        "TO: Another@case.com\r\n"
        "SUBJECT: Caps\r\n"
        "\r\n"
        "Case test.";

    auto msg = parseEmailMessage(raw);
    ASSERT_EQ(msg.from, std::string("Upper@case.com"));
    ASSERT_EQ(msg.to, std::string("Another@case.com"));
    ASSERT_EQ(msg.subject, std::string("Caps"));
    PASS();
}

TEST(parse_multipart_body) {
    // Body with MIME boundaries - we just return the raw body
    std::string raw =
        "From: test@test.com\r\n"
        "Subject: Multipart\r\n"
        "Content-Type: multipart/mixed; boundary=\"----=_Part_123\"\r\n"
        "\r\n"
        "------=_Part_123\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Text part\r\n"
        "------=_Part_123--";

    auto msg = parseEmailMessage(raw);
    ASSERT_EQ(msg.from, std::string("test@test.com"));
    // Body should contain the full MIME body
    ASSERT_TRUE(msg.body.find("Text part") != std::string::npos);
    PASS();
}

TEST(parse_malformed_no_blank_line) {
    // No blank line separating headers from body
    std::string raw = "From: broken@test.com\r\nSubject: No separator";

    auto msg = parseEmailMessage(raw);
    // Should treat entire thing as headers
    ASSERT_EQ(msg.from, std::string("broken@test.com"));
    ASSERT_EQ(msg.subject, std::string("No separator"));
    ASSERT_TRUE(msg.body.empty());
    PASS();
}

TEST(parse_raw_preserved) {
    std::string raw = "From: x@y.com\r\n\r\nHello";
    auto msg = parseEmailMessage(raw);
    ASSERT_EQ(msg.raw, raw);
    PASS();
}

TEST(fetch_request_defaults) {
    POP3FetchRequest req;
    ASSERT_EQ(req.port, uint16_t(995));
    ASSERT_TRUE(req.use_tls);
    ASSERT_FALSE(req.delete_after_fetch);
    ASSERT_EQ(req.max_messages, 50);
    PASS();
}

TEST(dot_unstuffing_basic) {
    // Lines starting with ".." should have leading dot removed
    std::string input = "..This line was dot-stuffed\r\nNormal line\r\n..Another stuffed";
    auto result = dotUnstuff(input);
    ASSERT_EQ(result, std::string(".This line was dot-stuffed\r\nNormal line\r\n.Another stuffed"));
    PASS();
}

TEST(dot_unstuffing_no_dots) {
    std::string input = "No dots here\r\nJust text";
    auto result = dotUnstuff(input);
    ASSERT_EQ(result, std::string("No dots here\r\nJust text"));
    PASS();
}

TEST(dot_unstuffing_empty) {
    ASSERT_EQ(dotUnstuff(""), std::string(""));
    PASS();
}

TEST(dot_unstuffing_multiple_consecutive) {
    // "..." at start of line → ".." after unstuffing
    std::string input = "...triple dots";
    auto result = dotUnstuff(input);
    ASSERT_EQ(result, std::string("..triple dots"));
    PASS();
}

TEST(dot_unstuffing_mixed_line_endings) {
    // Mixed CRLF and LF line endings with dot-stuffing
    std::string input = "..Line1\r\nNormal\n..Line3\r\n..Line4";
    auto result = dotUnstuff(input);
    ASSERT_EQ(result, std::string(".Line1\r\nNormal\n.Line3\r\n.Line4"));
    PASS();
}

TEST(dot_unstuffing_single_dot_line) {
    // A line with just ".." should become "."
    std::string input = "before\r\n..\r\nafter";
    auto result = dotUnstuff(input);
    ASSERT_EQ(result, std::string("before\r\n.\r\nafter"));
    PASS();
}

TEST(fetch_empty_host_fails) {
    POP3FetchRequest req;
    req.host = "";
    auto result = fetchPOP3Messages(req);
    ASSERT_FALSE(result.success);
    ASSERT_TRUE(result.error.find("not configured") != std::string::npos);
    PASS();
}

TEST_MAIN()
