/* Written by nativeTv: the remote keys set on its Apps tab, read from the
 * configd value nativetv.remoteKeys. First in com.webos.surfacemanager.keyFilters,
 * so a key set there never reaches LG's filters. */
var nativeTvKeyIds = {};
// nativeTv Home: the toast for Home until it is in place, and whether it is.
var nativeTvStarting = "";
var nativeTvHomeReady = false;
var nativeTvHoldingHome = false;
var nativeTvHoldOver = false;
var nativeTvEarlyDone = false;
var nativeTvStartAnswered = false;
var nativeTvSplash = false;
var nativeTvSplashDone = false;
var nativeTvMediaCodes = {"play":207,"pause":119,"stop":128,"rewind":168,"fastForward":208,"volumeUp":115,"volumeDown":114,"mute":113};
var nativeTvMediaRepeats = {"volumeUp":true,"volumeDown":true};

// Key code by name; a name this firmware lacks is left out.
function nativeTvKeyCodes() {
    var codes = {};
    try { codes["netflix"] = WebOS.Key_webOS_Netflix; } catch (e) {}
    try { codes["amazon"] = WebOS.Key_webOS_Amazon; } catch (e) {}
    try { codes["disneyplus"] = WebOS.Key_webOS_Disneyplus; } catch (e) {}
    try { codes["lgchannels"] = WebOS.Key_webOS_LGChannels; } catch (e) {}
    try { codes["rakutentv"] = WebOS.Key_webOS_RakutenTV; } catch (e) {}
    try { codes["hulu"] = WebOS.Key_webOS_Hulu; } catch (e) {}
    try { codes["iplayer"] = WebOS.Key_webOS_IPlayer; } catch (e) {}
    try { codes["freeviewplay"] = WebOS.Key_webOS_FreeviewPlay; } catch (e) {}
    try { codes["slingtv"] = WebOS.Key_webOS_SlingTV; } catch (e) {}
    try { codes["alexa"] = WebOS.Key_webOS_ALEXA; } catch (e) {}
    try { codes["googleassistant"] = WebOS.Key_webOS_GoogleVoice; } catch (e) {}
    try { codes["apps"] = WebOS.Key_webOS_Apps; } catch (e) {}
    try { codes["guide"] = WebOS.Key_webOS_TVGuide; } catch (e) {}
    try { codes["input"] = WebOS.Key_webOS_InputSource; } catch (e) {}
    try { codes["livetv"] = WebOS.Key_webOS_InputTV; } catch (e) {}
    try { codes["search"] = Qt.Key_Search; } catch (e) {}
    try { codes["info"] = WebOS.Key_webOS_Info; } catch (e) {}
    try { codes["help"] = Qt.Key_Help; } catch (e) {}
    try { codes["red"] = WebOS.Key_webOS_Red; } catch (e) {}
    try { codes["green"] = WebOS.Key_webOS_Green; } catch (e) {}
    try { codes["yellow"] = WebOS.Key_webOS_Yellow; } catch (e) {}
    try { codes["blue"] = WebOS.Key_webOS_Blue; } catch (e) {}
    return codes;
}

function nativeTvKeysChanged(payload) {
    try {
        var r = typeof payload === "string" ? JSON.parse(payload) : payload;
        var value = (r && r.configs && r.configs["nativetv.remoteKeys"]) || {};
        var set = value.keys || {};
        var codes = nativeTvKeyCodes(), ids = {};
        for (var name in set) {
            if (set.hasOwnProperty(name) && codes[name] !== undefined && typeof set[name] === "string") ids[codes[name]] = set[name];
        }
        // Home, only while nativeTv Home sets it.
        if (typeof value.home === "string" && value.home) ids[Qt.Key_Super_L] = value.home;
        nativeTvKeyIds = ids;
        nativeTvStarting = typeof value.starting === "string" ? value.starting : "";
        nativeTvSplash = value.splash === true;
        if (!nativeTvEarlyDone) {
            nativeTvEarlyDone = true;
            nativeTvEarlyStart();
            nativeTvSplashStart();
        }
    } catch (e) {
        console.warn("nativetv keyfilter: " + e + "; keys left as they were");
    }
}

// The first config read of a compositor start, which is before anything of
// nativeTv's own runs: asks the Homebrew Channel's service to run its
// startup script now rather than when LG's launcher asks it for its preview,
// some 20 s later (docs/development/TV-RESEARCH.md). The script runs once per
// boot whoever asks first, and the boot hook shows the starting notice: shown
// here too it came twice, and again on a compositor restart mid-session.
// The boot hook, early: calls a bus service of ours (assets/early-service) that starts it at about
// 22 s rather than 43-48 s, when the Homebrew Channel runs its startup script. Named under
// com.webos.*, the only names this filter's role may call, and allowing this caller,
// com.webos.starfish.keyfilters. The bus is not ready for the call at the first config read:
// the first to get through was the third, about 2 s on. One try a second until about 33 s;
// the service runs once per boot.
function nativeTvStartTry() {
    if (nativeTvStartAnswered) return;
    try {
        lunaCall.send("com.webos.nativetv.boot", "/start", "{}", function () { nativeTvStartAnswered = true; });
    } catch (e) {
        console.warn("nativetv keyfilter: " + e + "; boot hook not asked for");
    }
}

function nativeTvStartTries(n) {
    nativeTvStartTry();
    if (n > 0) genericTimer.set(1000, function () { nativeTvStartTries(n - 1); });
}

function nativeTvEarlyStart() {
    nativeTvStartTries(15);
}

