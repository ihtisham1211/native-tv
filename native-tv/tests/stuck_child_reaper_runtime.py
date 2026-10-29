#!/usr/bin/env python3
import os
from pathlib import Path
import signal
import shlex
import subprocess
import sys
import tempfile
import time


def write_process(proc_root, pid, parent_pid, executable, command_line):
    process = proc_root / str(pid)
    process.mkdir(parents=True, exist_ok=True)
    (process / "exe").symlink_to(executable)
    (process / "cmdline").write_bytes(command_line)
    (process / "stat").write_text(
        f"{pid} (node test process) S {parent_pid} 0 0 0 0 0 0\n")
    (process / "syscall").write_text("202 0 0 0 0 0 0 0 0x1000\n")
    (process / "maps").write_text(
        "1000-2000 r-xp 00000010 00:00 0 /usr/lib/libnode.so\n")


def spawn_waiting_child():
    pid = os.fork()
    if pid == 0:
        while True:
            time.sleep(10)
    return pid


def wait_for_child(pid):
    try:
        return os.waitpid(pid, 0)[1]
    except ChildProcessError:
        return None


def run_tvwebctl_reaper(binary, proc_root, parent_pid, restarts, fallback):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "native-tv/lifecycle/tvwebctl").read_text()
    begin = source.index("free_stuck_children() {")
    end = source.index("\n}\n", begin) + 2
    function = source[begin:end]
    assignments = "\n".join([
        "SERVICE=" + shlex.quote(str(binary)),
        "PROC_ROOT=" + shlex.quote(str(proc_root)),
        "STUCK_SETTLE_MS=0",
        "RESTARTS=" + shlex.quote(str(restarts)),
        "free_stuck_children_compat() { echo fallback >> " +
        shlex.quote(str(fallback)) + "; }",
    ])
    script = assignments + "\n" + function + "\nfree_stuck_children " + str(parent_pid) + "\n"
    return subprocess.run(["/bin/sh", "-c", script], capture_output=True,
                          text=True, timeout=5)


def main():
    binary = Path(sys.argv[1]).resolve()
    parent_pid = os.getpid()
    executable = Path(sys.executable).resolve()
    with tempfile.TemporaryDirectory(prefix="nativetv-stuck-child-") as folder:
        proc_root = Path(folder) / "proc"
        proc_root.mkdir()
        write_process(proc_root, parent_pid, 1, executable, b"python\0test\0")
        restart_log = Path(folder) / "watchdog.restarts"
        fallback_log = Path(folder) / "watchdog.fallback"

        stuck_pid = spawn_waiting_child()
        try:
            write_process(proc_root, stuck_pid, parent_pid, executable,
                          b"/usr/bin/node\0")
            result = run_tvwebctl_reaper(binary, proc_root, parent_pid,
                                         restart_log, fallback_log)
            if result.returncode != 0:
                raise AssertionError(result.stderr or result.stdout)
            expected = f"child {stuck_pid} stuck before exec"
            watchdog_log = restart_log.read_text()
            if expected not in watchdog_log or "libnode.so+0x10" not in watchdog_log:
                raise AssertionError("missing reaper output: " + watchdog_log)
            if fallback_log.exists():
                raise AssertionError("the shell compatibility detector ran with service installed")
            status = wait_for_child(stuck_pid)
            if status is None or not os.WIFSIGNALED(status) or os.WTERMSIG(status) != signal.SIGKILL:
                raise AssertionError("the matching child was not killed with SIGKILL")
        finally:
            try:
                os.kill(stuck_pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                os.waitpid(stuck_pid, os.WNOHANG)
            except ChildProcessError:
                pass

        excluded_pid = spawn_waiting_child()
        try:
            if restart_log.exists():
                restart_log.unlink()
            write_process(proc_root, excluded_pid, parent_pid, executable,
                          b"/usr/bin/node\0remotebuttons\0")
            result = run_tvwebctl_reaper(binary, proc_root, parent_pid,
                                         restart_log, fallback_log)
            if result.returncode != 0:
                raise AssertionError(result.stderr or result.stdout)
            if restart_log.exists() and restart_log.read_text():
                raise AssertionError("remote-button helper was reaped: " + restart_log.read_text())
            waited, _ = os.waitpid(excluded_pid, os.WNOHANG)
            if waited:
                raise AssertionError("remote-button helper exited unexpectedly")
        finally:
            try:
                os.kill(excluded_pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                os.waitpid(excluded_pid, 0)
            except ChildProcessError:
                pass

    print("native stuck-child reaper runtime passed")


if __name__ == "__main__":
    main()
