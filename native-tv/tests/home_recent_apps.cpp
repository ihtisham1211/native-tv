#include "service/home_recent_apps.hpp"
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
        char pattern[] = "/tmp/nativetv-home-recents-XXXXXX";
        char *created = mkdtemp(pattern);
        assert(created);
        path_ = created;
    }
    ~TemporaryDirectory() {
        (void)unlink((path_ + "/first.PNG").c_str());
        (void)unlink((path_ + "/second.jpeg").c_str());
        (void)unlink((path_ + "/symlink.png").c_str());
        (void)rmdir(path_.c_str());
    }
    const std::string &path() const { return path_; }

  private:
    std::string path_;
};

nativetv::Json app(const std::string &id, const nativetv::Json &title,
                     const std::string &custom, const std::string &snapshot,
                     const std::string &splash) {
    nativetv::Json result = nativetv::Json::object();
    result.set("appId", nativetv::Json::text(id));
    if (title.valid()) result.set("title", title);
    if (!custom.empty()) result.set("customImageFilePath", nativetv::Json::text(custom));
    if (!snapshot.empty()) result.set("cardSnapShotFilePath",
                                      nativetv::Json::text(snapshot));
    if (!splash.empty()) result.set("splashBackground", nativetv::Json::text(splash));
    return result;
}
} // namespace

int main() {
    using namespace nativetv;
    TemporaryDirectory directory;
    const std::string firstPath = directory.path() + "/first.PNG";
    const std::string secondPath = directory.path() + "/second.jpeg";
    const std::string symlinkPath = directory.path() + "/symlink.png";
    const char firstFixture[] = "\x89PNG\r\n\x1a\nrecent-image";
    const char secondFixture[] = "\xff\xd8recent-jpeg\xff\xd9";
    const std::string firstBytes(firstFixture, sizeof(firstFixture) - 1);
    const std::string secondBytes(secondFixture, sizeof(secondFixture) - 1);
    std::ofstream(firstPath.c_str(), std::ios::binary).write(
        firstBytes.data(), static_cast<std::streamsize>(firstBytes.size()));
    std::ofstream(secondPath.c_str(), std::ios::binary).write(
        secondBytes.data(), static_cast<std::streamsize>(secondBytes.size()));
    assert(symlink(firstPath.c_str(), symlinkPath.c_str()) == 0);

    Json recent = Json::array();
    recent.add(app("com.example.first", Json::text("First"), firstPath,
                   secondPath, ""));
    recent.add(app("com.example.second", Json::numeric(0), "", secondPath, ""));
    recent.add(app("com.example.second", Json::text("Duplicate"), "", "", ""));
    recent.add(app("com.example.no-picture", Json::text("Plain app"),
                   directory.path() + "/image.gif", "", ""));
    recent.add(app("com.example.symlink", Json::text("Symlink"), symlinkPath, "", ""));
    recent.add(app("com.webos.app.home", Json::text("LG Home"), firstPath, "", ""));
    recent.add(app("com.tvweb.native", Json::text("Native Home"), firstPath, "", ""));
    recent.add(app("com.tvweb.dashboard", Json::text("Native Tv"), firstPath, "", ""));
    recent.add(app("com.tvweb.splash", Json::text("Native Splash"), firstPath, "", ""));
    Json invalid = Json::object();
    invalid.set("title", Json::text("Missing app id"));
    recent.add(invalid);

    Json reply = Json::object();
    reply.set("returnValue", Json::flag(true));
    reply.set("recentsAppList", recent);
    const service::HomeRecentApps projected = service::homeRecentApps(reply);
    assert(projected.available && projected.apps.isArray());
    assert(projected.apps.size() == 4);
    assert(projected.apps[0]["id"].string() == "com.example.first");
    assert(projected.apps[0]["title"].string() == "First");
    assert(projected.apps[0]["image"].boolean());
    assert(projected.apps[1]["id"].string() == "com.example.second");
    assert(projected.apps[1]["title"].string() == "com.example.second");
    assert(projected.apps[1]["image"].boolean());
    assert(projected.apps[2]["id"].string() == "com.example.no-picture");
    assert(!projected.apps[2]["image"].boolean());
    assert(projected.apps[3]["id"].string() == "com.example.symlink");
    assert(projected.apps[3]["image"].boolean());

    const service::HomeRecentAppImage first =
        service::readHomeRecentAppImage(projected, "com.example.first");
    assert(first.listed && first.found && !first.readError);
    assert(first.contentType == "image/png" && first.bytes == firstBytes);
    assert(first.etag.size() > 4 && first.etag[0] == '"');
    // The first advertised path wins, matching Node's ordered path selection.
    const service::HomeRecentAppImage second =
        service::readHomeRecentAppImage(projected, "com.example.second");
    assert(second.found && second.contentType == "image/jpeg" &&
           second.bytes == secondBytes);
    assert(!service::readHomeRecentAppImage(projected, "com.example.unknown").listed);
    assert(!service::readHomeRecentAppImage(projected, "com.webos.app.home").listed);
    const service::HomeRecentAppImage symlink =
        service::readHomeRecentAppImage(projected, "com.example.symlink");
    assert(symlink.listed && !symlink.found);

    Json failed = Json::object();
    failed.set("returnValue", Json::flag(false));
    failed.set("recentsAppList", recent);
    assert(!service::homeRecentApps(failed).available);
    assert(!service::homeRecentApps(Json::object()).available);
    return 0;
}