// Where nativeTv Home opens at power-on (the boot hook's own conditions: the
// power-on reason is one LG opens Home for), the splash app is opened at once,
// and again when the input the TV starts on comes to the front over it, until
// the hook opens Home. Any other app first, or 90 s, and it is not opened again.
function nativeTvSplashStart() {
    if (!nativeTvSplash) return;
    try {
        genericTimer.set(90000, function () { nativeTvSplashDone = true; });
        lunaCall.send("com.webos.service.tvpower", "/power/getPowerOnReason", "{}", function (payload) {
            try {
                var r = typeof payload === "string" ? JSON.parse(payload) : payload;
                console.warn("nativetv keyfilter: power-on reason " + (r && r.reason));
                if (!r || ["aiKey", "lastPowerOn", "localKey", "remoteKey", "seniorHelp"].indexOf(r.reason) === -1) return;
                applicationManager.launch("com.tvweb.splash", JSON.stringify({ screen: "splash" }));
                lunaCall.subscribe("com.webos.applicationManager", "/getForegroundAppInfo",
                    JSON.stringify({ "subscribe": true }), nativeTvSplashForeground, true);
            } catch (e) {
                console.warn("nativetv keyfilter: " + e + "; no splash");
            }
        });
    } catch (e) {
        console.warn("nativetv keyfilter: " + e + "; no splash");
    }
}

function nativeTvSplashForeground(payload) {
    try {
        if (nativeTvSplashDone) return;
        var r = typeof payload === "string" ? JSON.parse(payload) : payload;
        var id = r && r.appId;
        console.warn("nativetv keyfilter: in front " + id);
        if (!id || id === "com.tvweb.splash") return;
        nativeTvSplashDone = true;
        var input = id === "com.webos.app.livetv" || id.indexOf("com.webos.app.hdmi") === 0 ||
            id.indexOf("com.webos.app.av") === 0 || id.indexOf("com.webos.app.comp") === 0;
        // After bootd's own launch of the input, which a launch in the same instant lost to;
        // 0.5 s after it held on a B3 (webOS 10.3.2).
        if (input) genericTimer.set(500, function () { applicationManager.launch("com.tvweb.splash", JSON.stringify({ screen: "splash" })); });
    } catch (e) {
        console.warn("nativetv keyfilter: " + e + "; no splash");
    }
}

function nativeTvSendKey(code) {
    lunaCall.send("com.webos.service.networkinput", "/test/sendKeyCode", JSON.stringify({ "keyCode": code }), function () {});
}

// Sends the media key a key is set to, by its name after "media:".
function nativeTvMedia(name) {
    if (nativeTvMediaCodes.hasOwnProperty(name)) nativeTvSendKey(nativeTvMediaCodes[name]);
}

// Opens Home once the app manager runs nativeTv Home under its id, and
// says it is starting until then. The key has been kept from LG's filters.
function nativeTvHomePressed() {
    lunaCall.send("com.webos.applicationManager", "/getAppInfo", JSON.stringify({ "id": "com.webos.app.home" }), function (payload) {
        try {
            var r = typeof payload === "string" ? JSON.parse(payload) : payload;
            if (r && r.appInfo && r.appInfo.type === "web") {
                nativeTvHomeReady = true;
                applicationManager.launch("com.webos.app.home", JSON.stringify({ "activateType": "home", "launchType": "homeKey" }));
                return;
            }
            lunaCall.send("com.webos.notification", "/createToast",
                JSON.stringify({ "sourceId": lunaCall.appId, "message": nativeTvStarting }), function () {});
        } catch (e) {
            console.warn("nativetv keyfilter: " + e);
        }
    });
}

function init() {
    try {
        genericTimer.set(90000, function () { nativeTvHoldOver = true; });
    } catch (e) {
        nativeTvHoldOver = true;
        console.warn("nativetv keyfilter: " + e + "; Home not held");
    }
    try {
        lunaCall.subscribe("com.webos.service.config", "/getConfigs",
            JSON.stringify({ "configNames": ["nativetv.remoteKeys"], "subscribe": true }), nativeTvKeysChanged, true);
    } catch (e) {
        console.warn("nativetv keyfilter: " + e + "; no keys set");
    }
}

function handleNativeTvKeys(key, pressed, autoRepeat) {
    try {
        // LG's filters open Home on the release, so a release whose press was kept
        // here is kept too, even where Home has opened in between.
        if (key === Qt.Key_Super_L && !pressed && nativeTvHoldingHome) {
            nativeTvHoldingHome = false;
            return KeyPolicy.Accepted;
        }
        if (key === Qt.Key_Super_L && pressed && nativeTvStarting && !nativeTvHomeReady && !nativeTvHoldOver) {
            nativeTvHoldingHome = true;
            if (!autoRepeat) nativeTvHomePressed();
            return KeyPolicy.Accepted;
        }
        if (!nativeTvKeyIds.hasOwnProperty(key)) return KeyPolicy.NextPolicy;
        var id = nativeTvKeyIds[key];
        if (id.indexOf("media:") === 0) {
            var media = id.substring(6);
            if (pressed && (!autoRepeat || nativeTvMediaRepeats.hasOwnProperty(media))) {
                if (!autoRepeat) console.info("nativetv keyfilter: key", key, "sends", id);
                nativeTvMedia(media);
            }
        } else if (pressed && !autoRepeat && id) {
            console.info("nativetv keyfilter: key", key, "opens", id);
            applicationManager.launch(id, "{}");
        }
        return KeyPolicy.Accepted;
    } catch (e) {
        console.warn("nativetv keyfilter: " + e + "; key passed on");
        return KeyPolicy.NextPolicy;
    }
}
