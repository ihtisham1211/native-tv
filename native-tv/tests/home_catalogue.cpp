#include "service/home_catalogue.hpp"
#include "service/home_profiles.hpp"
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
        char pattern[] = "/tmp/nativetv-home-catalogue-XXXXXX";
        char *created = mkdtemp(pattern);
        assert(created);
        path_ = created;
    }
    ~TemporaryDirectory() {
        (void)unlink((path_ + "/home-recent.json").c_str());
        (void)rmdir(path_.c_str());
    }
    const std::string &path() const { return path_; }

  private:
    std::string path_;
};
} // namespace

int main() {
    using namespace nativetv;
    const Json launchPoints = Json::parse(
        R"({"launchPoints":[
          {"id":"com.webos.app.netflix","title":"Netflix","largeIcon":"netflix.svg","icon":"netflix.PNG","iconColor":"#AABBCC","appDescription":"Movies"},
          {"id":"com.webos.app.youtube","title":"YouTube","extraLargeIcon":"youtube.png","bgColor":"bad"},
          {"id":"com.example.bookmark","lptype":"bookmark"},
          {"id":"com.example.hidden","visible":false},
          {"id":"com.tvweb.home"},
          {"id":"com.webos.app.hdmi1"},
          {"id":"com.webos.app.livetv-extra"}
        ]})");
    Json sidebar = Json::array();
    sidebar.add(Json::parse(
        R"({"returnValue":true,"appInfo":{"id":"com.webos.app.notificationcenter","title":"Notifications","icon":"notification.png","folderPath":"/apps/notifications"}})"));
    sidebar.add(Json::parse(
        R"({"returnValue":false,"appInfo":{"id":"com.webos.app.voice"}})"));
    const Json apps = service::homeCatalogueApps(launchPoints, sidebar);
    assert(apps.isArray() && apps.size() == 3);
    assert(apps[0]["id"].string() == "com.webos.app.netflix");
    assert(apps[0]["icon"].string() == "/api/home/icon?id=com.webos.app.netflix");
    assert(apps[0]["color"].string() == "#aabbcc");
    assert(apps[1]["icon"].string() == "/api/home/icon?id=com.webos.app.youtube");
    assert(!apps[1]["color"].valid());
    assert(apps[2]["id"].string() == "com.webos.app.notificationcenter");

    const Json devices = service::homeCatalogueDevices(Json::parse(
        R"({"devices":[
          {"id":"HDMI_1","connected":false},
          {"id":"HDMI_2","connected":true,"subList":[
            {"id":"SIMPLINK","osdName":"\u00a0AV   Receiver\u00a0","depth":1,"physicalAddress":4096},
            {"id":"SIMPLINK","osdName":"Game\u2003Console ","depth":2,"physicalAddress":8192},
            {"id":"OTHER","osdName":"ignored","depth":2}
          ]},
          {"id":"HDMI_10","connected":true}
        ]})"));
    assert(devices.isArray() && devices.size() == 3);
    assert(devices[0]["id"].string() == "hdmi1" && devices[0]["unused"].boolean());
    assert(devices[1]["id"].string() == "hdmi2:av-receiver");
    assert(devices[1]["address"].string() == "1.0.0.0");
    assert(devices[2]["id"].string() == "hdmi2:game-console");
    assert(devices[2]["via"].string() == "AV   Receiver");
    assert(devices[2]["address"].string() == "2.0.0.0");

    const Json rawDocument = Json::parse(
        R"({"channels":["channel-1","channel-2"],
           "sort":{"app":"recent","dev":"recent","ch":"recent"},
           "order":{"app":["com.webos.app.netflix"],"dev":["hdmi1"]},
           "profiles":[{"id":"p1","name":"Kids","color":"#F2B8B5"}],"profile":"p1"})");
    const Json document = service::normalizeHomeDocument(rawDocument);
    const Json channels = Json::parse(
        R"([{"id":"channel-1","number":"1","name":"One","radio":false},
            {"id":"unselected","number":"2","name":"Other","radio":false},
            {"id":"channel-2","number":"3","name":"Two","radio":true}])");
    const Json recent = Json::parse(
        R"({"app":{"com.webos.app.netflix":5,"com.webos.app.youtube":1},
           "dev":{"hdmi2":4,"hdmi2:game-console":10},
           "ch":{"channel-1":9,"channel-2":1}})");
    const Json totals = Json::parse(R"({"netflix":10,"youtube":80})");
    const Json catalogue = service::homeCatalogue(document, apps, devices, channels,
                                                   true, recent, totals, true);
    assert(catalogue["apps"][0]["id"].string() == "com.webos.app.youtube");
    assert(catalogue["order"]["app"][0].string() == "com.webos.app.netflix");
    assert(catalogue["order"]["app"][1].string() == "com.webos.app.youtube");
    assert(catalogue["order"]["app"][3].string() == "lg-home");
    assert(catalogue["order"]["dev"][0].string() == "hdmi2:game-console");
    assert(catalogue["order"]["dev"][1].string() == "hdmi2:av-receiver");
    assert(catalogue["order"]["ch"][0].string() == "channel-1");
    assert(catalogue["order"]["ch"][1].string() == "channel-2");
    assert(catalogue["channels"].size() == 2);
    assert(catalogue["sources"]["apps"].boolean());
    assert(catalogue["sources"]["devices"].boolean());
    assert(catalogue["sources"]["channels"].number() == 3);

    const Json withoutChannels = service::homeCatalogue(
        document, apps, devices, Json::parse("null"), false, recent, totals, false);
    assert(withoutChannels["channels"].size() == 0);
    assert(!withoutChannels["sources"]["channels"].valid());

    TemporaryDirectory root;
    std::ofstream recentFile((root.path() + "/home-recent.json").c_str());
    recentFile << R"({"app":{"com.webos.app.netflix":20}})";
    recentFile.close();
    assert(service::readHomeRecent(root.path())["app"]["com.webos.app.netflix"].number() == 20);
    return 0;
}
