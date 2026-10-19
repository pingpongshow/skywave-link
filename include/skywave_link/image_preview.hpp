// Image decoding helpers (file window previews, station map)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace skylink {

// Loaded image texture data (pre-upload to GPU)
struct ImageData {
    std::vector<uint8_t> pixels;  // RGBA
    int width = 0;
    int height = 0;
    bool valid = false;
};

// GPU-uploaded texture handle
struct ImageTexture {
    uint32_t gl_texture_id = 0;
    int width = 0;
    int height = 0;
    bool valid = false;
};

// Detect image format from magic bytes
enum class ImageFormat {
    Unknown,
    PNG,
    JPEG,
    BMP,
    GIF
};

ImageFormat detectImageFormat(const uint8_t* data, size_t size);
ImageFormat detectImageFormatByExtension(const std::string& filename);
bool isImageFile(const std::string& filename);

// Decode an image from memory (uses stb_image)
ImageData decodeImage(const uint8_t* data, size_t size);
ImageData decodeImageFile(const std::string& path);

} // namespace skylink
