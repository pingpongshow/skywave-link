#pragma once

#include "window_base.hpp"

#include <cstdint>
#include <string>

namespace skylink {

class StationMapWindow : public WindowBase {
public:
    explicit StationMapWindow(SharedAppState& state);
    ~StationMapWindow() override;
    void render() override;
    const char* name() const override { return "StationMap"; }

private:
    void ensureBasemapLoaded();
    void releaseBasemap();
    bool loadBasemapTexture(const std::string& path);
    std::string resolveBasemapPath() const;

    uint32_t basemap_texture_id_ = 0;
    int basemap_width_ = 0;
    int basemap_height_ = 0;
    bool basemap_load_attempted_ = false;
    std::string basemap_error_;
    std::string basemap_path_;
};

} // namespace skylink
