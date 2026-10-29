#include "service/remote_keys.hpp"
#include <cassert>
#include <cerrno>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace nativetv;

void makeDirectory(const std::string &path) {
    std::string current;
    for (size_t index = 0; index < path.size(); ++index) {
        current.push_back(path[index]);
        if (path[index] != '/' || current.size() <= 1) continue;
        if (mkdir(current.c_str(), 0700) != 0 && errno != EEXIST)
            assert(false && "could not create fixture directory");
    }
    if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST)
        assert(false && "could not create fixture directory");
}

void writeFile(const std::string &path, const std::string &contents) {
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) makeDirectory(path.substr(0, slash));
    std::ofstream output(path.c_str(), std::ios::binary);
    assert(output.good());
    output << contents;
    assert(output.good());
}

nativetv::Json findKey(const nativetv::Json &keys, const std::string &name) {
    for (const nativetv::Json &key : keys.items())
        if (key["key"].string() == name) return key;
    return nativetv::Json();
}

nativetv::Json findChoice(const nativetv::Json &choices, const std::string &id) {
    for (const nativetv::Json &choice : choices.items())
        if (choice["id"].string() == id) return choice;
    return nativetv::Json();
}
} // namespace

int main() {
    char temporary[] = "/tmp/nativetv-remote-keys-XXXXXX";
    char *root = mkdtemp(temporary);
    assert(root);
    const std::string systemRoot = root;
    const std::string dataRoot = systemRoot + "/data";
    const std::string filterPath = dataRoot + "/remotekeys-filter.js";
    const std::string assetPath = dataRoot + "/native/remotekeys-filter.js";
    writeFile(systemRoot + "/usr/lib/qt5/qml/KeyFilters/streaming.js",
              "WebOS.Key_webOS_Netflix WebOS.Key_webOS_Hulu "
              "Qt.Key_Search WebOS.Key_webOS_Red");
    const std::string filterSource = "// bundled filter fixture\n";
    writeFile(assetPath, filterSource);
    bool changed = false;
    std::string installError;
    assert(service::installRemoteKeysFilterAsset(assetPath, filterPath, changed,
                                                  &installError));
    assert(changed);
    std::ifstream installed(filterPath.c_str(), std::ios::binary);
    const std::string installedContents((std::istreambuf_iterator<char>(installed)),
                                        std::istreambuf_iterator<char>());
    assert(installedContents == filterSource);
    struct stat installedInfo = {};
    assert(stat(filterPath.c_str(), &installedInfo) == 0);
    assert((installedInfo.st_mode & 0777) == 0644);
    changed = true;
    assert(service::installRemoteKeysFilterAsset(assetPath, filterPath, changed,
                                                  &installError));
    assert(!changed);
    writeFile(dataRoot + "/country", "GB\n");
    writeFile(dataRoot + "/remotekeys.json",
              "{\"keys\":{\"netflix\":\"org.example.netflix\","
              "\"hulu\":\"\",\"red\":\"com.webos.app.livetv\"}}\n");

    service::RemoteKeysReadState state;
    state.systemRoot = systemRoot;
    state.dataRoot = dataRoot;
    state.filterChain = Json::parse(
        "[{\"file\":\"" + filterPath + "\",\"handler\":\"handleNativeTvKeys\"}]");
    state.appsReply = Json::parse(
        "{\"apps\":["
        "{\"id\":\"org.example.netflix\",\"title\":\"Netflix App\"},"
        "{\"id\":\"org.example.zulu\",\"title\":\"Zulu\"},"
        "{\"id\":\"org.example.alpha\",\"title\":\"alpha\"},"
        "{\"id\":\"org.example.hidden\",\"title\":\"Hidden\",\"visible\":false},"
        "{\"id\":\"com.webos.app.hdmi1\",\"title\":\"HDMI app\"},"
        "{\"id\":\"com.webos.app.hdmi-extra\",\"title\":\"HDMI helper\"},"
        "{\"id\":\"com.webos.app.container.demo\",\"title\":\"Container\"},"
        "{\"id\":\"com.webos.app.livetv\",\"title\":\"Live TV App\"}]}" );
    state.inputDevices = Json::parse(
        "[{\"id\":\"HDMI_1\",\"label\":\"Apple TV\"},"
        "{\"id\":\"HDMI2\"},{\"id\":\"AV1\",\"label\":\"AV\"}]");
    state.inputSwitcherAvailable = true;

    assert(service::hasJavaScriptRemoteKeys(systemRoot));
    const Json result = service::remoteKeysReadResponse(state);
    assert(result["ok"].boolean() && result["supported"].boolean());
    assert(result["inChain"].boolean());
    assert(result["keys"].size() == 4);
    const Json netflix = findKey(result["keys"], "netflix");
    assert(netflix["action"].string() == "open");
    assert(netflix["id"].string() == "org.example.netflix");
    assert(netflix["opens"].string() == "Netflix App");
    assert(!netflix["elsewhere"].boolean());
    const Json hulu = findKey(result["keys"], "hulu");
    assert(hulu["action"].string() == "nothing" && hulu["elsewhere"].boolean());
    assert(findKey(result["keys"], "search")["action"].string() == "lg");
    assert(findKey(result["keys"], "red")["opens"].string() == "Live TV");

    const Json switcher = findChoice(result["choices"], "com.webos.app.quickinputpicker");
    assert(switcher["kind"].string() == "switcher");
    const Json appleTv = findChoice(result["choices"], "com.webos.app.hdmi1");
    assert(appleTv["title"].string() == "Apple TV" && appleTv["kind"].string() == "input");
    assert(findChoice(result["choices"], "com.webos.app.hdmi2")["title"].string() == "HDMI 2");
    assert(findChoice(result["choices"], "media:fastForward")["title"].string() == "Fast forward");
    assert(!findChoice(result["choices"], "org.example.hidden").valid());
    assert(!findChoice(result["choices"], "com.webos.app.hdmi-extra").valid());
    assert(!findChoice(result["choices"], "com.webos.app.container.demo").valid());
    assert(findChoice(result["choices"], "org.example.alpha").valid());
    assert(findChoice(result["choices"], "org.example.zulu").valid());

    writeFile(dataRoot + "/remotekeys.json",
              "{\"keys\":{\"netflix\":\"org.example.alpha\","
              "\"multikey\":\"com.webos.app.hdmi2\","
              "\"multikeyhold\":\"\"}}\n");
    writeFile(dataRoot + "/remotekeys-lg.json",
              "{\"mapping\":{\"iplayer\":{\"launch_param\":null,"
              "\"app_id\":\"iplayer\",\"isActive\":true}},"
              "\"mappingWritten\":{}}");
    service::NativeRemoteKeysReadState nativeState;
    nativeState.dataRoot = dataRoot;
    nativeState.filterChain = Json::parse(
        "[{\"file\":\"/usr/lib/libappLaunch.so\",\"handler\":\"handleHotkeys\"}]");
    nativeState.appsReply = Json::parse(
        "{\"apps\":["
        "{\"id\":\"org.example.alpha\",\"title\":\"Alpha\"},"
        "{\"id\":\"org.example.hidden\",\"title\":\"Hidden\",\"visible\":false}]}");
    nativeState.inputDevices = Json::parse(
        "[{\"id\":\"HDMI_2\",\"label\":\"Game console\"}]");
    nativeState.inputSwitcherAvailable = true;
    nativeState.mappingInfo = Json::parse(
        "[{\"netflix\":{\"launch_param\":null,\"app_id\":\"netflix\","
        "\"isActive\":true}},{\"amazon\":{\"launch_param\":null,"
        "\"app_id\":\"amazon\",\"isActive\":false}},{\"iplayer\":{"
        "\"launch_param\":null,\"app_id\":\"iplayer\",\"isActive\":false}}]");
    nativeState.actions = Json::parse(
        "{\"tvGuide\":{\"action\":\"launch\",\"id\":\"guide\"},"
        "\"inputHub\":{\"action\":\"launch\",\"id\":\"hub\"}}");
    nativeState.buttonSettings = Json::parse(
        "{\"customShortPress\":{\"id\":\"short\"},"
        "\"customLongPress\":{\"id\":\"long\"}}");
    nativeState.voiceConductorAvailable = true;
    nativeState.alexaViewAvailable = true;
    const Json nativeResult = service::nativeRemoteKeysReadResponse(nativeState);
    assert(nativeResult["ok"].boolean() && nativeResult["supported"].boolean());
    assert(!nativeResult["inChain"].boolean());
    assert(findKey(nativeResult["keys"], "netflix")["action"].string() == "open");
    assert(findKey(nativeResult["keys"], "homehub").valid());
    assert(findKey(nativeResult["keys"], "guide").valid());
    assert(findKey(nativeResult["keys"], "multikey")["noNothing"].boolean());
    assert(findKey(nativeResult["keys"], "ai")["turnsOffVoice"].boolean());
    assert(findKey(nativeResult["keys"], "alexa")["blocksApp"].boolean());
    assert(findKey(nativeResult["keys"], "iplayer").valid());
    assert(findChoice(nativeResult["choices"], "com.webos.app.quickinputpicker").valid());
    assert(findChoice(nativeResult["choices"], "media:play").valid() == false);
    assert(findChoice(nativeResult["choices"], "org.example.hidden").valid() == false);

    service::NativeRemoteKeysWriteState nativeWriteState;
    nativeWriteState.mappingInfo = Json::parse(
        "[{\"netflix\":{\"launch_param\":null,\"app_id\":\"netflix\","
        "\"isActive\":true}},{\"amazon\":{\"launch_param\":null,"
        "\"app_id\":\"amazon\",\"isActive\":true}},{\"vendorKey\":"
        "{\"preserve\":true}}]");
    nativeWriteState.actions = nativeState.actions;
    nativeWriteState.buttonSettings = Json::parse(
        "{\"customShortPress\":{\"id\":\"accessibility\",\"type\":"
        "\"app\",\"params\":{}},\"customLongPress\":{\"id\":"
        "\"help\",\"type\":\"app\",\"params\":{}}}");
    nativeWriteState.lgRecord = Json::object();
    Json nativeMap = Json::parse(
        "{\"netflix\":\"org.example.alpha\",\"amazon\":\"\","
        "\"guide\":\"\",\"homehub\":\"org.example.alpha\","
        "\"multikey\":\"org.example.alpha\"}");
    service::NativeRemoteKeysWriteResult nativeWrites =
        service::nativeRemoteKeysWriteRequests(nativeMap, nativeWriteState);
    assert(nativeWrites.mappingChanged);
    assert(nativeWrites.mappingInfo[0]["netflix"]["app_id"].string() ==
           "org.example.alpha");
    assert(nativeWrites.mappingInfo[1]["amazon"]["isActive"].boolean() == false);
    assert(nativeWrites.mappingInfo[2]["vendorKey"]["preserve"].boolean());
    assert(nativeWrites.actionValues["com.webos.keyaction.tvGuide"]["action"].string() ==
           "ignore");
    assert(nativeWrites.actionValues["com.webos.keyaction.inputHub"]["id"].string() ==
           "org.example.alpha");
    assert(nativeWrites.buttonSettings["customShortPress"]["id"].string() ==
           "org.example.alpha");
    assert(!nativeWrites.buttonSettings.has("customLongPress"));
    assert(nativeWrites.lgRecord["mapping"]["netflix"]["app_id"].string() ==
           "netflix");
    assert(nativeWrites.lgRecord["actions"]["tvGuide"]["id"].string() == "guide");
    assert(service::nativeRemoteKeysLgRecordHasValues(nativeWrites.lgRecord));

    service::NativeRemoteKeysWriteState appliedWriteState = nativeWriteState;
    appliedWriteState.mappingInfo = nativeWrites.mappingInfo;
    appliedWriteState.actions.set("tvGuide",
        nativeWrites.actionValues["com.webos.keyaction.tvGuide"]);
    appliedWriteState.actions.set("inputHub",
        nativeWrites.actionValues["com.webos.keyaction.inputHub"]);
    appliedWriteState.buttonSettings = nativeWrites.buttonSettings;
    appliedWriteState.buttonSettings.set("customLongPress",
        nativeWriteState.buttonSettings["customLongPress"]);
    appliedWriteState.lgRecord = nativeWrites.lgRecord;
    const service::NativeRemoteKeysWriteResult unchangedWrites =
        service::nativeRemoteKeysWriteRequests(nativeMap, appliedWriteState);
    assert(!unchangedWrites.mappingChanged);
    assert(unchangedWrites.actionValues.size() == 0);
    assert(unchangedWrites.buttonSettings.size() == 0);

    Json lgNetflix = Json::object();
    lgNetflix.set("launch_param", Json());
    lgNetflix.set("app_id", Json::text("lg.netflix"));
    lgNetflix.set("isActive", Json::flag(true));
    Json lgChangedMapping = Json::array();
    Json lgChangedFirst = Json::object();
    lgChangedFirst.set("netflix", lgNetflix);
    lgChangedMapping = Json::array();
    lgChangedMapping.add(lgChangedFirst);
    for (size_t index = 1; index < appliedWriteState.mappingInfo.size(); ++index)
        lgChangedMapping.add(appliedWriteState.mappingInfo[index]);
    service::NativeRemoteKeysWriteState lgChangedState = appliedWriteState;
    lgChangedState.mappingInfo = lgChangedMapping;
    const service::NativeRemoteKeysWriteResult reapplyWrites =
        service::nativeRemoteKeysWriteRequests(nativeMap, lgChangedState);
    assert(reapplyWrites.mappingChanged);
    assert(reapplyWrites.lgRecord["mapping"]["netflix"]["app_id"].string() ==
           "lg.netflix");
    assert(reapplyWrites.mappingInfo[0]["netflix"]["app_id"].string() ==
           "org.example.alpha");

    service::NativeRemoteKeysWriteState restoreState = appliedWriteState;
    restoreState.lgRecord = nativeWrites.lgRecord;
    const service::NativeRemoteKeysWriteResult restoreWrites =
        service::nativeRemoteKeysWriteRequests(Json::object(), restoreState);
    assert(restoreWrites.mappingChanged);
    assert(restoreWrites.mappingInfo[0]["netflix"]["app_id"].string() == "netflix");
    assert(restoreWrites.mappingInfo[1]["amazon"]["isActive"].boolean());
    assert(restoreWrites.actionValues["com.webos.keyaction.tvGuide"]["id"].string() ==
           "guide");
    assert(restoreWrites.buttonSettings["customShortPress"]["id"].string() ==
           "accessibility");
    assert(!service::nativeRemoteKeysLgRecordHasValues(restoreWrites.lgRecord));

    state.filterChain = Json::object();
    const Json unsupported = service::remoteKeysReadResponse(state);
    assert(unsupported["ok"].boolean() && !unsupported["supported"].boolean());

    unlink((dataRoot + "/country").c_str());
    unlink((dataRoot + "/remotekeys.json").c_str());
    unlink((dataRoot + "/remotekeys-lg.json").c_str());
    unlink(filterPath.c_str());
    unlink(assetPath.c_str());
    unlink((systemRoot + "/usr/lib/qt5/qml/KeyFilters/streaming.js").c_str());
    rmdir((systemRoot + "/usr/lib/qt5/qml/KeyFilters").c_str());
    rmdir((systemRoot + "/usr/lib/qt5/qml").c_str());
    rmdir((systemRoot + "/usr/lib/qt5").c_str());
    rmdir((systemRoot + "/usr/lib").c_str());
    rmdir((systemRoot + "/usr").c_str());
    rmdir((dataRoot + "/native").c_str());
    rmdir(dataRoot.c_str());
    rmdir(systemRoot.c_str());
    return 0;
}
