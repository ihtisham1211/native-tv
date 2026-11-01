#!/usr/bin/env python3
"""Build the native TV client for the host or a relocated webOSbrew SDK."""
import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent

# The pkg-config names of the libraries a host build links, as
# (module, Homebrew package, Debian/Ubuntu package). A package of None is part
# of the system on that platform.
SERVICE_LIBRARIES = [("json-c", "json-c", "libjson-c-dev"),
                     ("libcurl", None, "libcurl4-openssl-dev")]
CLIENT_LIBRARIES = [("sdl2", "sdl2", "libsdl2-dev"),
                    ("SDL2_image", "sdl2_image", "libsdl2-image-dev"),
                    ("SDL2_ttf", "sdl2_ttf", "libsdl2-ttf-dev"),
                    ("freetype2", "freetype", "libfreetype6-dev")]
GLES_LIBRARY = ("glesv2", None, "libgles2-mesa-dev")  # the client links it on Linux; macOS has OpenGL
# Tools a build is faster or more complete with, as (command, what it gives,
# Homebrew package, Debian/Ubuntu package). Only --check mentions them.
OPTIONAL_TOOLS = [("ccache", "faster rebuilds", "ccache", "ccache"),
                  ("node", "3 more tests, skipped without it", "node", "nodejs"),
                  ("openssl", "1 more test, skipped without it", None, "openssl")]


