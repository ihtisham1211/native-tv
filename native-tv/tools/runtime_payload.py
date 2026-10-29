#!/usr/bin/env python3
"""Stage portable ARM daemon and worker executables for the TV runtime."""
import argparse
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import struct
import tarfile
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
# libfreetype.so.6 is the TV's own, the one its SDL2_ttf needs: libtool 6.18.1 on a
# B3 (webOS 10.3.2) against the SDK's 6.18.3, the same soname series. Bundling the
# SDK's build would also pull in its bzip2/brotli/png, which have no licence notice
# here. webOS 4 and 5 are not checked: their FreeType is unverified.
SYSTEM_LIBRARIES = {"libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2",
                    "librt.so.1", "ld-linux.so.3", "libz.so.1",
                    "libGLESv2.so.2", "libSDL2-2.0.so.0",
                    "libSDL2_image-2.0.so.0", "libSDL2_ttf-2.0.so.0",
                    "libfreetype.so.6"}
# What the client opens below assets/ beside its executable, as paths under
# native-tv/assets: the legacy renderer's Roboto, the Figtree faces, the icon
# SVGs (icons/material/ and its licence included) and the carousel scrim. The
# one list for the SSH deploy, the Homebrew package and the IPK; every entry
# must match a file, so a rename here or in assets/ cannot ship half an install.
CLIENT_ASSETS = ("Roboto-Regular.woff", "Roboto-Medium.woff", "carousel-scrim.svg",
                 "fonts/Figtree-*.ttf", "fonts/LICENSE-*.txt", "icons/**/*")
PINNED_LICENSES = {
    "curl": {
        "7.87.0": (
            "https://raw.githubusercontent.com/curl/curl/curl-7_87_0/COPYING",
            "321b1a09ebc30410f2e837c072e5521cf7095b757193af4a7dae1086e36ed31a"),
    },
    "openssl": {
        "1.1.1q": (
            "https://raw.githubusercontent.com/openssl/openssl/OpenSSL_1_1_1q/LICENSE",
            "c32913b33252e71190af2066f08115c69bc9fddadf3bf29296e20c835389841c"),
    },
    "gcc-runtime": {
        "12.2.0": (
            "https://raw.githubusercontent.com/gcc-mirror/gcc/releases/gcc-12.2.0/COPYING.RUNTIME",
            "9d6b43ce4d8de0c878bf16b54d8e7a10d9bd42b75178153e3af6a815bdc90f74"),
    },
}


def json_notice():
    source = (ROOT / "third_party/json-c/SOURCE.md").read_text()
    start = "<!-- BEGIN DISTRIBUTION NOTICE -->\n"
    end = "<!-- END DISTRIBUTION NOTICE -->"
    if start not in source or end not in source:
        raise ValueError("The json-c source record has no distribution notice")
    return source.split(start, 1)[1].split(end, 1)[0].encode()


def civetweb_notice():
    return (ROOT / "third_party/civetweb/LICENSE.md").read_bytes()


def client_assets():
    """The client's asset files as sorted paths under native-tv/assets."""
    assets = ROOT / "assets"
    found = set()
    for pattern in CLIENT_ASSETS:
        matches = {path.relative_to(assets).as_posix()
                   for path in assets.glob(pattern) if path.is_file()}
        if not matches:
            raise ValueError("No client asset matches assets/" + pattern)
        found |= matches
    return sorted(found)


