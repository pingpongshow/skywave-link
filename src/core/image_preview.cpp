// Image preview - format detection and decoding via stb_image

#include "skywave_link/image_preview.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>

// stb_image implementation (if available)
#if __has_include("stb_image.h")
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO  // We'll load from memory
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#include "stb_image.h"
#define SKYWAVE_LINK_HAS_STB_IMAGE
#elif __has_include("stb/stb_image.h")
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO  // We'll load from memory
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#include "stb/stb_image.h"
#define SKYWAVE_LINK_HAS_STB_IMAGE
#endif

namespace skylink {

ImageFormat detectImageFormat(const uint8_t* data, size_t size) {
    if (!data || size < 4) return ImageFormat::Unknown;

    // PNG: 89 50 4E 47
    if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47)
        return ImageFormat::PNG;

    // JPEG: FF D8 FF
    if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        return ImageFormat::JPEG;

    // BMP: 42 4D
    if (data[0] == 0x42 && data[1] == 0x4D)
        return ImageFormat::BMP;

    // GIF: 47 49 46
    if (data[0] == 0x47 && data[1] == 0x49 && data[2] == 0x46)
        return ImageFormat::GIF;

    return ImageFormat::Unknown;
}

ImageFormat detectImageFormatByExtension(const std::string& filename) {
    // Find last dot
    auto dot = filename.rfind('.');
    if (dot == std::string::npos) return ImageFormat::Unknown;

    std::string ext = filename.substr(dot + 1);
    // Lowercase
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (ext == "png") return ImageFormat::PNG;
    if (ext == "jpg" || ext == "jpeg") return ImageFormat::JPEG;
    if (ext == "bmp") return ImageFormat::BMP;
    if (ext == "gif") return ImageFormat::GIF;

    return ImageFormat::Unknown;
}

bool isImageFile(const std::string& filename) {
    return detectImageFormatByExtension(filename) != ImageFormat::Unknown;
}

ImageData decodeImage(const uint8_t* data, size_t size) {
    ImageData result;
    if (!data || size == 0) return result;
#ifdef SKYWAVE_LINK_HAS_STB_IMAGE
    if (size > static_cast<size_t>(INT32_MAX)) return result;
    int w, h, channels;
    unsigned char* pixels = stbi_load_from_memory(data, static_cast<int>(size),
                                                   &w, &h, &channels, 4);  // Force RGBA
    if (pixels) {
        if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
            stbi_image_free(pixels);
            return result;
        }
        result.width = w;
        result.height = h;
        size_t pixel_count = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
        result.pixels.assign(pixels, pixels + pixel_count);
        result.valid = true;
        stbi_image_free(pixels);
    }
#else
    (void)data; (void)size;
#endif
    return result;
}

ImageData decodeImageFile(const std::string& path) {
    ImageData result;
#ifdef SKYWAVE_LINK_HAS_STB_IMAGE
    // Use stbi_load with stdio since we need file loading
    // But we disabled stdio above, so read manually
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return result;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 50 * 1024 * 1024) {  // 50 MB max
        fclose(f);
        return result;
    }

    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (fread(buf.data(), 1, buf.size(), f) != buf.size()) {
        fclose(f);
        return result;
    }
    fclose(f);

    return decodeImage(buf.data(), buf.size());
#else
    (void)path;
    return result;
#endif
}

// ImagePreviewCache

void ImagePreviewCache::store(const std::string& key, ImageData data) {
    std::lock_guard<std::mutex> lock(mutex_);

    // If key already exists, remove it from insertion_order_ so we can re-add at back
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        auto dq_it = std::find(insertion_order_.begin(), insertion_order_.end(), key);
        if (dq_it != insertion_order_.end()) {
            insertion_order_.erase(dq_it);
        }
        cache_.erase(it);
    }

    // Evict BEFORE inserting to avoid self-eviction of the new entry
    evictIfNeeded();

    cache_[key] = std::move(data);
    insertion_order_.push_back(key);
}

std::optional<ImageData> ImagePreviewCache::get(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(key);
    if (it == cache_.end()) return std::nullopt;
    return it->second;
}

bool ImagePreviewCache::withImage(const std::string& key,
                                   const std::function<void(const ImageData&)>& fn) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(key);
    if (it == cache_.end()) return false;
    fn(it->second);
    return true;
}

void ImagePreviewCache::evictIfNeeded() {
    // Note: called BEFORE inserting new entry, so we evict when size >= max
    if (max_entries_ == 0 || cache_.size() < max_entries_) return;
    // Evict oldest entry by insertion order
    while (cache_.size() >= max_entries_ && !insertion_order_.empty()) {
        auto oldest_key = insertion_order_.front();
        insertion_order_.pop_front();
        cache_.erase(oldest_key);
    }
}

bool ImagePreviewCache::has(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.count(key) > 0;
}

void ImagePreviewCache::remove(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_.erase(key) > 0) {
        auto it = std::find(insertion_order_.begin(), insertion_order_.end(), key);
        if (it != insertion_order_.end()) {
            insertion_order_.erase(it);
        }
    }
}

void ImagePreviewCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
    insertion_order_.clear();
}

void ImagePreviewCache::setMaxEntries(size_t max) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_entries_ = max;
    // Evict surplus entries if the new max is smaller than current size
    if (max > 0) {
        while (cache_.size() > max && !insertion_order_.empty()) {
            auto oldest_key = insertion_order_.front();
            insertion_order_.pop_front();
            cache_.erase(oldest_key);
        }
    }
}

size_t ImagePreviewCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

} // namespace skylink