def has_library(module):
    return subprocess.run(["pkg-config", "--exists", module], stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


def missing_prerequisites(args):
    """What the build needs that this computer lacks, as ([(name, brew, apt)], [(brew, apt)]).

    The second list is the libraries that could not be looked for because
    pkg-config is missing: they go into the install commands but not the
    headline, so one round of installing is enough.
    """
    missing, unchecked = [], []

    def need(present, name, brew, apt):
        if not present:
            missing.append((name, brew, apt))

    need(shutil.which("cmake"), "cmake", "cmake", "cmake")
    need(shutil.which("pkg-config"), "pkg-config", "pkgconf", "pkg-config")
    # CMake uses Ninja when it finds it and make otherwise; macOS has make and a bare Debian does not.
    need(shutil.which("ninja") or shutil.which("make"), "ninja or make", "ninja", "ninja-build")
    if args.sdk:
        return missing, unchecked  # the SDK brings its own compiler and libraries
    need(os.environ.get("CXX") or any(shutil.which(c) for c in ("c++", "g++", "clang++")),
         "a C++ compiler", None, "g++")
    libraries = SERVICE_LIBRARIES if args.service_only else SERVICE_LIBRARIES + CLIENT_LIBRARIES
    if not args.service_only and platform.system() != "Darwin":
        libraries = libraries + [GLES_LIBRARY]
    for module, brew, apt in libraries:
        if shutil.which("pkg-config"):
            need(has_library(module), module, brew, apt)
        else:
            unchecked.append((brew, apt))
    return missing, unchecked


def install_commands(missing, unchecked):
    """The one-line install command for macOS (Homebrew) and for Debian/Ubuntu."""
    def unique(items):
        return list(dict.fromkeys(i for i in items if i))
    brew = unique([b for _, b, _ in missing] + [b for b, _ in unchecked])
    apt = unique([a for _, _, a in missing] + [a for _, a in unchecked])
    mac = ["xcode-select --install"] if any(name == "a C++ compiler" for name, _, _ in missing) else []
    if brew:
        mac.append("brew install " + " ".join(brew))
    return " && ".join(mac) or "nothing to install", "sudo apt install " + " ".join(apt)


def check_sdk(parser, sdk):
    if not (sdk / "bin/arm-webos-linux-gnueabi-g++").is_file():
        parser.error(str(sdk) + " has no ARM C++ compiler. --sdk takes the unpacked webOSbrew native SDK "
                     "(its folder is arm-webos-linux-gnueabi_sdk-buildroot); "
                     "docs/getting-started/install.md, \"Get the SDK\", has the steps.")
    location = sdk / "share/buildroot/sdk-location"
    # The SDK records the folder it was built in, and the compiler stops working until
    # relocate-sdk.sh points the record at the folder it is in now.
    if location.is_file() and not Path(location.read_text().strip()).is_dir():
        parser.error("The SDK at " + str(sdk) + " has not been relocated yet. Run "
                     + str(sdk / "relocate-sdk.sh") + " once, then build again.")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog="A host build needs cmake, pkg-config, a C++14 compiler, json-c and libcurl (the "
               "client adds SDL2, SDL2_image, SDL2_ttf and FreeType); an SDK build needs cmake "
               "and pkg-config. --check says what is missing and how to install it.")
    parser.add_argument("--sdk", type=Path, help="Relocated webOSbrew native SDK")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--check", action="store_true",
                        help="Only report what this computer lacks for the build, and how to install it")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--test", action="store_true")
    parser.add_argument("--target", action="append", default=[],
                        help="Build only this CMake target; may be repeated")
    parser.add_argument("--test-filter",
                        help="Run CTest tests matching this regular expression")
    # The daemon tests mostly wait on timers and child processes (about 25% CPU
    # at 8 jobs on 8 cores), so twice the cores finishes sooner on a computer: 117 s
    # at 8 jobs and 69 s at 16 on an 8-core Mac, all 220 passing. CI keeps one job
    # per core: at twice that, native-state-subscriptions aborted on a runner
    # (a timing-dependent failure that one job per core had not shown).
    cores = os.cpu_count() or 4
    default_test_jobs = min(cores, 8) if os.environ.get("CI") else min(cores * 2, 16)
    parser.add_argument("--test-jobs", type=int,
                        default=default_test_jobs,
                        help="Maximum concurrent CTest targets (default: twice the available CPUs, "
                             "capped at 16; one per CPU, capped at 8, when CI is set)")
    parser.add_argument("--build-jobs", type=int,
                        default=min(os.cpu_count() or 2, 8),
                        help="Maximum concurrent build jobs (default: available CPUs, capped at 8)")
    parser.add_argument("--no-ccache", action="store_true",
                        help="Do not use ccache even when it is installed")
    parser.add_argument("--service-only", action="store_true", help="Build the daemon and metrics worker without SDL/OpenGL dependencies")
    args = parser.parse_args()
    if args.test_jobs < 1 or args.test_jobs > 32:
        parser.error("--test-jobs must be between 1 and 32")
    if args.build_jobs < 1 or args.build_jobs > 32:
        parser.error("--build-jobs must be between 1 and 32")
    if args.target and args.test and not args.test_filter:
        parser.error("--target with --test requires --test-filter to avoid running unbuilt tests")
    if args.sdk and (args.test or args.test_filter):
        parser.error("Cross-compiled tests must run on the TV; use a host build for tests")
    missing, unchecked = missing_prerequisites(args)
    if missing:
        mac, debian = install_commands(missing, unchecked)
        library_modules = [module for module, _, _ in SERVICE_LIBRARIES + CLIENT_LIBRARIES + [GLES_LIBRARY]]
        pkg_config_hint = ("\nA library installed where pkg-config does not look is found by setting "
                           "PKG_CONFIG_PATH." if any(name in library_modules for name, _, _ in missing) else "")
        parser.exit(2, "Cannot build yet. Missing: " + ", ".join(name for name, _, _ in missing)
                    + ".\n\n  macOS (Homebrew)  " + mac + "\n  Debian/Ubuntu     " + debian + "\n"
                    + pkg_config_hint + "\nRun with --check to look again without building.\n")
    if args.sdk:
        check_sdk(parser, args.sdk.resolve())
    if args.check:
        print("This computer has what the " + ("SDK " if args.sdk else "host ")
              + "build needs.")
        if not args.sdk:
            for tool, gives, brew, apt in OPTIONAL_TOOLS:
                if not shutil.which(tool):
                    print("Optional, not installed: " + tool + " (" + gives + ").  "
                          + (("brew install " + brew + "  or  ") if brew else "") + "sudo apt install " + apt)
        return
    if args.out:
        output = args.out.resolve()
    elif args.sdk:
        output = ROOT / "build-webos"
    else:
        # Keep the legacy Make build tree available, but make the default
        # development loop use Ninja when it is installed.
        output = ROOT / ("build-host-ninja" if shutil.which("ninja") else "build-host")
    output = output.resolve()
    env = os.environ.copy()
    command = ["cmake", "-S", str(ROOT), "-B", str(output), "-DCMAKE_BUILD_TYPE=Debug" if args.sanitize else "-DCMAKE_BUILD_TYPE=Release"]
    command += ["-DNATIVETV_BUILD_CLIENT=" + ("OFF" if args.service_only else "ON")]
    # CMake rejects a different generator for an existing build directory, so
    # only a fresh one switches; older ones keep the generator they were made with.
    if shutil.which("ninja") and not (output / "CMakeCache.txt").exists():
        command += ["-G", "Ninja"]
    ccache = None if args.no_ccache else shutil.which("ccache")
    if ccache:
        command += ["-DCMAKE_C_COMPILER_LAUNCHER=" + ccache,
                    "-DCMAKE_CXX_COMPILER_LAUNCHER=" + ccache]
        # Make cache hits portable across build directories while keeping the
        # working directory out of the hash.
        env.setdefault("CCACHE_BASEDIR", str(ROOT.parent))
        env.setdefault("CCACHE_NOHASHDIR", "true")
    if args.sdk:
        sdk = args.sdk.resolve()
        sysroot = sdk / "arm-webos-linux-gnueabi/sysroot"
        command += ["-DCMAKE_TOOLCHAIN_FILE=" + str(sdk / "share/buildroot/toolchainfile.cmake"),
                    "-DPKG_CONFIG_EXECUTABLE=" + shutil.which("pkg-config"),
                    "-DNATIVETV_SDK_SYSROOT=" + str(sysroot), "-DBUILD_TESTING=OFF"]
        env["PKG_CONFIG_SYSROOT_DIR"] = str(sysroot)
        env["PKG_CONFIG_LIBDIR"] = str(sysroot / "usr/lib/pkgconfig") + ":" + str(sysroot / "usr/share/pkgconfig")
    if args.sanitize:
        command += ["-DNATIVETV_SANITIZE=ON"]
    subprocess.run(command, env=env, check=True)
    build_command = ["cmake", "--build", str(output), "--parallel", str(args.build_jobs)]
    if args.target:
        build_command += ["--target"] + args.target
    subprocess.run(build_command, env=env, check=True)
    if args.test or args.test_filter:
        test_command = ["ctest", "--test-dir", str(output), "--output-on-failure",
                        "--parallel", str(args.test_jobs)]
        if args.test_filter:
            test_command += ["-R", args.test_filter]
        subprocess.run(test_command, check=True)
    if args.target:
        print("Built targets: " + ", ".join(args.target))
    else:
        print(output / ("nativetv-daemon" if args.service_only else "nativetv-tv"))


if __name__ == "__main__":
    main()
