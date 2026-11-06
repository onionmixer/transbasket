"""PLAN_OPEN_ISSUES.md A: -d reports a failed start, keeps cwd/umask, -l log file.

    python3 -I tests/test_daemon.py <transbasket> <transbasket built with -DDAEMON_READY_TIMEOUT=3>

Runs inside an unprivileged network + PID namespace (--mount-proc) so every daemon,
which detaches from its launcher by design, dies with the namespace.
"""
import os
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INNER = "--inner"
FAILURES = []

FAKE_CODEX = """#!/usr/bin/env python3
import os, sys, time
if "--version" in sys.argv:
    time.sleep(float(os.environ.get("FAKE_SLEEP", "0")))
    print("codex-cli 0.166.0")
elif "--help" in sys.argv: print("exec --ephemeral --ignore-user-config -o")
elif sys.argv[1:3] == ["mcp", "list"]: print("[]")
"""


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        FAILURES.append(what)


def daemons(binary):
    found = []
    for d in Path("/proc").iterdir():
        if d.name.isdigit():
            try:
                if os.readlink(d / "exe") == binary and (d / "stat").read_text().split()[2] != "Z":
                    found.append(int(d.name))
            except OSError:
                pass
    return found


def reap():
    """We are pid 1 of the namespace: collect orphaned daemons that exited."""
    while True:
        try:
            pid, _ = os.waitpid(-1, os.WNOHANG)
        except ChildProcessError:
            return
        if pid == 0:
            return


def kill_all(binary):
    for pid in daemons(binary):
        os.kill(pid, signal.SIGKILL)
    time.sleep(0.2)
    reap()


