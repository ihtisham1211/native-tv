#!/usr/bin/env python3
"""Stage the native client in the Native Tv tile's app folder with the managed runtime, optionally running a bounded TV smoke test.

The client is the one native app sam is told of (assets/dashboard-app, added by the
dashboard's "App on the TV's home screen" or lifecycle/deploy.sh); this puts a development
build over its files. The server brings the folder back to the release's copy at its next
start, when the two differ and the app is not open."""
import argparse
import io
import json
from pathlib import Path
import shlex
import subprocess
import tarfile
import tempfile
from tools.runtime_payload import stage as stage_runtime, stage_client

ROOT = Path(__file__).resolve().parent
APP_ID = "com.tvweb.dashboard"
APP = "/media/developer/apps/usr/palm/applications/" + APP_ID
RUNTIME = "/var/lib/tvweb/native"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tv", required=True, help="Root SSH destination")
    parser.add_argument("--identity", type=Path)
    parser.add_argument("--known-hosts", type=Path)
    parser.add_argument("--build", type=Path, default=ROOT / "build-webos")
    parser.add_argument("--screen", default="home")
    parser.add_argument("--server", help="Optional loopback fixture origin passed to the native client")
    parser.add_argument("--script", type=Path)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--duration-ms", type=int, default=12000)
    parser.add_argument("--output", type=Path, default=ROOT / "out")
    args = parser.parse_args()
    if args.duration_ms < 1000 or args.duration_ms > 300000:
        parser.error("Duration must be 1–300 seconds")
    ssh = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", "-o", "LogLevel=ERROR"]
    if args.identity:
        ssh += ["-i", str(args.identity), "-o", "IdentitiesOnly=yes"]
    if args.known_hosts:
        ssh += ["-o", "UserKnownHostsFile=" + str(args.known_hosts)]
    ssh += [args.tv]
    with tempfile.TemporaryDirectory(prefix="nativetv-runtime-") as folder:
        manifest = stage_runtime(args.build, folder)
        runtime_files = [item["path"] for item in manifest["files"]] + ["runtime.json"]
        runtime = io.BytesIO()
        with tarfile.open(fileobj=runtime, mode="w:gz") as archive:
            for name in runtime_files:
                archive.add(Path(folder) / name, arcname=name + ".next")
        command = "mkdir -p " + shlex.quote(RUNTIME) + " && tar -xzf - -C " + shlex.quote(RUNTIME)
        for name in runtime_files:
            destination = RUNTIME + "/" + name
            command += " && mv " + shlex.quote(destination + ".next") + " " + shlex.quote(destination)
            mode = "755" if name in ("nativetv-daemon", "nativetv-service") else "644"
            command += " && chmod " + mode + " " + shlex.quote(destination)
        subprocess.run([*ssh, command], input=runtime.getvalue(), check=True)
    stream = io.BytesIO()
    with tempfile.TemporaryDirectory(prefix="nativetv-client-runtime-") as folder:
        client_manifest = stage_client(args.build, folder)
        # The executable and libraries are renamed into place; the assets are only read at start.
        client_files = [item["path"] for item in client_manifest["files"]
                        if not item["path"].startswith("assets/")] + ["runtime.json"]
        with tarfile.open(fileobj=stream, mode="w:gz") as archive:
            for name in client_files:
                archive.add(Path(folder) / name, arcname=name + ".next")
            for item in client_manifest["files"]:
                if item["path"].startswith("assets/"):
                    archive.add(Path(folder) / item["path"], arcname=item["path"])
            tile = ROOT / "assets/dashboard-app"
            archive.add(tile / "appinfo.json", arcname="appinfo.json")
            for icon in ("icon80.png", "icon130.png"):
                archive.add(tile / "assets" / icon, arcname="assets/" + icon)
            if args.script:
                if not isinstance(json.loads(args.script.read_text()), list):
                    parser.error("The smoke script must be a JSON array")
                archive.add(args.script, arcname="smoke.json")
    # A running client can finish on its old executable while the new one is staged.
    stage_command = "mkdir -p " + shlex.quote(APP) + " && tar -xzf - -C " + shlex.quote(APP)
    for name in client_files:
        destination = APP + "/" + name
        mode = "755" if name == "nativetv-tv" else "644"
        stage_command += " && mv " + shlex.quote(destination + ".next") + " " + shlex.quote(destination)
        stage_command += " && chmod " + mode + " " + shlex.quote(destination)
    subprocess.run([*ssh, stage_command], input=stream.getvalue(), check=True)
    # The licences that sat in assets/ in an earlier layout.
    subprocess.run([*ssh, "rm -f " + " ".join(shlex.quote(APP + "/" + name) for name in (
        "assets/LICENSE-json-c.txt", "assets/LICENSE-Roboto.txt"))], check=True)
    print("Staged " + APP_ID)
    if not args.run:
        return
    if args.server:
        origin = args.server
    else:
        port = subprocess.check_output([
            *ssh,
            "/var/lib/tvweb/native/nativetv-service port --data-root /var/lib/tvweb"
        ], text=True).strip()
        if not port.isdecimal() or not 1 <= int(port) <= 65535:
            parser.error("The TV returned an invalid server port")
        origin = "http://127.0.0.1:" + port
    options = [json.dumps({"@system_native_app": True, "nid": APP_ID}, separators=(",", ":")),
               "--server", origin, "--screen", args.screen,
               "--duration-ms", str(args.duration_ms), "--capture", "/tmp/nativetv-native.ppm", "--report", "/tmp/nativetv-native.json"]
    if args.script:
        options += ["--script", APP + "/smoke.json"]
    command = "cd " + shlex.quote(APP) + " && XDG_RUNTIME_DIR=/tmp/xdg /usr/bin/jailer -t native_devmode -i " + shlex.quote(APP_ID) + " -p " + shlex.quote(APP) + " " + shlex.quote(APP + "/nativetv-tv") + " " + " ".join(shlex.quote(value) for value in options)
    subprocess.run([*ssh, command], check=True, timeout=args.duration_ms / 1000 + 40)
    args.output.mkdir(parents=True, exist_ok=True)
    for name, remote in [("preview.ppm", "/tmp/nativetv-native.ppm"), ("report.json", "/tmp/nativetv-native.json")]:
        args.output.joinpath(name).write_bytes(subprocess.check_output([*ssh, "cat " + shlex.quote(remote)]))
    report = json.loads(args.output.joinpath("report.json").read_text())
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
