"""A luna-send stand-in for tests/sam_rescan_manager.cpp: configd's blocked-app list
and sam's block, release and app info, scripted from a JSON state file
(NATIVETV_SAMFAKE_STATE) that it also records every call into."""
import fcntl
import json
import os
import sys

KEY = "profile.blockedAppList"


def take(sequence, default):
    """The next scripted answer; the last one repeats."""
    if not sequence:
        return default
    return sequence.pop(0) if len(sequence) > 1 else sequence[0]


def handle(tv, uri, payload):
    tv["uris"].append(uri)
    method = uri.split("/")[-1]
    if method in ("blockSystemApp", "releaseBlockSystemApp"):
        tv["samCalls"].append("%s %s" % (method, payload.get("id")))
        tv["samPayloads"].append(payload)
        if not tv["hasBlock"]:
            return {"returnValue": False, "errorText": "Unknown method"}
        if method == "releaseBlockSystemApp" and tv["failReleases"] > 0:
            tv["failReleases"] -= 1
            return {"returnValue": False}
        tv["blocked"][payload.get("id")] = method == "blockSystemApp"
        # A block sam made whose answer never came back, as when the call times out.
        if method == "blockSystemApp" and tv["blockAnswerLost"]:
            return None
        return {"returnValue": True}
    if uri == "com.webos.service.config/getConfigs":
        if tv["list"] is None:
            return {"returnValue": True, "configs": {}, "missingConfigs": [KEY]}
        return {"returnValue": True, "configs": {KEY: list(tv["list"])}}
    if uri == "com.webos.service.config/setConfigs":
        written = payload["configs"][KEY]
        failed = tv["failWrites"] > 0 and bool(tv["writes"])
        if failed:
            tv["failWrites"] -= 1
        else:
            tv["list"] = list(written)
        tv["writes"].append(list(written))
        return {"returnValue": not failed}
    if method == "getAppInfo":
        info = take(tv["appInfo"].get(payload.get("id"), []), None)
        if info is None:
            return {"returnValue": False, "errorText": "Invalid appId specified"}
        return {"returnValue": True, "appInfo": info}
    if method == "listLaunchPoints":
        ids = take(tv["launchPoints"], [])
        return {"returnValue": True, "launchPoints": [{"launchPointId": i} for i in ids]}
    if method == "getForegroundAppInfo":
        return {"returnValue": True, "appId": tv["foreground"]}
    if method == "launch":
        tv["launches"].append(payload.get("id"))
    return {"returnValue": True}


def main():
    path = os.environ["NATIVETV_SAMFAKE_STATE"]
    args = sys.argv[1:]
    target = next(a for a in args if a.startswith("luna://"))
    payload = json.loads(args[args.index(target) + 1])
    with open(path + ".lock", "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        with open(path) as handle_file:
            tv = json.load(handle_file)
        reply = handle(tv, target[len("luna://"):], payload)
        with open(path, "w") as handle_file:
            json.dump(tv, handle_file)
    if reply is not None:
        print(json.dumps(reply))


main()
