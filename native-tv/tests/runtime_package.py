"""Runtime ABI/package contracts and Homebrew install/upgrade behavior."""
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import stat
import struct
import subprocess
import tarfile
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("runtime_payload", ROOT / "tools/runtime_payload.py")
runtime = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runtime)


def elf(libraries=("libc.so.6", "libpthread.so.0"), glibc="2.4",
        runpath=None, shared=False, soname=None):
    data = bytearray(1024)
    data[:7] = b"\x7fELF\x01\x01\x01"
    kind = 3 if shared else 2
    programCount = 2 if shared else 3
    struct.pack_into("<HHIIIIIHHHHHH", data, 16, kind, 40, 1, 0x1000, 52, 0, 0,
                     52, 32, programCount, 0, 0, 0)
    interpreter = b"/lib/ld-linux.so.3\0"
    strings, offsets = b"\0", []
    for name in libraries:
        offsets.append(len(strings))
        strings += name.encode() + b"\0"
    strings += ("GLIBC_" + glibc).encode() + b"\0"
    sonameOffset = None
    if soname:
        sonameOffset = len(strings)
        strings += soname.encode() + b"\0"
    runpathOffset = None
    if runpath is not None:
        runpathOffset = len(strings)
        strings += runpath.encode() + b"\0"
    data[200:200 + len(strings)] = strings
    entries = [(1, offset) for offset in offsets]
    if sonameOffset is not None:
        entries.append((14, sonameOffset))
    if runpathOffset is not None:
        entries.append((29, runpathOffset))
    entries += [(5, 0x1000 + 200), (10, len(strings)), (0, 0)]
    for index, entry in enumerate(entries):
        struct.pack_into("<II", data, 600 + 8 * index, *entry)
    segments = [
        (1, 0, 0x1000, 0, len(data), len(data), 5, 4096),
        (2, 600, 0x1000 + 600, 0, len(entries) * 8, len(entries) * 8, 6, 4)]
    if not shared:
        data[160:160 + len(interpreter)] = interpreter
        segments.append((3, 160, 0x1000 + 160, 0, len(interpreter), len(interpreter), 4, 1))
    for index, entry in enumerate(segments):
        struct.pack_into("<IIIIIIII", data, 52 + index * 32, *entry)
    return bytes(data)


