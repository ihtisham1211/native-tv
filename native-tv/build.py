#!/usr/bin/env python3
"""Build the native TV client for the host or a relocated webOSbrew SDK."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, help="Relocated webOSbrew native SDK")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--test", action="store_true")
    parser.add_argument("--target", action="append", default=[],
                        help="Build only this CMake target; may be repeated")
    parser.add_argument("--test-filter",
                        help="Run CTest tests matching this regular expression")
    # The daemon tests mostly wait on timers and child processes (about 25% CPU
    # at 8 jobs on 8 cores), so twice the cores finishes sooner: 117 s at 8 jobs
    # and 69 s at 16 on an 8-core Mac, all 220 passing.
    parser.add_argument("--test-jobs", type=int,
                        default=min((os.cpu_count() or 4) * 2, 16),
                        help="Maximum concurrent CTest targets (default: twice the available CPUs, capped at 16)")
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
        if not (sdk / "bin/arm-webos-linux-gnueabi-g++").is_file():
            parser.error("The SDK has no ARM C++ compiler")
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
