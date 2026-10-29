#include "service/home_wallpapers.hpp"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        char pattern[] = "/tmp/nativetv-home-wallpapers-XXXXXX";
        char *created = mkdtemp(pattern);
        assert(created);
        path_ = created;
    }
    ~TemporaryDirectory() {
        (void)unlink((path_ + "/home-wallpaper-uploads/morning.jpg").c_str());
        (void)unlink((path_ + "/home-wallpaper-uploads/afternoon.jpg.part").c_str());
        (void)unlink((path_ + "/home-wallpaper-uploads/night.jpg").c_str());
        (void)unlink((path_ + "/home-wallpaper-uploads/evening.jpg").c_str());
        (void)rmdir((path_ + "/home-wallpaper-uploads").c_str());
        (void)rmdir(path_.c_str());
    }
    const std::string &path() const { return path_; }

  private:
    std::string path_;
};

void write(const std::string &path, const std::string &bytes) {
    std::ofstream output(path.c_str(), std::ios::binary);
    assert(output.good());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    assert(output.good());
}

std::string validJpeg() {
    std::string bytes("\xff\xd8", 2);
    bytes.append("\xff\xdb\x00\x43", 4);
    bytes.append(65, '\0');
    bytes.append("\xff\xc4\x00\x14", 4);
    bytes.append(18, '\0');
    bytes.append("\xff\xc0\x00\x0b\x08\x00\x01\x00\x01\x01\x01\x11\x00", 13);
    bytes.append("\xff\xda\x00\x08\x01\x01\x00\x00\x3f\x00", 10);
    bytes.push_back('\x01');
    bytes.append("\xff\xd9", 2);
    return bytes;
}
} // namespace

int main() {
    using nativetv::service::readHomeWallpaper;
    TemporaryDirectory root;
    const std::string uploads = root.path() + "/home-wallpaper-uploads";
    assert(mkdir(uploads.c_str(), 0700) == 0);

    assert(!readHomeWallpaper("../night", root.path()).found);
    assert(!readHomeWallpaper("night/../morning", root.path()).found);
    assert(!readHomeWallpaper("morning", root.path()).found);

    const std::string bytes("jpeg\0fixture", 12);
    write(uploads + "/morning.jpg", bytes);
    const auto wallpaper = readHomeWallpaper("morning", root.path());
    assert(wallpaper.found && !wallpaper.readError);
    assert(wallpaper.bytes == bytes);
    assert(wallpaper.etag.size() > 5 && wallpaper.etag[0] == '"' &&
           wallpaper.etag[wallpaper.etag.size() - 1] == '"');
    const auto status = nativetv::service::homeWallpaperStatus(root.path());
    assert(status["available"].boolean() && !status["uploading"].boolean());
    assert(status["slots"]["morning"].boolean());
    assert(status["slots"]["night"].boolean() == false);
    write(uploads + "/afternoon.jpg.part", "partial upload");
    assert(nativetv::service::homeWallpaperStatus(root.path())["uploading"].boolean());

    assert(symlink("/etc/passwd", (uploads + "/night.jpg").c_str()) == 0);
    const auto symlinked = readHomeWallpaper("night", root.path());
    assert(!symlinked.found && !symlinked.readError);

    write(uploads + "/evening.jpg", std::string(5 * 1024 * 1024 + 1, 'x'));
    const auto oversized = readHomeWallpaper("evening", root.path());
    assert(!oversized.found && !oversized.readError);

    const std::string jpeg = validJpeg();
    assert(nativetv::service::validHomeWallpaperJpeg(jpeg));
    assert(!nativetv::service::validHomeWallpaperJpeg("\xff\xd8\xff\xd9"));
    std::string error;
    assert(nativetv::service::saveHomeWallpaper(
               "morning", root.path(), jpeg, &error) ==
           nativetv::service::HomeWallpaperSaveResult::saved);
    assert(error.empty());
    assert(readHomeWallpaper("morning", root.path()).bytes == jpeg);
    assert(nativetv::service::saveHomeWallpaper(
               "morning", root.path(), "invalid", &error) ==
           nativetv::service::HomeWallpaperSaveResult::invalidImage);
    assert(readHomeWallpaper("morning", root.path()).bytes == jpeg);
    assert(nativetv::service::saveHomeWallpaper(
               "unknown", root.path(), jpeg, &error) ==
           nativetv::service::HomeWallpaperSaveResult::invalidSlot);
    return 0;
}