def inspect_binary(data, allow_external=False, shared=False):
    """Check the ARM ELF ABI and optionally return SDK library dependencies."""
    if len(data) < 52 or data[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError("The runtime requires a 32-bit little-endian ARM ELF executable")
    kind, machine = struct.unpack_from("<HH", data, 16)
    if kind not in (2, 3) or machine != 40 or (shared and kind != 3):
        raise ValueError("The runtime requires an ARM executable")
    offset = struct.unpack_from("<I", data, 28)[0]
    width, count = struct.unpack_from("<HH", data, 42)
    if width < 32 or not count or offset + width * count > len(data):
        raise ValueError("Invalid ELF program headers")
    loads, dynamic, interpreter = [], None, None
    for index in range(count):
        segment = struct.unpack_from("<IIIIIIII", data, offset + width * index)
        tag, start, address, _, size, _, _, _ = segment
        if start + size > len(data):
            raise ValueError("Truncated ELF segment")
        if tag == 1:
            loads.append((address, start, size))
        elif tag == 2:
            dynamic = data[start:start + size]
        elif tag == 3:
            interpreter = data[start:start + size].rstrip(b"\0")
    if (not shared and interpreter != b"/lib/ld-linux.so.3") or not dynamic or len(dynamic) % 8:
        raise ValueError("The runtime requires webOS ARM ELF metadata")
    tags, needed = {}, []
    for position in range(0, len(dynamic), 8):
        tag, value = struct.unpack_from("<II", dynamic, position)
        if tag == 0:
            break
        if tag == 1:
            needed.append(value)
        tags[tag] = value
    address, size = tags.get(5, -1), tags.get(10, 0)
    strings = None
    for base, start, extent in loads:
        if base <= address and address + size <= base + extent and size:
            strings = data[start + address - base:start + address - base + size]
    if strings is None:
        raise ValueError("Invalid ELF dynamic string table")

    def dynamic_string(offset):
        if offset < 0 or offset >= len(strings):
            raise ValueError("Invalid ELF dynamic string offset")
        end = strings.find(b"\0", offset)
        if end < 0:
            raise ValueError("Invalid ELF dynamic string")
        return strings[offset:end].decode("ascii")

    libraries = []
    for index in needed:
        libraries.append(dynamic_string(index))
    extra = set(libraries) - SYSTEM_LIBRARIES
    if extra and not allow_external:
        raise ValueError("Runtime must embed SDK libraries: " + ", ".join(sorted(extra)))
    runpathTag = 29 if 29 in tags else 15
    runpath = dynamic_string(tags[runpathTag]) if runpathTag in tags else ""
    soname = dynamic_string(tags[14]) if 14 in tags else ""
    versions = sorted(set(re.findall(rb"GLIBC_(\d+\.\d+(?:\.\d+)?)\x00", strings)))
    if any(tuple(map(int, value.decode().split("."))) > (2, 17) for value in versions):
        raise ValueError("The runtime must target glibc 2.17 or earlier")
    return {"architecture": "arm", "elfBits": 32, "needed": sorted(libraries),
            "glibc": [value.decode() for value in versions], "runpath": runpath,
            "soname": soname}


def _configured_sysroot(build):
    cache = Path(build) / "CMakeCache.txt"
    if not cache.is_file():
        return None
    for line in cache.read_text(errors="replace").splitlines():
        if line.startswith("NATIVETV_SDK_SYSROOT:") and "=" in line:
            value = line.split("=", 1)[1].strip()
            return Path(value) if value else None
    return None


def _resolve_in_sysroot(sysroot, path):
    root = Path(sysroot).resolve()
    current = Path(path)
    if not current.is_absolute():
        current = root / current
    for _ in range(128):
        current = Path(os.path.abspath(current))
        if os.path.commonpath((str(root), str(current))) != str(root):
            return None
        cursor = root
        followedLink = False
        for part in current.relative_to(root).parts:
            cursor = cursor / part
            if not cursor.is_symlink():
                continue
            target = os.readlink(cursor)
            current = (root / target.lstrip("/") if target.startswith("/")
                       else cursor.parent / target)
            followedLink = True
            break
        if followedLink:
            continue
        if not current.is_file():
            return None
        return current
    raise ValueError("SDK sysroot has a circular library symlink: " + str(path))


def _find_sdk_file(sysroot, name):
    root = Path(sysroot).resolve()
    for base in (root / "lib", root / "usr/lib", root / "usr/local/lib"):
        direct = base / name
        resolved = _resolve_in_sysroot(root, direct)
        if resolved:
            return resolved
    for base in (root / "lib", root / "usr/lib", root / "usr/local/lib"):
        if not base.is_dir():
            continue
        for candidate in base.rglob(name):
            resolved = _resolve_in_sysroot(root, candidate)
            if resolved:
                return resolved
    return None


def _license_package(soname):
    if soname.startswith(("libssl", "libcrypto")):
        return "openssl"
    if soname.startswith("libcurl"):
        return "curl"
    if soname.startswith("libatomic"):
        return "gcc-runtime"
    if soname.startswith("libbrotli"):
        return "brotli"
    prefix = soname.split(".so", 1)[0]
    return prefix[3:] if prefix.startswith("lib") else prefix


def _validate_library_name(name):
    if (not name or name in (".", "..", "runtime.json") or
            any(character not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-"
                for character in name)):
        raise ValueError("Unsupported SDK runtime library name: " + repr(name))
    return name


def _sdk_package_version(sysroot, package):
    root = Path(sysroot).resolve()
    if package in ("curl", "openssl"):
        filename = "libcurl.pc" if package == "curl" else "openssl.pc"
        metadata = root / "usr/lib/pkgconfig" / filename
        if not metadata.is_file():
            return ""
        match = re.search(r"^Version:\s*(\S+)\s*$", metadata.read_text(), re.MULTILINE)
        return match.group(1) if match else ""
    if package == "gcc-runtime":
        sdk_root = root.parents[1]
        include_root = sdk_root / "arm-webos-linux-gnueabi/include/c++"
        versions = sorted(path.name for path in include_root.iterdir()
                          if path.is_dir() and
                          re.fullmatch(r"\d+\.\d+\.\d+", path.name)) \
            if include_root.is_dir() else []
        return versions[0] if len(versions) == 1 else ""
    return ""


def _download_pinned_notice(sysroot, package, version, cache_directory):
    source = PINNED_LICENSES.get(package, {}).get(version)
    if not source:
        raise ValueError("SDK runtime dependency " + package + " " +
                         (version or "with unknown version") +
                         " has no license notice in the SDK or pinned source list")
    url, expected = source
    cache_path = None
    if cache_directory:
        cache_path = Path(cache_directory) / (package + "-" + version + ".txt")
        if cache_path.is_file():
            cached = cache_path.read_bytes()
            if hashlib.sha256(cached).hexdigest() == expected:
                return cached
            cache_path.unlink()
    try:
        with urllib.request.urlopen(url, timeout=20) as response:
            notice = response.read(2 * 1024 * 1024 + 1)
    except (OSError, urllib.error.URLError) as error:
        raise ValueError("could not fetch the pinned " + package + " " +
                         version + " license notice: " + str(error))
    if len(notice) > 2 * 1024 * 1024:
        raise ValueError("the pinned " + package + " license notice is too large")
    if hashlib.sha256(notice).hexdigest() != expected:
        raise ValueError("the pinned " + package + " license notice hash does not match")
    if cache_path:
        cache_path.parent.mkdir(parents=True, exist_ok=True)
        pending = cache_path.with_name(cache_path.name + ".new")
        pending.write_bytes(notice)
        pending.replace(cache_path)
    return notice


def _sdk_license_notice(sysroot, package, cache_directory=None):
    root = Path(sysroot).resolve()
    directories = (root / "usr/share/licenses", root / "usr/share/doc")
    noticeNames = {"license", "copying", "notice", "copyright"}
    candidates = []
    for base in directories:
        packageDirectory = base / package
        if packageDirectory.is_dir():
            candidates.extend(path for path in packageDirectory.rglob("*")
                              if path.is_file() and
                              (path.name.lower().split(".", 1)[0] in noticeNames or
                               path.name.lower().startswith(("license", "copying", "notice"))))
        elif packageDirectory.is_file():
            candidates.append(packageDirectory)
    notices = []
    seen = set()
    for candidate in sorted(candidates):
        resolved = _resolve_in_sysroot(root, candidate)
        if not resolved or resolved in seen or resolved.stat().st_size > 2 * 1024 * 1024:
            continue
        seen.add(resolved)
        notices.append((str(resolved.relative_to(root)), resolved.read_bytes()))
    if not notices:
        version = _sdk_package_version(root, package)
        notice = _download_pinned_notice(root, package, version, cache_directory)
        return ("Source: " + PINNED_LICENSES[package][version][0] + "\n" +
                "SHA-256: " + PINNED_LICENSES[package][version][1] + "\n\n").encode() + notice
    content = bytearray()
    for source, notice in notices:
        content.extend(("\n--- " + source + " ---\n").encode())
        content.extend(notice)
        if not notice.endswith(b"\n"):
            content.extend(b"\n")
    return bytes(content)


def _collect_sdk_libraries(sysroot, initial_libraries, cache_directory):
    pending = list(initial_libraries)
    bundled = {}
    files = {}
    packages = set()
    while pending:
        soname = _validate_library_name(pending.pop(0))
        if soname in SYSTEM_LIBRARIES or soname in bundled:
            continue
        if not sysroot or not Path(sysroot).is_dir():
            raise ValueError("Runtime dependency " + soname +
                             " requires the webOS SDK sysroot; build with --sdk or pass sysroot")
        library_path = _find_sdk_file(sysroot, soname)
        if not library_path:
            raise ValueError("Could not find " + soname + " in the webOS SDK sysroot")
        data = library_path.read_bytes()
        info = inspect_binary(data, allow_external=True, shared=True)
        if info["soname"] and info["soname"] != soname:
            raise ValueError("SDK library " + soname + " resolves to " + info["soname"])
        bundled[soname] = info
        files[soname] = (data, 0o644)
        packages.add(_license_package(soname))
        pending.extend(_validate_library_name(name) for name in sorted(
            set(info["needed"]) - SYSTEM_LIBRARIES - set(bundled)))

    for package in sorted(packages):
        notice_name = "LICENSE-" + package + ".txt"
        if notice_name in files:
            raise ValueError("SDK license filename conflicts with runtime file: " + notice_name)
        files[notice_name] = (
            _sdk_license_notice(sysroot, package, cache_directory), 0o644)
    return bundled, files


def _write_payload(destination, files, binaries, libraries):
    manifest = {"schema": 3, "protocol": 1,
                "abi": {"architecture": "arm", "elfBits": 32,
                        "binaries": binaries},
                "files": [
                    {"path": name, "sha256": hashlib.sha256(content).hexdigest(),
                     "size": len(content), "mode": format(mode, "04o")}
                    for name, (content, mode) in sorted(files.items())],
                "libraries": {name: libraries[name] for name in sorted(libraries)}}
    files["runtime.json"] = (
        (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode(), 0o644)
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    for name, (content, mode) in files.items():
        pending = destination / (name + ".new")
        pending.parent.mkdir(parents=True, exist_ok=True)
        pending.write_bytes(content)
        pending.chmod(mode)
        pending.replace(destination / name)
    return manifest


def stage(build, destination, sysroot=None):
    build = Path(build)
    sysroot = Path(sysroot) if sysroot else _configured_sysroot(build)
    binaries = {}
    files = {"LICENSE-json-c.txt": (json_notice(), 0o644),
             "LICENSE-civetweb.txt": (civetweb_notice(), 0o644),
             "remotekeys-filter.js": ((ROOT / "assets/remotekeys-filter.js").read_bytes(),
                                      0o644)}
    pendingLibraries = []
    for name in ("nativetv-daemon", "nativetv-service"):
        binary = build / name
        if not binary.is_file():
            raise ValueError("Build nativetv-service and nativetv-daemon first with "
                             "native-tv/build.py --sdk SDK --service-only")
        if binary.stat().st_size > 32 * 1024 * 1024:
            raise ValueError("The " + name + " binary exceeds the runtime size limit")
        data = binary.read_bytes()
        binaries[name] = inspect_binary(data, allow_external=True)
        external = set(binaries[name]["needed"]) - SYSTEM_LIBRARIES
        if external and "$ORIGIN" not in binaries[name]["runpath"].split(":"):
            raise ValueError(name + " must have $ORIGIN in its runtime library path")
        pendingLibraries.extend(_validate_library_name(name) for name in sorted(external))
        files[name] = (data, 0o755)

    bundled, library_files = _collect_sdk_libraries(
        sysroot, pendingLibraries, build / ".runtime-license-cache")
    files.update(library_files)
    return _write_payload(destination, files, binaries, bundled)


def stage_client(build, destination, sysroot=None):
    """Stage the native app, its assets and non-system SDK libraries for an app folder."""
    build = Path(build)
    sysroot = Path(sysroot) if sysroot else _configured_sysroot(build)
    binary = build / "nativetv-tv"
    if not binary.is_file():
        raise ValueError("Build the native webOS client first")
    if binary.stat().st_size > 32 * 1024 * 1024:
        raise ValueError("The nativetv-tv binary exceeds the runtime size limit")
    data = binary.read_bytes()
    info = inspect_binary(data, allow_external=True)
    external = set(info["needed"]) - SYSTEM_LIBRARIES
    if external and "$ORIGIN" not in info["runpath"].split(":"):
        raise ValueError("nativetv-tv must have $ORIGIN in its runtime library path")
    files = {"nativetv-tv": (data, 0o755), "LICENSE-json-c.txt": (json_notice(), 0o644)}
    for relative in client_assets():
        files["assets/" + relative] = ((ROOT / "assets" / relative).read_bytes(), 0o644)
    bundled, library_files = _collect_sdk_libraries(
        sysroot, sorted(external), build / ".runtime-license-cache")
    files.update(library_files)
    return _write_payload(destination, files, {"nativetv-tv": info}, bundled)


def archive_runtime(directory, output):
    """Reproducible runtime archive rooted at native/, ready for a server payload."""
    directory = Path(directory)
    manifest = json.loads((directory / "runtime.json").read_text())
    names = [item["path"] for item in manifest["files"]] + ["runtime.json"]
    with Path(output).open("wb") as file:
        with gzip.GzipFile(fileobj=file, mode="wb", filename="", mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.USTAR_FORMAT) as archive:
                for name in sorted(names):
                    content = (directory / name).read_bytes()
                    info = tarfile.TarInfo("native/" + name)
                    info.size = len(content)
                    info.mode = 0o755 if name in (
                        "nativetv-daemon", "nativetv-service", "nativetv-tv") else 0o644
                    archive.addfile(info, io.BytesIO(content))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build-webos")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--sysroot", type=Path,
                        help="webOS SDK sysroot (inferred from CMakeCache when omitted)")
    parser.add_argument("--client", action="store_true",
                        help="stage nativetv-tv and its non-system SDK libraries")
    args = parser.parse_args()
    try:
        manifest = (stage_client if args.client else stage)(
            args.build, args.out, args.sysroot)
        if args.archive:
            archive_runtime(args.out, args.archive)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    binary_name = "nativetv-tv" if args.client else "nativetv-daemon"
    label = "native client" if args.client else "daemon runtime"
    print("ARM " + label + ": " + str(args.out) + " (" + ", ".join(
        manifest["abi"]["binaries"][binary_name]["needed"]) + ")")


if __name__ == "__main__":
    main()