def health(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as s:
            s.sendall(b"GET /health HTTP/1.0\r\n\r\n")
            return b" 200 " in s.recv(200).split(b"\r\n", 1)[0]
    except OSError:
        return False


def wait_health(port, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if health(port):
            return True
        time.sleep(0.1)
    return False


def make_conf(d, port, **over):
    (d / "role.txt").write_text("role")
    v = {"OPENAI_BASE_URL": "http://127.0.0.1:9/v1", "OPENAI_MODEL": "m", "OPENAI_API_KEY": "k", "LISTEN": "127.0.0.1",
         "PORT": str(port), "PROMPT_PREFIX_FILE": str(ROOT / "PROMPT_PREFIX.txt"),
         "LANGUAGE_TABLE_FILE": str(ROOT / "language_tables.conf"), "TRANS_CACHE_TYPE": "text",
         "TRANS_CACHE_FILE": "rel_cache.jsonl"}
    v.update(over)
    (d / "t.conf").write_text("".join(f'{k}="{x}"\n' for k, x in v.items()))
    return d / "t.conf"


def launch(binary, d, conf, extra=(), env=None, stderr=None, umask=None):
    t0 = time.time()

    def pre():
        if umask is not None:
            os.umask(umask)
    p = subprocess.run([binary, "-d", "-c", str(conf), "-r", "role.txt", *extra], cwd=d, capture_output=stderr is None,
                       stderr=stderr, stdout=subprocess.DEVNULL if stderr is not None else None, text=True,
                       env={**os.environ, **(env or {})}, preexec_fn=pre, timeout=120)
    out = (p.stdout or "") + (p.stderr or "") if stderr is None else ""
    return p.returncode, out, time.time() - t0


def rows(binary, short):
    subprocess.run(["ip", "link", "set", "lo", "up"], check=True)
    work = Path(tempfile.mkdtemp(prefix="daemon-"))
    port = 18893
    try:
        # failed starts: the launcher must say so
        for name, over, needle in [("invalid LISTEN", {"LISTEN": "abc"}, 'Error: LISTEN "abc"'),
                                   ("missing prompt", {"PROMPT_PREFIX_FILE": "/nonexistent/p.txt"}, "Error: "),
                                   ("port in use", {}, "Address already in use"),
                                   ("removed CODEX_INTERFACE=mcp", {"MODEL_TYPE": "codex", "CODEX_INTERFACE": "mcp"},
                                    'Error: CODEX_INTERFACE "mcp" was removed')]:
            d = Path(tempfile.mkdtemp(dir=work))
            conf = make_conf(d, port, **over)
            blocker = None
            if name == "port in use":
                blocker = socket.socket()
                blocker.bind(("127.0.0.1", port))
                blocker.listen(1)
            rc, out, _ = launch(binary, d, conf)
            if blocker:
                blocker.close()
            time.sleep(0.3)
            reap()
            check(rc == 1 and needle in out and "the daemon failed to start" in out and not daemons(binary),
                  f"{name}: launcher exit 1 with the reason, nothing left running (rc={rc})")

        # a good start: exit 0, cwd and umask kept, relative cache path as in the foreground
        d = Path(tempfile.mkdtemp(dir=work))
        conf = make_conf(d, port)
        rc, out, _ = launch(binary, d, conf, umask=0o027)
        pids = daemons(binary)
        check(rc == 0 and "running in the background (pid" in out and "HTTP server listening on 127.0.0.1" in out,
              f"good start: exit 0 and the startup log is shown (rc={rc})")
        check(len(pids) == 1 and wait_health(port, 5), "the daemon serves /health")
        if pids:
            pid = pids[0]
            check(os.readlink(f"/proc/{pid}/cwd") == str(d), "working directory kept (not /)")
            umask = [l for l in Path(f"/proc/{pid}/status").read_text().splitlines() if l.startswith("Umask")][0].split()[1]
            check(umask == "0027", f"umask kept ({umask})")
            os.kill(pid, signal.SIGTERM)
            end = time.time() + 15             # shutdown waits for the cache thread's 5 s sleep, as in the foreground
            while daemons(binary) and time.time() < end:
                time.sleep(0.1)
                reap()
            cache = d / "rel_cache.jsonl"
            check(not daemons(binary), "SIGTERM stops it")
            check(cache.exists() and stat.S_IMODE(cache.stat().st_mode) == 0o640,
                  f"relative cache file in the launch directory, mode per umask "
                  f"({oct(stat.S_IMODE(cache.stat().st_mode)) if cache.exists() else 'missing'})")

        # -l: startup lines and later lines both in the file
        d = Path(tempfile.mkdtemp(dir=work))
        conf = make_conf(d, port)
        rc, out, _ = launch(binary, d, conf, extra=("-l", "daemon.log"))
        ok = rc == 0 and wait_health(port, 5)
        time.sleep(0.3)
        text = (d / "daemon.log").read_text() if (d / "daemon.log").exists() else ""
        check(ok and "HTTP server listening on 127.0.0.1" in text and "Loading configuration" in text,
              "-l: the startup log is in the file")
        kill_all(binary)
        text = (d / "daemon.log").read_text()
        check(text.count("HTTP server listening on") == 1, "-l: each startup line once")

        rc, out, _ = launch(binary, d, conf, extra=("-l", "/nonexistent/dir/x.log"))
        reap()
        check(rc == 1 and "cannot open log file" in out, "-l with an unwritable path: exit 1 with the reason")
        p = subprocess.run([binary, "-l", "x.log", "-c", str(conf)], cwd=d, capture_output=True, text=True, timeout=20)
        check(p.returncode == 1 and "only used with -d" in p.stdout + p.stderr, "-l without -d is refused")

        # the launcher killed early: the daemon still comes up
        d = Path(tempfile.mkdtemp(dir=work))
        bindir = d / "bin"
        bindir.mkdir()
        (bindir / "codex").write_text(FAKE_CODEX)
        (bindir / "codex").chmod(0o755)
        conf = make_conf(d, port, MODEL_TYPE="codex", CODEX_BIN=str(bindir / "codex"), CODEX_RUN_DIR=str(d))
        lp = subprocess.Popen([binary, "-d", "-c", str(conf), "-r", "role.txt"], cwd=d, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, env={**os.environ, "FAKE_SLEEP": "1.5"})
        time.sleep(0.5)
        lp.kill()
        lp.wait()
        check(wait_health(port, 10), "launcher SIGKILLed during startup: the daemon still starts")
        kill_all(binary)

        # SIGTERM to the daemon during startup: no success report
        lp = subprocess.Popen([binary, "-d", "-c", str(conf), "-r", "role.txt"], cwd=d, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True, env={**os.environ, "FAKE_SLEEP": "1.5"})
        time.sleep(0.6)
        for pid in daemons(binary):
            if pid != lp.pid:
                os.kill(pid, signal.SIGTERM)
        out, _ = lp.communicate(timeout=30)
        reap()
        check(lp.returncode == 1 and "running in the background" not in out, f"SIGTERM during startup: launcher exit 1 (rc={lp.returncode})")
        kill_all(binary)

        # the deadline (test build: 3 s) - slow start, and a launcher whose stderr is never read
        rc, out, took = launch(short, d, conf, env={"FAKE_SLEEP": "4"})
        check(rc == 1 and "did not report within 3 s" in out and took < 8, f"slow start: exit 1 after the deadline ({took:.1f} s)")
        check(wait_health(port, 15), "...and the daemon still finishes starting")
        kill_all(short)
        r, w = os.pipe()
        os.set_blocking(w, False)
        try:
            while True:
                os.write(w, b"x" * 65536)
        except BlockingIOError:
            pass
        os.set_blocking(w, True)
        t0 = time.time()
        # stdout is the launcher's own first line (any program blocks there); the forwarded log goes to stderr
        p = subprocess.Popen([short, "-d", "-c", str(conf), "-r", "role.txt"], cwd=d, stdout=subprocess.DEVNULL, stderr=w,
                             env={**os.environ, "FAKE_SLEEP": "0"})
        try:
            p.wait(timeout=20)
            took = time.time() - t0
            check(took < 12 and p.returncode == 0, f"launcher with a full, unread stderr still exits and reports the start ({took:.1f} s, rc={p.returncode})")
        except subprocess.TimeoutExpired:
            p.kill()
            check(False, "launcher with a full, unread stderr hung")
        os.close(r)
        os.close(w)
        kill_all(binary)
        kill_all(short)
    finally:
        kill_all(binary)
        kill_all(short)
        shutil.rmtree(work, ignore_errors=True)


def main():
    args = sys.argv[1:]
    if args and args[0] == INNER:
        rows(str(Path(args[1]).resolve()), str(Path(args[2]).resolve()))
        print("OK" if not FAILURES else f"FAILED: {len(FAILURES)}")
        return 0 if not FAILURES else 1
    cmd = ["unshare", "-rn", "--pid", "--fork", "--kill-child=KILL", "--mount-proc"]
    if not shutil.which("unshare") or subprocess.run(cmd + ["true"], capture_output=True).returncode != 0:
        print("unshare not usable here; daemon rows skipped")
        return 0
    return subprocess.run(cmd + [sys.executable, "-I", __file__, INNER] + [str(Path(a).resolve()) for a in args[:2]],
                          timeout=600).returncode


if __name__ == "__main__":
    sys.exit(main())
