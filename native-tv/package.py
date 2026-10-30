#!/usr/bin/env python3
"""Package the Native Tv tile's app (the native client) with LG's packager, independently of the server."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
from tools.runtime_payload import stage_client

ROOT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build-webos")
    parser.add_argument("--out", type=Path, default=ROOT / "out")
    parser.add_argument("--packager", default="ares-package")
    args = parser.parse_args()
    binary = args.build / "nativetv-tv"
    if not binary.is_file():
        parser.error("Build the native webOS client first")
    header = binary.read_bytes()[:20]
    if len(header) != 20 or header[:7] != b"\x7fELF\x01\x01\x01" or header[18:20] != b"\x28\x00":
        parser.error("Packaging requires a 32-bit little-endian ARM ELF build")
    output = args.out.resolve()
    tile = ROOT / "assets/dashboard-app"
    app_id = json.loads(tile.joinpath("appinfo.json").read_text())["id"]
    stage = output / "package" / app_id
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    stage_client(args.build, stage)
    shutil.copyfile(tile / "appinfo.json", stage / "appinfo.json")
    for icon in ("icon80.png", "icon130.png"):
        shutil.copyfile(tile / "assets" / icon, stage / "assets" / icon)
    command = [args.packager] if shutil.which(args.packager) else ["npx", "--yes", "--package=@webos-tools/cli@3.2.6", "--", "ares-package"]
    subprocess.run([*command, "--no-minify", "--outdir", str(output), str(stage)], check=True)
    version = json.loads(tile.joinpath("appinfo.json").read_text())["version"]
    package = output / (app_id + "_" + version + "_arm.ipk")
    candidates = list(output.glob(app_id + "_" + version + "_*.ipk"))
    if not candidates:
        parser.error("The packager produced no native IPK")
    print(candidates[0] if not package.exists() else package)


if __name__ == "__main__":
    main()
