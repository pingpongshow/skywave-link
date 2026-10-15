// Image preview cache for file transfers
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
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

// Cache for decoded image data (thread-safe)
class ImagePreviewCache {
public:
    ImagePreviewCache() = default;
    ~ImagePreviewCache() = default;

    // Store decoded image data keyed by path
    void store(const std::string& key, ImageData data);

    // Get a copy of decoded image data (thread-safe, copies pixel data)
    // Prefer withImage() to avoid copying megabytes of pixel data
    std::optional<ImageData> get(const std::string& key) const;

    // Access image data without copying (thread-safe, holds lock during callback)
    // Returns true if key was found and callback was invoked
    bool withImage(const std::string& key, const std::function<void(const ImageData&)>& fn) const;

    // Check if key exists
    bool has(const std::string& key) const;

    // Remove an entry
    void remove(const std::string& key);

    // Clear entire cache
    void clear();

    // Get number of cached entries
    size_t size() const;

    // Set maximum cache entries (0 = unlimited)
    void setMaxEntries(size_t max);

    static constexpr size_t DEFAULT_MAX_ENTRIES = 128;

private:
    mutable std::mutex mutex_;
    std::map<std::string, ImageData> cache_;
    std::deque<std::string> insertion_order_;  // Front = oldest
    size_t max_entries_ = DEFAULT_MAX_ENTRIES;

    void evictIfNeeded();  // Called under lock
};

} // namespace skylink