class RuntimeTests(unittest.TestCase):
    def test_manual_packages_require_arm_runtime(self):
        spec = importlib.util.spec_from_file_location("build_ipk", ROOT.parent / "scripts/build-ipk.py")
        package = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(package)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            app = root / "app"
            build = root / "build"
            build.mkdir()
            with self.assertRaises(ValueError):
                package.stage(str(app), "9.1.0", build)
            self.assertFalse(app.exists(), "Missing runtime must fail before web payload staging")
            binary = elf()
            (build / "nativetv-service").write_bytes(binary)
            with self.assertRaises(ValueError):
                package.stage(str(app), "9.1.0", build)
            self.assertFalse(app.exists(), "A worker without its daemon must fail before web payload staging")
            (build / "nativetv-daemon").write_bytes(binary)
            with self.assertRaises(ValueError):
                package.stage(str(app), "9.1.0", build)
            self.assertFalse(app.exists(), "The native splash payload requires the client build")
            # The client links FreeType, SDL and GLES directly; all are the TV's own.
            (build / "nativetv-tv").write_bytes(elf(
                libraries=("libc.so.6", "libfreetype.so.6", "libSDL2_ttf-2.0.so.0",
                           "libGLESv2.so.2"), runpath="$ORIGIN"))
            package.stage(str(app), "9.1.0", build)
            self.assertEqual((app / "payload/tvweb/native/nativetv-service").read_bytes(), binary)
            self.assertEqual((app / "payload/tvweb/native/nativetv-daemon").read_bytes(), binary)
            self.assertTrue((app / "payload/tvweb/native/remotekeys-filter.js").is_file())
            self.assertTrue((app / "payload/tvweb/native/runtime.json").is_file())
            # The dashboard tile and the splash are each the whole client.
            for name, identifier in (("splash-app", "com.tvweb.splash"),
                                     ("dashboard-app", "com.tvweb.dashboard")):
                folder = app / "payload/tvweb/assets" / name
                self.assertEqual((folder / "nativetv-tv").read_bytes(),
                                 (build / "nativetv-tv").read_bytes())
                info = json.loads((ROOT / "assets" / name / "appinfo.json").read_text())
                self.assertEqual((info["id"], info["type"], info["main"]),
                                 (identifier, "native", "nativetv-tv"))
                self.assertFalse((folder / "main.qml").exists())
                self.assertTrue((folder / "assets/icons/splash-mark.svg").is_file())
                self.assertTrue((folder / "assets/icons/material/LICENSE-MaterialDesignIcons.txt").is_file())
                self.assertTrue((folder / "assets/fonts/GoogleSansFlex-Regular.ttf").is_file())
                self.assertTrue((folder / "assets/fonts/LICENSE-GoogleSansFlex-OFL.txt").is_file())
                self.assertTrue((folder / "assets/Roboto-Regular.woff").is_file())
                self.assertTrue((folder / "LICENSE-json-c.txt").is_file())
                self.assertTrue((folder / "runtime.json").is_file())
            # The tile's web page is gone; the old Home app keeps its own.
            self.assertFalse((app / "payload/tvweb/assets/dashboard-app/index.html").exists())
            # A missing build must fail locally, before SSH or telnet to the supplied address.
            deploy = subprocess.run(["bash", str(ROOT / "lifecycle/deploy.sh"),
                                     "invalid.invalid", "--native-build=" + str(root / "missing")],
                                    capture_output=True, text=True, timeout=10)
            self.assertNotEqual(deploy.returncode, 0)
            self.assertIn("Build nativetv-service and nativetv-daemon first", deploy.stderr)

    def test_deploy_probe_asks_nc_before_bash(self):
        # On macOS, bash's own /dev/tcp open fails now and then with "No route to
        # host" on an address nc connects to, so answers() asks nc first and falls
        # back to /dev/tcp without it or when it says no.
        script = (ROOT / "lifecycle/deploy.sh").read_text()
        start = script.index("\nanswers() {")
        probe = script[start:script.index("\n}\n", start) + 3]
        with tempfile.TemporaryDirectory() as folder, socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            # Nobody accepts, so every probe's connection waits in the queue. A queue one deep
            # refused a later connection about one run in twelve on macOS, and the check that
            # follows nc's no then reported the port closed.
            listener.listen(8)
            openPort = listener.getsockname()[1]
            with socket.socket() as spare:
                spare.bind(("127.0.0.1", 0))
                closedPort = spare.getsockname()[1]
            tools = Path(folder) / "bin"
            tools.mkdir()
            (tools / "sleep").symlink_to(shutil.which("sleep"))
            calls = Path(folder) / "nc-calls"

            def answers(port, nc=None):
                fake = tools / "nc"
                if nc is None:
                    fake.unlink(missing_ok=True)
                else:
                    fake.write_text('#!/bin/sh\necho "$@" >> "%s"\nexit %d\n' % (calls, nc))
                    fake.chmod(0o755)
                run = subprocess.run(
                    [shutil.which("bash"), "-c", probe + '\nTV=127.0.0.1\nanswers "$1"',
                     "deploy", str(port)],
                    env={"PATH": str(tools)}, capture_output=True, text=True, timeout=20)
                return run.returncode

            self.assertEqual(answers(openPort), 0, "bash alone reaches a listening port")
            self.assertNotEqual(answers(closedPort), 0, "bash alone reports a closed port")
            self.assertEqual(answers(closedPort, nc=0), 0, "nc's yes is believed")
            self.assertEqual(calls.read_text().split(),
                             ["-z", "-w", "6", "127.0.0.1", str(closedPort)])
            self.assertEqual(answers(openPort, nc=1), 0, "after nc's no, bash still tries")

    def test_deploy_bundle_is_written_as_owned_by_root(self):
        # ustar holds IDs up to 2097151, and a Mac account with a larger group ID made tar refuse
        # every entry (upstream Glasshouse #924), so pack_bundle writes the bundle as root: with
        # --owner/--group, then --uid/--gid for the tar on macOS 10.15 to 15.3, then as it is.
        script = (ROOT / "lifecycle/deploy.sh").read_text()
        start = script.index("\nmake_bundle() {")
        functions = script[start:script.index("\npack_bundle\n", start) + 1]
        with tempfile.TemporaryDirectory() as folder:
            work = Path(folder) / "work"
            (work / "b").mkdir(parents=True)
            (work / "b/file").write_text("x")
            tools = Path(folder) / "bin"
            tools.mkdir()
            calls = Path(folder) / "tar-calls"
            # A tar that takes the options in TAR_TAKES (a word each) and, given none of them, still
            # sees the account's large group ID.
            (tools / "tar").write_text(
                '#!/bin/sh\n'
                'for a in "$@"; do case "$a" in\n'
                '  --owner=*) case " $TAR_TAKES " in *" owner "*) ;; *) echo "unsupported $a" >&2; exit 1 ;; esac; given=1 ;;\n'
                '  --uid=*) case " $TAR_TAKES " in *" uid "*) ;; *) echo "unsupported $a" >&2; exit 1 ;; esac; given=1 ;;\n'
                'esac; done\n'
                'echo "$@" >> "%s"\n'
                '[ -n "$given" ] || [ -z "$TAR_LARGE_GID" ] || { echo "gid too large" >&2; exit 1; }\n' % calls)
            (tools / "tar").chmod(0o755)

            def pack(takes, largeGid=False, path=None):
                calls.unlink(missing_ok=True)
                env = {"PATH": path or "%s:%s" % (tools, "/usr/bin:/bin"), "WORK": str(work),
                       "TAR_TAKES": takes}
                if largeGid:
                    env["TAR_LARGE_GID"] = "1"
                run = subprocess.run([shutil.which("bash"), "-c", "set -e\n" + functions + "\npack_bundle"],
                                     env=env, capture_output=True, text=True, timeout=20)
                return run.returncode, (calls.read_text().split("\n")[:-1] if calls.exists() else [])

            owner = "--format=ustar --owner=0 --group=0 -cf %s/bundle.tar ." % work
            uid = "--format=ustar --uid=0 --gid=0 -cf %s/bundle.tar ." % work
            plain = "--format=ustar -cf %s/bundle.tar ." % work
            self.assertEqual(pack("owner uid", largeGid=True), (0, [owner]))
            self.assertEqual(pack("uid", largeGid=True), (0, [uid]), "the tar of macOS 10.15 to 15.3")
            self.assertEqual(pack("", largeGid=False), (0, [plain]), "macOS 10.14 and older keep the IDs")
            self.assertEqual(pack("", largeGid=True)[0], 1, "a tar with neither option still fails loudly")

            # The tar of this computer, where it takes either option: every entry is root's.
            real = Path(folder) / "real"
            real.mkdir()
            takesOwner = [subprocess.run(["tar", "--format=ustar", *options, "-cf", str(real / "probe.tar"),
                                          "-C", str(work / "b"), "."],
                                         capture_output=True).returncode == 0
                          for options in (["--owner=0", "--group=0"], ["--uid=0", "--gid=0"])]
            if any(takesOwner):
                self.assertEqual(pack("", path=os.environ["PATH"])[0], 0)
                with tarfile.open(work / "bundle.tar") as bundle:
                    members = bundle.getmembers()
                self.assertTrue(members)
                self.assertEqual({(m.uid, m.gid) for m in members}, {(0, 0)})

    def test_abi_rejections(self):
        for invalid in (b"host Mach-O", elf()[:100], elf(glibc="2.34"),
                        elf(libraries=("libjson-c.so.5",)), elf(libraries=("libstdc++.so.6",))):
            with self.subTest(invalid=invalid[:20]):
                with self.assertRaises(ValueError):
                    runtime.inspect_binary(invalid)
        self.assertIn("libz.so.1", runtime.inspect_binary(
            elf(libraries=("libc.so.6", "libz.so.1")))["needed"])
        for offset, value in ((4, 2), (5, 2), (18, 3)):
            invalid = bytearray(elf())
            invalid[offset] = value
            with self.assertRaises(ValueError):
                runtime.inspect_binary(invalid)

    def test_sdk_shared_libraries_and_licenses_are_staged_recursively(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            build = root / "build"
            sysroot = root / "sdk/sysroot"
            output = root / "runtime/native"
            libraries = sysroot / "usr/lib"
            licenses = sysroot / "usr/share/licenses"
            build.mkdir()
            libraries.mkdir(parents=True)
            (build / "nativetv-daemon").write_bytes(elf(
                libraries=("libc.so.6", "libcurl.so.4"), runpath="$ORIGIN"))
            (build / "nativetv-service").write_bytes(elf())
            curlLibrary = elf(
                libraries=("libssl.so.3", "libcrypto.so.3", "libc.so.6"),
                shared=True, soname="libcurl.so.4")
            (libraries / "libcurl.so.4.8.0").write_bytes(curlLibrary)
            (libraries / "libcurl.so.4").symlink_to("libcurl.so.4.8.0")
            sslLibrary = elf(
                libraries=("libcrypto.so.3", "libc.so.6"),
                shared=True, soname="libssl.so.3")
            (libraries / "libssl.so.3.0.0").write_bytes(sslLibrary)
            (libraries / "libssl.so.3").symlink_to("/usr/lib/libssl.so.3.0.0")
            (libraries / "libcrypto.so.3").write_bytes(elf(
                libraries=("libc.so.6",), shared=True, soname="libcrypto.so.3"))
            (build / "CMakeCache.txt").write_text(
                "NATIVETV_SDK_SYSROOT:PATH=" + str(sysroot) + "\n")
            for package, notice in (("curl", "curl license text"),
                                    ("openssl", "openssl license text")):
                directory = licenses / package
                directory.mkdir(parents=True)
                (directory / "LICENSE").write_text(notice)

            manifest = runtime.stage(build, output)
            self.assertEqual(sorted(manifest["libraries"]), [
                "libcrypto.so.3", "libcurl.so.4", "libssl.so.3"])
            self.assertTrue((output / "libcurl.so.4").is_file())
            self.assertTrue((output / "libssl.so.3").is_file())
            self.assertTrue((output / "libcrypto.so.3").is_file())
            self.assertEqual((output / "libcurl.so.4").read_bytes(), curlLibrary)
            self.assertEqual((output / "libssl.so.3").read_bytes(), sslLibrary)
            self.assertIn("curl license text", (output / "LICENSE-curl.txt").read_text())
            self.assertIn("openssl license text",
                          (output / "LICENSE-openssl.txt").read_text())
            archivePath = root / "runtime.tar.gz"
            runtime.archive_runtime(output, archivePath)
            with tarfile.open(archivePath) as archive:
                names = set(archive.getnames())
            self.assertIn("native/libcurl.so.4", names)
            self.assertIn("native/libcrypto.so.3", names)
            self.assertIn("native/LICENSE-curl.txt", names)
            self.assertIn("native/LICENSE-openssl.txt", names)

    def test_pinned_sdk_license_fallback_checks_version_hash_and_cache(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            sysroot = root / "sdk/arm-webos-linux-gnueabi/sysroot"
            (sysroot / "usr/lib/pkgconfig").mkdir(parents=True)
            (sysroot / "usr/lib/pkgconfig/libcurl.pc").write_text(
                "Name: libcurl\nVersion: test-curl\n")
            (sysroot / "usr/lib/pkgconfig/openssl.pc").write_text(
                "Name: OpenSSL\nVersion: test-openssl\n")
            (root / "sdk/arm-webos-linux-gnueabi/include/c++/12.2.0").mkdir(
                parents=True)
            self.assertEqual(runtime._sdk_package_version(sysroot, "curl"),
                             "test-curl")
            self.assertEqual(runtime._sdk_package_version(sysroot, "openssl"),
                             "test-openssl")
            self.assertEqual(runtime._sdk_package_version(sysroot, "gcc-runtime"),
                             "12.2.0")
            self.assertEqual(runtime._license_package("libatomic.so.1"),
                             "gcc-runtime")

            notice = b"pinned test license notice\n"
            digest = hashlib.sha256(notice).hexdigest()
            source = ("https://example.invalid/COPYING", digest)
            cache = root / "build/.runtime-license-cache"
            with patch.dict(runtime.PINNED_LICENSES,
                            {"curl": {"test-curl": source}}):
                with patch.object(runtime.urllib.request, "urlopen",
                                  return_value=io.BytesIO(notice)) as fetch:
                    result = runtime._sdk_license_notice(sysroot, "curl", cache)
                self.assertIn(source[0].encode(), result)
                self.assertIn(digest.encode(), result)
                self.assertTrue(result.endswith(notice))
                fetch.assert_called_once_with(source[0], timeout=20)

                with patch.object(runtime.urllib.request, "urlopen",
                                  side_effect=AssertionError("cache missed")):
                    cached = runtime._sdk_license_notice(sysroot, "curl", cache)
                self.assertEqual(cached, result)

                with patch.object(runtime.urllib.request, "urlopen",
                                  return_value=io.BytesIO(b"altered notice")):
                    with self.assertRaisesRegex(ValueError, "hash does not match"):
                        runtime._sdk_license_notice(sysroot, "curl", root / "bad-cache")

                (sysroot / "usr/lib/pkgconfig/libcurl.pc").write_text(
                    "Name: libcurl\nVersion: unlisted-curl\n")
                with patch.object(runtime.urllib.request, "urlopen",
                                  side_effect=AssertionError("unknown version fetched")):
                    with self.assertRaisesRegex(ValueError, "no license notice"):
                        runtime._sdk_license_notice(sysroot, "curl", root / "unknown-cache")

    def test_native_client_runtime_bundles_curl_but_uses_tv_sdl_and_gles(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            build = root / "build"
            sysroot = root / "sdk/sysroot"
            library_directory = sysroot / "usr/lib"
            license_directory = sysroot / "usr/share/licenses"
            build.mkdir()
            library_directory.mkdir(parents=True)
            client = elf(libraries=("libc.so.6", "libcurl.so.4",
                                    "libSDL2-2.0.so.0",
                                    "libSDL2_image-2.0.so.0",
                                    "libSDL2_ttf-2.0.so.0", "libGLESv2.so.2",
                                    "libfreetype.so.6"),
                         runpath="$ORIGIN")
            (build / "nativetv-tv").write_bytes(client)
            curl = elf(libraries=("libssl.so.1.1", "libcrypto.so.1.1",
                                  "libatomic.so.1", "libc.so.6"),
                       shared=True, soname="libcurl.so.4")
            ssl = elf(libraries=("libcrypto.so.1.1", "libatomic.so.1",
                                 "libc.so.6"), shared=True,
                      soname="libssl.so.1.1")
            crypto = elf(libraries=("libatomic.so.1", "libc.so.6"),
                         shared=True, soname="libcrypto.so.1.1")
            atomic = elf(libraries=("libc.so.6",), shared=True,
                         soname="libatomic.so.1")
            for soname, data in (("libcurl.so.4", curl),
                                 ("libssl.so.1.1", ssl),
                                 ("libcrypto.so.1.1", crypto),
                                 ("libatomic.so.1", atomic)):
                (library_directory / soname).write_bytes(data)
            for package in ("curl", "openssl", "gcc-runtime"):
                directory = license_directory / package
                directory.mkdir(parents=True)
                (directory / "LICENSE").write_text(package + " notice")

            output = root / "app"
            manifest = runtime.stage_client(build, output, sysroot)
            self.assertEqual(sorted(manifest["libraries"]), [
                "libatomic.so.1", "libcrypto.so.1.1", "libcurl.so.4",
                "libssl.so.1.1"])
            self.assertEqual(manifest["abi"]["binaries"]["nativetv-tv"]["runpath"],
                             "$ORIGIN")
            self.assertTrue((output / "libcurl.so.4").is_file())
            self.assertTrue((output / "libssl.so.1.1").is_file())
            self.assertTrue((output / "libcrypto.so.1.1").is_file())
            self.assertTrue((output / "libatomic.so.1").is_file())
            self.assertFalse((output / "libSDL2-2.0.so.0").exists())
            self.assertFalse((output / "libGLESv2.so.2").exists())
            self.assertFalse((output / "libfreetype.so.6").exists())
            self.assertNotIn("libfreetype.so.6", manifest["libraries"])
            self.assertIn("libfreetype.so.6",
                          manifest["abi"]["binaries"]["nativetv-tv"]["needed"])
            self.assertTrue((output / "LICENSE-curl.txt").is_file())

            # The fonts, icons and licences the client opens are staged beside it
            # and listed, with their hashes, in the manifest the install checks.
            assets = runtime.client_assets()
            listed = {item["path"]: item for item in manifest["files"]}
            for relative in assets:
                staged = output / "assets" / relative
                self.assertEqual(staged.read_bytes(), (ROOT / "assets" / relative).read_bytes())
                self.assertEqual(listed["assets/" + relative]["sha256"],
                                 hashlib.sha256(staged.read_bytes()).hexdigest())
                self.assertEqual(listed["assets/" + relative]["mode"], "0644")
            for required in ("fonts/GoogleSansFlex-Display.ttf", "fonts/GoogleSansFlex-Regular.ttf",
                             "fonts/GoogleSansFlex-Medium.ttf", "fonts/LICENSE-GoogleSansFlex-OFL.txt",
                             "icons/material/LICENSE-MaterialDesignIcons.txt",
                             "icons/splash-mark.svg", "icons/logo.svg", "carousel-scrim.svg",
                             "Roboto-Regular.woff"):
                self.assertIn(required, assets)
            self.assertEqual(listed["nativetv-tv"]["mode"], "0755")
            self.assertTrue((output / "LICENSE-json-c.txt").is_file())
            self.assertIn("LICENSE-json-c.txt", listed)
            self.assertTrue((output / "LICENSE-openssl.txt").is_file())
            self.assertTrue((output / "LICENSE-gcc-runtime.txt").is_file())

            missing_rpath = root / "missing-rpath"
            missing_rpath.mkdir()
            (missing_rpath / "nativetv-tv").write_bytes(
                elf(libraries=("libc.so.6", "libcurl.so.4")))
            with self.assertRaisesRegex(ValueError, r"must have \$ORIGIN"):
                runtime.stage_client(missing_rpath, root / "bad-app", sysroot)

    def test_sysroot_symlinks_cannot_escape_the_root(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            sysroot = root / "sysroot"
            libraryDirectory = sysroot / "usr/lib"
            libraryDirectory.mkdir(parents=True)
            outside = root / "host-library.so"
            outside.write_bytes(elf(shared=True, soname="host-library.so"))
            link = libraryDirectory / "libhost.so"
            link.symlink_to(outside)
            self.assertIsNone(runtime._resolve_in_sysroot(sysroot, link))

    def test_payload_hashes_permissions_and_reproducibility(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            build = root / "build"
            build.mkdir()
            binary = elf(glibc="2.17")
            (build / "nativetv-service").write_bytes(binary)
            (build / "nativetv-daemon").write_bytes(binary)
            output = root / "payload/native"
            manifest = runtime.stage(build, output)
            self.assertEqual(manifest["abi"]["architecture"], "arm")
            self.assertEqual(manifest["abi"]["binaries"]["nativetv-daemon"]["architecture"], "arm")
            service = next(item for item in manifest["files"] if item["path"] == "nativetv-service")
            self.assertEqual(service["sha256"], hashlib.sha256(binary).hexdigest())
            keyFilter = next(item for item in manifest["files"]
                             if item["path"] == "remotekeys-filter.js")
            self.assertEqual(keyFilter["sha256"], hashlib.sha256(
                (ROOT / "assets/remotekeys-filter.js").read_bytes()).hexdigest())
            self.assertEqual(stat.S_IMODE(
                (output / "remotekeys-filter.js").stat().st_mode), 0o644)
            self.assertEqual(stat.S_IMODE((output / "nativetv-service").stat().st_mode), 0o755)
            self.assertEqual(stat.S_IMODE((output / "nativetv-daemon").stat().st_mode), 0o755)
            for name in ("one.tar.gz", "two.tar.gz"):
                runtime.archive_runtime(output, root / name)
            self.assertEqual((root / "one.tar.gz").read_bytes(), (root / "two.tar.gz").read_bytes())
            with tarfile.open(root / "one.tar.gz") as archive:
                self.assertEqual(sorted(archive.getnames()), ["native/LICENSE-civetweb.txt", "native/LICENSE-json-c.txt", "native/nativetv-daemon", "native/nativetv-service", "native/remotekeys-filter.js", "native/runtime.json"])
                self.assertEqual(archive.getmember("native/nativetv-service").mode, 0o755)
                self.assertEqual(archive.getmember("native/nativetv-daemon").mode, 0o755)
            (build / "nativetv-service").write_bytes(b"invalid replacement")
            with self.assertRaises(ValueError):
                runtime.stage(build, output)
            self.assertEqual((output / "nativetv-service").read_bytes(), binary)
            self.assertEqual((output / "nativetv-daemon").read_bytes(), binary)


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.payload = self.root / "app/payload"
        self.source = self.payload / "tvweb"
        self.install = self.root / "installed"
        self.source.mkdir(parents=True)
        self.install.mkdir()
        self.put(self.source / "VERSION", "9.1.0\n")
        # A restart starts a "daemon", a process of its own with a pid file, and writes the
        # heartbeat a server writes once it is up. What the daemon does is what its file
        # says: "broken" never starts, "crashing" dies after a second, "flapping" is
        # started again under another pid after a second (as the watchdog does), "stalled"
        # stops its heartbeat after a second, and any other keeps running.
        self.beat = self.root / "tvweb.beat"
        self.pidfile = self.root / "tvweb.pid"
        self.environment = dict(os.environ, TVWEB_BEAT=str(self.beat), TVWEB_PIDFILE=str(self.pidfile),
                                TVWEB_UP_WAIT="2", TVWEB_UPDATE_SETTLE_SECONDS="3")
        self.put(self.source / "tvwebctl",
                 '#!/bin/sh\nhere=$(dirname "$0")\n'
                 'kind() { grep -q "$1" "$here/native/nativetv-daemon"; }\n'
                 'case "$1" in\n'
                 '  status) echo running ;;\n'
                 '  restart) echo restart >> "$here/commands"; rm -f "$TVWEB_BEAT" "$TVWEB_PIDFILE"\n'
                 '           [ -z "$TVWEB_TEST_RESTART_DELAY" ] || sleep "$TVWEB_TEST_RESTART_DELAY"\n'
                 '           kind broken && exit 0\n'
                 '           life=30; kind crashing && life=1\n'
                 '           sleep "$life" >/dev/null 2>&1 &\n'
                 '           echo $! > "$TVWEB_PIDFILE"; echo $! >> "$here/pids"; echo 1 > "$TVWEB_BEAT"\n'
                 '           kind flapping && ( sleep 1; sleep 30 >/dev/null 2>&1 & echo $! > "$TVWEB_PIDFILE"; echo $! >> "$here/pids" ) >/dev/null 2>&1 &\n'
                 '           kind stalled && ( sleep 1; rm -f "$TVWEB_BEAT" ) >/dev/null 2>&1 &\n'
                 '           exit 0 ;;\n'
                 '  *) echo "$1" >> "$here/commands" ;;\n'
                 'esac\n', 0o755)
        self.put(self.source / "50-tvweb.sh", "#!/bin/sh\n", 0o755)
        self.put(self.source / "assets/dashboard-app/install-app.sh", '#!/bin/sh\necho null\n', 0o755)
        self.put(self.source / "native/nativetv-service", "new runtime", 0o666)
        self.put(self.source / "native/nativetv-daemon", "new daemon", 0o666)
        self.put(self.source / "native/LICENSE-json-c.txt", "notice", 0o666)
        self.put(self.source / "native/LICENSE-civetweb.txt", "civetweb notice", 0o666)
        self.put(self.source / "native/LICENSE-curl.txt", "curl notice", 0o666)
        self.put(self.source / "native/libcurl.so.4", "curl runtime", 0o666)
        self.put(self.source / "native/remotekeys-filter.js", "filter source", 0o666)
        runtime_files = ["nativetv-daemon", "nativetv-service", "LICENSE-json-c.txt",
                         "LICENSE-civetweb.txt", "LICENSE-curl.txt", "libcurl.so.4",
                         "remotekeys-filter.js"]
        manifest = json.dumps({"protocol": 1,
                               "files": [{"path": name} for name in runtime_files]}, indent=2)
        self.put(self.source / "native/runtime.json", manifest + "\n", 0o666)
        script = (ROOT.parent / "hbc/install.sh").read_text()
        script = script.replace("D=/var/lib/tvweb\n", "D=" + str(self.install) + "\n")
        script = script.replace("HOOKDIR=/var/lib/webosbrew/init.d", "HOOKDIR=" + str(self.root / "hooks"))
        self.script = self.payload / "install.sh"
        self.script.write_text(script)

    def tearDown(self):
        # The stand-in daemons are processes of their own.
        try:
            for pid in (self.install / "pids").read_text().split():
                try:
                    os.kill(int(pid), signal.SIGKILL)
                except OSError:
                    pass
        except OSError:
            pass
        self.temporary.cleanup()

    def put(self, path, text, mode=0o644):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        path.chmod(mode)

    def existing(self, version):
        self.put(self.install / "VERSION", version + "\n")
        self.put(self.install / "tvwebctl", (self.source / "tvwebctl").read_text(), 0o755)
        self.put(self.install / "config.json", '{"port":8181,"token":"test-token"}\n', 0o600)
        self.put(self.install / "home.json", '{"profile":"kept"}\n')
        self.put(self.install / "native/nativetv-service", "previous runtime", 0o755)
        self.put(self.install / "native/nativetv-daemon", "previous daemon", 0o755)
        self.put(self.install / "assets/dashboard-app/install-app.sh", '#!/bin/sh\necho null\n', 0o755)

    def run_script(self, *arguments):
        result = subprocess.run(["sh", str(self.script), *arguments], capture_output=True, text=True, check=True,
                                env=self.environment)
        return json.loads(result.stdout.splitlines()[-1]), result

    def run_install(self):
        answer, result = self.run_script()
        self.assertTrue(answer["ok"], result.stdout + result.stderr)
        return answer

    def assert_runtime(self):
        binary = self.install / "native/nativetv-service"
        self.assertEqual(binary.read_text(), "new runtime")
        self.assertEqual(stat.S_IMODE(binary.stat().st_mode), 0o755)
        daemon = self.install / "native/nativetv-daemon"
        self.assertEqual(daemon.read_text(), "new daemon")
        self.assertEqual(stat.S_IMODE(daemon.stat().st_mode), 0o755)
        self.assertEqual((self.install / "native/remotekeys-filter.js").read_text(),
                         "filter source")
        self.assertEqual(stat.S_IMODE(
            (self.install / "native/remotekeys-filter.js").stat().st_mode), 0o644)
        self.assertEqual((self.install / "native/libcurl.so.4").read_text(), "curl runtime")
        self.assertEqual((self.install / "native/LICENSE-curl.txt").read_text(), "curl notice")
        self.assertEqual(stat.S_IMODE(
            (self.install / "native/libcurl.so.4").stat().st_mode), 0o644)

    def test_first_install(self):
        self.assertEqual(self.run_install()["action"], "installed")
        self.assert_runtime()
        self.assertEqual(json.loads((self.install / "config.json").read_text()), {"host": "127.0.0.1"})

    def test_upgrade_preserves_state(self):
        self.existing("9.0.0")
        before = [(self.install / name).read_bytes() for name in ("config.json", "home.json")]
        self.assertEqual(self.run_install()["action"], "upgraded")
        self.assert_runtime()
        self.assertEqual(before, [(self.install / name).read_bytes() for name in ("config.json", "home.json")])

    def test_upgrade_is_reported_once_the_new_server_is_up_and_keeps_no_copy(self):
        self.existing("9.0.0")
        started = time.monotonic()
        self.assertEqual(self.run_install()["action"], "upgraded")
        # A server that has written its heartbeat is up only once it has gone on running.
        self.assertGreaterEqual(time.monotonic() - started, 3)
        self.assertTrue(self.beat.exists())
        self.assertFalse((self.install / ".previous").exists())
        self.assertFalse((self.install / ".update-failed").exists())

    def test_new_server_that_does_not_start_is_replaced_by_the_one_it_replaced(self):
        self.existing("9.0.0")
        self.put(self.source / "native/nativetv-daemon", "broken daemon", 0o666)
        self.put(self.install / "assets/page.html", "old page")
        self.put(self.source / "assets/page.html", "new page")
        before = {name: (self.install / name).read_bytes()
                  for name in ("config.json", "home.json", "native/nativetv-daemon",
                               "native/nativetv-service", "assets/page.html", "VERSION")}
        answer, _ = self.run_script()
        self.assertFalse(answer["ok"])
        self.assertIn("did not start", answer["error"])
        # Every file the update replaced is back, the data was never touched, and the
        # old server was started again after the new one failed to come up.
        self.assertEqual(before, {name: (self.install / name).read_bytes() for name in before})
        self.assertEqual((self.install / "commands").read_text().split(), ["restart", "restart"])
        self.assertTrue(self.beat.exists(), "the restored server was started")
        failed = json.loads((self.install / ".update-failed").read_text())
        self.assertEqual(failed["version"], "9.1.0")
        self.assertIn("version 9.0.0 was put back", failed["error"])
        self.assertFalse((self.install / ".hbc-stage").exists())

    def test_a_server_that_starts_and_then_stops_is_replaced_by_the_one_it_replaced(self):
        # It writes its heartbeat and is running when the script looks, and then dies, is
        # started again under another pid, or stops writing: none of those is up.
        for kind in ("crashing", "flapping", "stalled"):
            with self.subTest(kind=kind):
                self.existing("9.0.0")
                (self.install / "commands").unlink(missing_ok=True)
                (self.install / ".update-failed").unlink(missing_ok=True)
                self.put(self.source / "native/nativetv-daemon", kind + " daemon", 0o666)
                answer, _ = self.run_script()
                self.assertFalse(answer["ok"], answer)
                self.assertIn("did not start", answer["error"])
                self.assertEqual((self.install / "native/nativetv-daemon").read_text(), "previous daemon")
                self.assertEqual((self.install / "VERSION").read_text().strip(), "9.0.0")
                self.assertEqual((self.install / "commands").read_text().split(), ["restart", "restart"])
                failed = json.loads((self.install / ".update-failed").read_text())
                self.assertEqual(failed["version"], "9.1.0")
                self.assertIn("version 9.0.0 was put back", failed["error"])

    def test_opening_the_app_does_not_try_an_update_that_was_put_back_again(self):
        self.existing("9.0.0")
        self.put(self.source / "native/nativetv-daemon", "broken daemon", 0o666)
        answer, _ = self.run_script()
        self.assertFalse(answer["ok"])
        # The launch page runs it with no argument each time the app is opened: the old
        # server is left running, and no attempt is made over again.
        (self.install / "commands").unlink()
        failed = (self.install / ".update-failed").read_text()
        answer, _ = self.run_script()
        self.assertTrue(answer["ok"], answer)
        self.assertEqual((answer["action"], answer["version"]), ("started", "9.0.0"))
        self.assertEqual((self.install / "native/nativetv-daemon").read_text(), "previous daemon")
        self.assertFalse((self.install / "commands").exists(), "nothing was restarted")
        self.assertEqual((self.install / ".update-failed").read_text(), failed)
        # The dashboard's Install passes `update`, and tries again, and a newer package is not held back.
        self.put(self.source / "native/nativetv-daemon", "new daemon", 0o666)
        answer, _ = self.run_script("update")
        self.assertEqual(answer["action"], "upgraded")
        self.assertFalse((self.install / ".update-failed").exists())

    def test_a_package_newer_than_the_one_that_was_put_back_is_installed_on_opening_the_app(self):
        self.existing("9.0.0")
        self.put(self.source / "native/nativetv-daemon", "broken daemon", 0o666)
        self.assertFalse(self.run_script()[0]["ok"])
        self.put(self.source / "VERSION", "9.2.0\n")
        self.put(self.source / "native/nativetv-daemon", "new daemon", 0o666)
        self.assertEqual(self.run_script()[0]["action"], "upgraded")
        self.assertEqual((self.install / "VERSION").read_text().strip(), "9.2.0")

    def test_the_files_an_update_replaces_are_kept_by_link_not_by_copy(self):
        # The old files are held where they are, which costs no room while both versions are on
        # the flash; the new ones go in by rename and leave them alone. After a rollback, which
        # leaves the kept files in place, they are still the very files that were installed.
        self.existing("9.0.0")
        self.put(self.source / "native/nativetv-daemon", "broken daemon", 0o666)
        installed = {name: (self.install / name).stat().st_ino
                     for name in ("native/nativetv-daemon", "native/nativetv-service", "VERSION", "config.json")}
        self.assertFalse(self.run_script()[0]["ok"])
        previous = self.install / ".previous"
        for name in ("native/nativetv-daemon", "native/nativetv-service", "VERSION"):
            self.assertEqual((previous / name).stat().st_ino, installed[name], name)
        # Data is not among the files kept.
        self.assertFalse((previous / "config.json").exists())
        self.assertEqual((previous / "native/nativetv-daemon").read_text(), "previous daemon")
        self.assertEqual((self.install / "native/nativetv-daemon").read_text(), "previous daemon")

    def without_hard_links(self):
        shim = self.root / "shim"
        shim.mkdir(exist_ok=True)
        # A `ln` that makes symbolic links, for the hook and the lock, and no other.
        self.put(shim / "ln", '#!/bin/sh\ncase "$1" in -*) exec /bin/ln "$@" ;; *) exit 1 ;; esac\n', 0o755)
        self.environment["PATH"] = str(shim) + os.pathsep + self.environment["PATH"]

    def test_the_files_are_copied_where_a_link_cannot_be_made(self):
        self.without_hard_links()
        self.existing("9.0.0")
        self.put(self.source / "native/nativetv-daemon", "broken daemon", 0o666)
        inode = (self.install / "native/nativetv-daemon").stat().st_ino
        answer, _ = self.run_script()
        self.assertFalse(answer["ok"])
        self.assertIn("did not start", answer["error"])
        previous = self.install / ".previous/native/nativetv-daemon"
        self.assertEqual(previous.read_text(), "previous daemon")
        self.assertNotEqual(previous.stat().st_ino, inode)
        self.assertEqual((self.install / "native/nativetv-daemon").read_text(), "previous daemon")

    def test_update_that_cannot_keep_the_old_version_replaces_nothing(self):
        if os.geteuid() == 0:
            self.skipTest("root reads any file")
        # Neither a link nor a copy of the file can be made.
        self.without_hard_links()
        self.existing("9.0.0")
        unreadable = self.install / "native/nativetv-daemon"
        unreadable.chmod(0)
        try:
            answer, _ = self.run_script()
        finally:
            unreadable.chmod(0o755)
        self.assertFalse(answer["ok"])
        self.assertIn("keep the version being replaced", answer["error"])
        self.assertEqual((self.install / "VERSION").read_text().strip(), "9.0.0")
        self.assertEqual(unreadable.read_text(), "previous daemon")
        self.assertFalse((self.install / "commands").exists(), "the server was not restarted")
        self.assertFalse((self.install / ".hbc-stage").exists())
        self.assertFalse((self.install / ".previous").exists())

    def wait_for_lock(self):
        lock = self.install / ".install.lock"
        deadline = time.monotonic() + 10
        while not os.path.lexists(lock) and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertTrue(os.path.lexists(lock), "the run never took the lock")
        return lock

    def boot_id(self):
        try:
            return Path("/proc/sys/kernel/random/boot_id").read_text().strip()
        except OSError:
            return ""

    def test_a_second_run_does_not_touch_an_install_that_is_being_made(self):
        # The launch page's run starts while the dashboard's is still waiting for its server.
        self.existing("9.0.0")
        self.environment["TVWEB_TEST_RESTART_DELAY"] = "2"
        first = subprocess.Popen(["sh", str(self.script)], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 text=True, env=self.environment)
        lock = self.wait_for_lock()
        answer, _ = self.run_script()
        self.assertEqual(answer, {"ok": False, "error": "an install is already running"})
        output, _ = first.communicate(timeout=30)
        self.assertEqual(json.loads(output.splitlines()[-1])["action"], "upgraded")
        # One swap and one restart, and the lock goes when the run does.
        self.assertEqual((self.install / "commands").read_text().split(), ["restart"])
        self.assertFalse(os.path.lexists(lock))
        self.assert_runtime()

    def test_a_lock_whose_holder_is_gone_does_not_block_an_install(self):
        self.existing("9.0.0")
        gone = subprocess.Popen(["sh", "-c", "exit 0"])
        gone.wait()
        os.symlink("%d:%s" % (gone.pid, self.boot_id()), self.install / ".install.lock")
        self.assertEqual(self.run_install()["action"], "upgraded")
        self.assertFalse(os.path.lexists(self.install / ".install.lock"))

    def test_a_pid_from_before_a_reboot_does_not_hold_the_lock(self):
        # After a power cut the pid in the lock may belong to something else now.
        self.existing("9.0.0")
        bystander = subprocess.Popen(["sleep", "30"])
        try:
            os.symlink("%d:another-boot" % bystander.pid, self.install / ".install.lock")
            self.assertEqual(self.run_install()["action"], "upgraded")
        finally:
            bystander.kill()
            bystander.wait()

    def test_a_lock_held_by_a_live_run_is_left_alone(self):
        self.existing("9.0.0")
        holder = subprocess.Popen(["sleep", "30"])
        target = "%d:%s" % (holder.pid, self.boot_id())
        try:
            os.symlink(target, self.install / ".install.lock")
            answer, _ = self.run_script()
            self.assertEqual(answer, {"ok": False, "error": "an install is already running"})
            self.assertEqual(os.readlink(self.install / ".install.lock"), target)
            self.assertEqual((self.install / "VERSION").read_text().strip(), "9.0.0")
            self.assertFalse((self.install / "commands").exists())
        finally:
            holder.kill()
            holder.wait()

    def test_the_lock_is_released_when_a_run_fails_or_is_stopped(self):
        self.existing("9.0.0")
        self.put(self.source / "native/nativetv-daemon", "broken daemon", 0o666)
        answer, _ = self.run_script()
        self.assertFalse(answer["ok"])
        self.assertFalse(os.path.lexists(self.install / ".install.lock"), "after a rollback")
        # A run that is told to stop, while its server restarts, puts nothing in the way of the next.
        self.put(self.source / "native/nativetv-daemon", "new daemon", 0o666)
        self.environment["TVWEB_TEST_RESTART_DELAY"] = "1"
        run = subprocess.Popen(["sh", str(self.script), "update"], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, env=self.environment)
        lock = self.wait_for_lock()
        run.terminate()
        run.communicate(timeout=30)
        self.assertFalse(os.path.lexists(lock), "after a signal")
        time.sleep(1.5)  # the stand-in restart that was under way finishes before the folder goes

    def test_a_new_attempt_forgets_the_failure_before_it(self):
        self.existing("9.0.0")
        self.put(self.install / ".update-failed", '{"version":"9.1.0","error":"x"}\n')
        answer, _ = self.run_script("update")
        self.assertEqual(answer["action"], "upgraded")
        self.assertFalse((self.install / ".update-failed").exists())

    def test_upgrade_from_the_node_server_clears_its_files(self):
        # An install from before the daemon was the whole server: its version is in
        # tvweb.js, and its lib/ goes.
        self.existing("9.0.0")
        (self.install / "VERSION").unlink()
        self.put(self.install / "tvweb.js", "var TVWEB_VERSION = '9.0.0';\n")
        self.put(self.install / "lib/ha.js", "old module")
        self.assertEqual(self.run_install()["action"], "upgraded")
        self.assertFalse((self.install / "tvweb.js").exists())
        self.assertFalse((self.install / "lib").exists())
        self.assertEqual((self.install / "VERSION").read_text().strip(), "9.1.0")
        self.assert_runtime()

    def test_same_version_repairs_worker_without_restart(self):
        self.existing("9.1.0")
        self.assertEqual(self.run_install()["action"], "started")
        self.assert_runtime()
        self.assertFalse((self.install / "commands").exists())

    def test_older_app_preserves_newer_runtime(self):
        self.existing("10.0.0")
        self.assertEqual(self.run_install()["action"], "started")
        self.assertEqual((self.install / "native/nativetv-service").read_text(), "previous runtime")
        self.assertEqual((self.install / "native/nativetv-daemon").read_text(), "previous daemon")

    def test_incomplete_runtime_does_not_touch_existing_install(self):
        self.existing("9.0.0")
        before = (self.install / "VERSION").read_bytes()
        for missing in ("runtime.json", "nativetv-service", "nativetv-daemon", "LICENSE-json-c.txt", "LICENSE-civetweb.txt", "remotekeys-filter.js", "LICENSE-curl.txt", "libcurl.so.4"):
            with self.subTest(missing=missing):
                source = self.source / "native" / missing
                content = source.read_bytes()
                source.unlink()
                result = subprocess.run(["sh", str(self.script)], capture_output=True, text=True, check=True,
                                        env=self.environment)
                self.assertFalse(json.loads(result.stdout.splitlines()[-1])["ok"])
                self.assertEqual((self.install / "VERSION").read_bytes(), before)
                self.assertEqual((self.install / "native/nativetv-service").read_text(), "previous runtime")
                self.assertEqual((self.install / "native/nativetv-daemon").read_text(), "previous daemon")
                source.write_bytes(content)

    def test_absent_runtime_does_not_touch_existing_install(self):
        self.existing("9.0.0")
        before = (self.install / "VERSION").read_bytes()
        shutil.rmtree(self.source / "native")
        result = subprocess.run(["sh", str(self.script)], capture_output=True, text=True, check=True,
                                env=self.environment)
        self.assertFalse(json.loads(result.stdout.splitlines()[-1])["ok"])
        self.assertEqual((self.install / "VERSION").read_bytes(), before)
        self.assertEqual((self.install / "native/nativetv-service").read_text(), "previous runtime")
        self.assertEqual((self.install / "native/nativetv-daemon").read_text(), "previous daemon")


if __name__ == "__main__":
    unittest.main()
