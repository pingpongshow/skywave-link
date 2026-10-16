// FileTransferManager filename helpers: wire-length truncation and
// non-overwriting download paths.

#include "test_macros.hpp"
#include "skywave_link/file_transfer.hpp"

#include <filesystem>
#include <fstream>
#include <random>

using namespace skylink;

TEST(truncate_short_name_unchanged) {
    ASSERT_EQ(FileTransferManager::truncateFilename("photo.jpg"), std::string("photo.jpg"));
    PASS();
}

TEST(truncate_long_name_keeps_extension) {
    std::string name(300, 'a');
    name += ".jpg";
    std::string t = FileTransferManager::truncateFilename(name);
    ASSERT_EQ(t.size(), size_t(255));
    ASSERT_EQ(t.substr(t.size() - 4), std::string(".jpg"));
    PASS();
}

TEST(truncate_respects_utf8_boundary) {
    std::string name;
    for (int i = 0; i < 200; ++i) name += "\xC3\xA9";  // 'e-acute', 2 bytes each
    std::string t = FileTransferManager::truncateFilename(name, 255);
    ASSERT_LE(t.size(), size_t(255));
    ASSERT_EQ(t.size() % 2, size_t(0));  // no split sequence
    PASS();
}

TEST(unique_download_path_never_overwrites) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() /
                   ("skylink_ft_test_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);

    std::string p0 = FileTransferManager::uniqueDownloadPath(dir.string(), "report.txt");
    ASSERT_EQ(fs::path(p0).filename().string(), std::string("report.txt"));
    std::ofstream(p0) << "x";

    std::string p1 = FileTransferManager::uniqueDownloadPath(dir.string(), "report.txt");
    ASSERT_EQ(fs::path(p1).filename().string(), std::string("report (1).txt"));
    std::ofstream(p1) << "y";

    std::string p2 = FileTransferManager::uniqueDownloadPath(dir.string(), "report.txt");
    ASSERT_EQ(fs::path(p2).filename().string(), std::string("report (2).txt"));

    // A max-length name still yields a valid (<= 255 byte) unique name.
    std::string longname = FileTransferManager::truncateFilename(std::string(300, 'b') + ".bin");
    std::ofstream(dir / longname) << "z";
    std::string pl = FileTransferManager::uniqueDownloadPath(dir.string(), longname);
    ASSERT_LE(fs::path(pl).filename().string().size(), size_t(255));
    ASSERT_FALSE(fs::exists(pl));
    ASSERT_CONTAINS(fs::path(pl).filename().string(), " (1).bin");

    fs::remove_all(dir);
    PASS();
}

TEST_MAIN()
