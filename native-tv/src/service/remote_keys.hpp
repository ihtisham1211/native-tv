#pragma once

#include "core/json.hpp"
#include <string>

namespace nativetv {
namespace service {

struct RemoteKeysReadState {
    std::string systemRoot = "/";
    std::string dataRoot;
    Json filterChain;
    Json appsReply;
    Json inputDevices;
    bool inputSwitcherAvailable = false;
};

struct NativeRemoteKeysReadState {
    std::string dataRoot;
    Json filterChain;
    Json appsReply;
    Json inputDevices;
    bool inputSwitcherAvailable = false;
    Json mappingInfo;
    Json actions;
    Json buttonSettings;
    bool voiceConductorAvailable = false;
    bool alexaViewAvailable = false;
};

struct NativeRemoteKeysWriteState {
    Json mappingInfo;
    Json actions;
    Json buttonSettings;
    Json lgRecord;
};

struct NativeRemoteKeysWriteResult {
    Json mappingInfo;
    Json actionValues;
    Json buttonSettings;
    Json lgRecord;
    bool mappingChanged = false;
};

// True when the installed JavaScript compositor filters name a key supported
// by Native Tv. False on webOS 11 native-filter firmware.
bool hasJavaScriptRemoteKeys(const std::string &systemRoot);

// Installs the bundled compositor filter at the path referenced by configd.
// The replacement is private and atomic; changed is false when bytes already
// match, since rewriting an active filter requires a compositor reload.
bool installRemoteKeysFilterAsset(const std::string &assetPath,
                                  const std::string &installedPath,
                                  bool &changed, std::string *error = nullptr);

// Builds the read-only /api/apps remoteKeys projection for JavaScript-filter
// firmware. Unsupported or incomplete device data returns supported:false.
Json remoteKeysReadResponse(const RemoteKeysReadState &state);

// Builds the per-firmware /api/apps projection for webOS 11 native filters.
// Media actions are intentionally omitted because LG's native filter only
// launches apps; current and saved LG values determine which keys are offered.
Json nativeRemoteKeysReadResponse(const NativeRemoteKeysReadState &state);

// Reconciles a saved remote-key map with webOS 11's LG-owned mapping, action
// and custom-button values. The result contains only settings that need a
// write, plus the updated LG value record used to restore the original later.
NativeRemoteKeysWriteResult nativeRemoteKeysWriteRequests(
    const Json &map, const NativeRemoteKeysWriteState &state);
bool nativeRemoteKeysLgRecordHasValues(const Json &record);

} // namespace service
} // namespace nativetv
