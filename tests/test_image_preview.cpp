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

TEST(cache_store_get) {
    ImagePreviewCache cache;
    ASSERT_EQ(cache.size(), size_t(0));

    ImageData img;
    img.width = 10;
    img.height = 10;
    img.pixels.resize(400, 0xFF);
    img.valid = true;

    cache.store("test.png", img);
    ASSERT_EQ(cache.size(), size_t(1));
    ASSERT_TRUE(cache.has("test.png"));

    auto got = cache.get("test.png");
    ASSERT_TRUE(got.has_value());
    ASSERT_EQ(got->width, 10);
    ASSERT_EQ(got->height, 10);
    ASSERT_TRUE(got->valid);
    PASS();
}

TEST(cache_miss) {
    ImagePreviewCache cache;
    ASSERT_FALSE(cache.has("missing.png"));
    ASSERT_FALSE(cache.get("missing.png").has_value());
    PASS();
}

TEST(cache_remove) {
    ImagePreviewCache cache;
    ImageData img;
    img.valid = true;
    cache.store("a.png", img);
    cache.store("b.png", img);
    ASSERT_EQ(cache.size(), size_t(2));

    cache.remove("a.png");
    ASSERT_FALSE(cache.has("a.png"));
    ASSERT_TRUE(cache.has("b.png"));
    ASSERT_EQ(cache.size(), size_t(1));
    PASS();
}

TEST(cache_clear) {
    ImagePreviewCache cache;
    ImageData img;
    img.valid = true;
    cache.store("a.png", img);
    cache.store("b.png", img);

    cache.clear();
    ASSERT_EQ(cache.size(), size_t(0));
    ASSERT_FALSE(cache.has("a.png"));
    PASS();
}

TEST(decode_invalid_data) {
    uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03, 0x04};
    auto img = decodeImage(garbage, sizeof(garbage));
    ASSERT_FALSE(img.valid);
    PASS();
}

TEST(cache_eviction_when_full) {
    ImagePreviewCache cache;
    cache.setMaxEntries(3);

    ImageData img;
    img.valid = true;
    img.width = 1;
    img.height = 1;
    img.pixels = {0xFF, 0xFF, 0xFF, 0xFF};

    cache.store("a.png", img);
    cache.store("b.png", img);
    cache.store("c.png", img);
    ASSERT_EQ(cache.size(), size_t(3));

    // Adding a 4th should evict the oldest (a.png)
    cache.store("d.png", img);
    ASSERT_EQ(cache.size(), size_t(3));
    ASSERT_FALSE(cache.has("a.png"));
    ASSERT_TRUE(cache.has("b.png"));
    ASSERT_TRUE(cache.has("c.png"));
    ASSERT_TRUE(cache.has("d.png"));

    // Adding a 5th should evict b.png
    cache.store("e.png", img);
    ASSERT_EQ(cache.size(), size_t(3));
    ASSERT_FALSE(cache.has("b.png"));
    ASSERT_TRUE(cache.has("c.png"));
    ASSERT_TRUE(cache.has("d.png"));
    ASSERT_TRUE(cache.has("e.png"));
    PASS();
}

TEST(set_max_entries_evicts_surplus) {
    ImagePreviewCache cache;

    ImageData img;
    img.valid = true;
    cache.store("a.png", img);
    cache.store("b.png", img);
    cache.store("c.png", img);
    cache.store("d.png", img);
    ASSERT_EQ(cache.size(), size_t(4));

    // Reduce max to 2 - should evict oldest two
    cache.setMaxEntries(2);
    ASSERT_EQ(cache.size(), size_t(2));
    ASSERT_FALSE(cache.has("a.png"));
    ASSERT_FALSE(cache.has("b.png"));
    ASSERT_TRUE(cache.has("c.png"));
    ASSERT_TRUE(cache.has("d.png"));
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

TEST(with_image_callback) {
    ImagePreviewCache cache;
    ImageData img;
    img.width = 32;
    img.height = 32;
    img.valid = true;

    cache.store("test.png", img);

    int called = 0;
    bool found = cache.withImage("test.png", [&](const ImageData& data) {
        called++;
        ASSERT_EQ(data.width, 32);
    });
    ASSERT_TRUE(found);
    ASSERT_EQ(called, 1);

    // Missing key returns false
    found = cache.withImage("missing.png", [&](const ImageData&) { called++; });
    ASSERT_FALSE(found);
    ASSERT_EQ(called, 1);
    PASS();
}

TEST_MAIN()
