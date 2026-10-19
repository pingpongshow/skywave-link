// Image preview tests - format detection, cache

#include "test_macros.hpp"
#include "skywave_link/image_preview.hpp"

using namespace skylink;

TEST(detect_png_magic) {
    uint8_t png[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    ASSERT_EQ(static_cast<int>(detectImageFormat(png, sizeof(png))),
              static_cast<int>(ImageFormat::PNG));
    PASS();
}

TEST(detect_jpeg_magic) {
    uint8_t jpg[] = {0xFF, 0xD8, 0xFF, 0xE0};
    ASSERT_EQ(static_cast<int>(detectImageFormat(jpg, sizeof(jpg))),
              static_cast<int>(ImageFormat::JPEG));
    PASS();
}

TEST(detect_bmp_magic) {
    uint8_t bmp[] = {0x42, 0x4D, 0x00, 0x00};
    ASSERT_EQ(static_cast<int>(detectImageFormat(bmp, sizeof(bmp))),
              static_cast<int>(ImageFormat::BMP));
    PASS();
}

TEST(detect_gif_magic) {
    uint8_t gif[] = {0x47, 0x49, 0x46, 0x38};
    ASSERT_EQ(static_cast<int>(detectImageFormat(gif, sizeof(gif))),
              static_cast<int>(ImageFormat::GIF));
    PASS();
}

TEST(detect_unknown_magic) {
    uint8_t data[] = {0x00, 0x01, 0x02, 0x03};
    ASSERT_EQ(static_cast<int>(detectImageFormat(data, sizeof(data))),
              static_cast<int>(ImageFormat::Unknown));
    PASS();
}

TEST(detect_too_short) {
    uint8_t data[] = {0x89, 0x50};
    ASSERT_EQ(static_cast<int>(detectImageFormat(data, 2)),
              static_cast<int>(ImageFormat::Unknown));
    PASS();
}

TEST(extension_detection) {
    ASSERT_EQ(static_cast<int>(detectImageFormatByExtension("photo.png")),
              static_cast<int>(ImageFormat::PNG));
    ASSERT_EQ(static_cast<int>(detectImageFormatByExtension("photo.PNG")),
              static_cast<int>(ImageFormat::PNG));
    ASSERT_EQ(static_cast<int>(detectImageFormatByExtension("photo.jpg")),
              static_cast<int>(ImageFormat::JPEG));
    ASSERT_EQ(static_cast<int>(detectImageFormatByExtension("photo.jpeg")),
              static_cast<int>(ImageFormat::JPEG));
    ASSERT_EQ(static_cast<int>(detectImageFormatByExtension("photo.bmp")),
              static_cast<int>(ImageFormat::BMP));
    ASSERT_EQ(static_cast<int>(detectImageFormatByExtension("photo.gif")),
              static_cast<int>(ImageFormat::GIF));
    ASSERT_EQ(static_cast<int>(detectImageFormatByExtension("data.bin")),
              static_cast<int>(ImageFormat::Unknown));
    ASSERT_EQ(static_cast<int>(detectImageFormatByExtension("noext")),
              static_cast<int>(ImageFormat::Unknown));
    PASS();
}

TEST(is_image_file) {
    ASSERT_TRUE(isImageFile("test.png"));
    ASSERT_TRUE(isImageFile("test.JPG"));
    ASSERT_TRUE(isImageFile("test.gif"));
    ASSERT_FALSE(isImageFile("test.txt"));
    ASSERT_FALSE(isImageFile("test.cpp"));
    PASS();
}

TEST(decode_invalid_data) {
    uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03, 0x04};
    auto img = decodeImage(garbage, sizeof(garbage));
    ASSERT_FALSE(img.valid);
    PASS();
}

TEST(detect_null_data) {
    ASSERT_EQ(static_cast<int>(detectImageFormat(nullptr, 100)),
              static_cast<int>(ImageFormat::Unknown));
    PASS();
}

TEST(decode_null_data) {
    auto img = decodeImage(nullptr, 0);
    ASSERT_FALSE(img.valid);
    PASS();
}

TEST_MAIN()
