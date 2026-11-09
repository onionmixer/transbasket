"""PLAN_FAULT_INJECTION.md F1 and F3, through the real server (offline fake codex).

    python3 -I tests/test_fault_server.py <transbasket> <TRANSBASKET_TEST_HOOKS build>

F1 - the per-request temp cwd cannot be created (CODEX_RUN_DIR made 0500 after
startup; run as an ordinary user): codex runs without -C, -o and --output-schema, in
the server's cwd, gets the plain prompt on stdin and answers on stdout; the reply is
200, SOURCE_HINT_MODE=model falls back to the script hint, nothing is left in
CODEX_RUN_DIR. Explicit-source requests too. With the permission back, the next
request uses -C/-o/--output-schema again. Every failed attempt logs one INFO line
"cannot create a temp directory under <run dir> (Permission denied); ..." - exactly
one per failed attempt, none for the others (PLAN_CONNECTION_LIMIT.md 2.1; until
2026-11-09 it was LOG_DEBUG and a release build logged nothing).

F1b - the temp cwd cannot be removed (the fake codex leaves a file in its -C
directory): the reply is still 200, one INFO line "cannot remove temp directory
<that directory> (Directory not empty)", and the directory stays.

F3 - the exec driver's state cannot be allocated (TB_TEST_FAIL_DRIVER_INIT in the
hooks build): exit code 1, "Error: Memory allocation failed" then "Error: Failed to
initialize server", and the server never got as far as listening (no "HTTP server
listening" line; startup stops before translation_server_start()); with -d the
launcher prints the same lines and exits 1.
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FAILURES = []
FAKE = r"""#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
a = sys.argv[1:]
if a == ["--version"]: print("codex-cli 0.167.0"); sys.exit(0)
if a[:2] == ["exec", "--help"]:
    print("--skip-git-repo-check --color --ephemeral --ignore-user-config -o --output-schema"); sys.exit(0)
if a[:3] == ["mcp", "list", "--json"]: print("[]"); sys.exit(0)
if a and a[0] == "exec":
    prompt = sys.stdin.read()
    log = Path(os.environ["FAKE_LOG"])
    with log.open("a") as f:
        f.write(json.dumps({"argv": a, "cwd": os.getcwd(), "prompt": prompt}) + "\n")
    if "--output-schema" in a:
        out = json.dumps({"source_language": "jpn", "mixed": False, "translation": "structured answer"})
    else:
        out = "plain answer"
    if "-o" in a:
        Path(a[a.index("-o") + 1]).write_text(out)
    if os.environ.get("FAKE_LEAVE") and "-C" in a:
        Path(a[a.index("-C") + 1], "left-behind.txt").write_text("x")
    print(out)
    sys.exit(0)
sys.exit(3)
"""


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        FAILURES.append(what)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def setup(d, port):
    (d / "codex").write_text(FAKE)
    (d / "codex").chmod(0o700)
    (d / "r.txt").write_text("role")
    (d / "run").mkdir(mode=0o700, exist_ok=True)
    (d / "t.conf").write_text(
        f'MODEL_TYPE="codex"\nCODEX_BIN="{d}/codex"\nCODEX_RUN_DIR="{d}/run"\nCODEX_TIMEOUT="10"\n'
        f'SOURCE_HINT_MODE="model"\nLISTEN="127.0.0.1"\nPORT="{port}"\nPROMPT_PREFIX_FILE="{ROOT}/PROMPT_PREFIX.txt"\n'
        f'LANGUAGE_TABLE_FILE="{ROOT}/language_tables.conf"\nTRANS_CACHE_TYPE="text"\nTRANS_CACHE_FILE="{d}/c.jsonl"\n'
        f'TRANS_CACHE_THRESHOLD="1"\n')


def wait_port(port, proc, secs=10):
    end = time.time() + secs
    while time.time() < end:
        if proc.poll() is not None:
            return False
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return True
        except OSError:
            time.sleep(0.05)
    return False


def ask(port, text, src):
    body = json.dumps({"timestamp": "2026-11-07T01:02:03Z", "uuid": "550e8400-e29b-41d4-a716-446655440000",
                       "from": src, "to": "eng", "text": text}).encode()
    with socket.create_connection(("127.0.0.1", port), timeout=30) as c:
        c.sendall(b"POST /translate HTTP/1.0\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
        data = b""
        while True:
            x = c.recv(65536)
            if not x:
                break
            data += x
    status = int(data.split(b" ")[1])
    return status, json.loads(data.split(b"\r\n\r\n", 1)[1])


def calls(log):
    return [json.loads(l) for l in log.read_text().splitlines()] if log.exists() else []


def f1(binary):
    with tempfile.TemporaryDirectory(prefix="fault-cwd-") as w:
        d = Path(w)
        port = free_port()
        setup(d, port)
        log = d / "fake.log"
        env = dict(os.environ, FAKE_LOG=str(log))
        out = open(d / "server.log", "wb")
        p = subprocess.Popen([binary, "-c", str(d / "t.conf"), "-r", "r.txt"], cwd=d, env=env, stdout=out,
                             stderr=subprocess.STDOUT)
        try:
            check(wait_port(port, p), "F1: server up")
            st, j = ask(port, "最初の文", "auto")
            c0 = calls(log)[-1]
            check(st == 200 and j.get("translatedText") == "structured answer" and "-C" in c0["argv"] and
                  "-o" in c0["argv"] and "--output-schema" in c0["argv"],
                  f"F1 baseline: writable run dir -> -C/-o/--output-schema, structured answer ({st})")
            os.chmod(d / "run", 0o500)
            try:
                results = {}
                for name, text, src in (("auto", "二番目の文", "auto"), ("explicit", "三番目の文", "jpn")):
                    n0 = len(calls(log))
                    st, j = ask(port, text, src)
                    cl = calls(log)
                    c = cl[-1] if len(cl) > n0 else {"argv": [], "cwd": "", "prompt": ""}
                    results[name] = (st, j.get("translatedText"), c)
                for name, (st, txt, c) in results.items():
                    a = c["argv"]
                    check(st == 200 and txt == "plain answer", f"F1 {name}: 200 with the stdout answer ({st}, {txt!r})")
                    check(not ({"-C", "-o", "--output-schema"} & set(a)),
                          f"F1 {name}: no -C/-o/--output-schema (argv {[x for x in a if x.startswith('-')]})")
                    check(Path(c["cwd"]).resolve() == d.resolve(), f"F1 {name}: codex ran in the server's cwd ({c['cwd']})")
                    check("Return ONLY the translated text" in c["prompt"] and "JSON object" not in c["prompt"],
                          f"F1 {name}: the plain prompt on stdin")
                check(sorted(os.listdir(d / "run")) == [], f"F1: nothing in CODEX_RUN_DIR ({os.listdir(d / 'run')})")
            finally:
                os.chmod(d / "run", 0o700)
            st, j = ask(port, "四番目の文", "auto")
            c = calls(log)[-1]
            check(st == 200 and "-C" in c["argv"] and "--output-schema" in c["argv"] and
                  sorted(os.listdir(d / "run")) == [],
                  "F1: permission back -> -C/-o/--output-schema again, run dir empty")
        finally:
            p.terminate()
            p.wait(timeout=20)
            out.close()
        rows = [json.loads(l) for l in (d / "c.jsonl").read_text(encoding="utf-8").splitlines() if l.strip()]
        by = {r["source"]: r for r in rows}
        a = by.get("二番目の文", {})
        check(a.get("hint_origin") == "script" and by.get("最初の文", {}).get("hint_origin") == "model",
              f"F1: SOURCE_HINT_MODE=model fell back to the script hint without a cwd "
              f"({a.get('hint_origin')}, baseline {by.get('最初の文', {}).get('hint_origin')})")
        text = (d / "server.log").read_text(errors="replace")
        want = (f"[550e8400-e29b-41d4-a716-446655440000] cannot create a temp directory under {d / 'run'} "
                f"(Permission denied); running codex in the server's working directory without -C/-o/--output-schema")
        lines = [l for l in text.splitlines() if "cannot create a temp directory" in l]
        check(len(lines) == 2 and all(l.endswith(want) for l in lines),
              f"F1: exactly one INFO line per failed attempt (2), none for the baseline/recovery ({lines})")
        check(all("[INFO]" in l for l in lines), "F1: logged at INFO (a release build shows it)")
        check("cannot remove temp directory" not in text, "F1: no removal failure was logged")
        check("cannot write the output schema" not in text, "F1: no schema file was attempted without a cwd")


def f1b(binary):
    with tempfile.TemporaryDirectory(prefix="fault-rmdir-") as w:
        d = Path(w)
        port = free_port()
        setup(d, port)
        log = d / "fake.log"
        env = dict(os.environ, FAKE_LOG=str(log), FAKE_LEAVE="1")
        out = open(d / "server.log", "wb")
        p = subprocess.Popen([binary, "-c", str(d / "t.conf"), "-r", "r.txt"], cwd=d, env=env, stdout=out,
                             stderr=subprocess.STDOUT)
        try:
            check(wait_port(port, p), "F1b: server up")
            st, j = ask(port, "五番目の文", "auto")
            c = calls(log)[-1]
            cwd = c["argv"][c["argv"].index("-C") + 1] if "-C" in c["argv"] else ""
            check(st == 200 and j.get("translatedText") == "structured answer",
                  f"F1b: the reply is still 200 from the -o file ({st}, {j.get('translatedText')!r})")
        finally:
            p.terminate()
            p.wait(timeout=20)
            out.close()
        left = sorted(os.listdir(d / "run"))
        check(cwd != "" and left == [Path(cwd).name] and os.listdir(cwd) == ["left-behind.txt"],
              f"F1b: that -C directory stays, with the file in it ({left})")
        text = (d / "server.log").read_text(errors="replace")
        lines = [l for l in text.splitlines() if "cannot remove temp directory" in l]
        want = f"[550e8400-e29b-41d4-a716-446655440000] cannot remove temp directory {cwd} (Directory not empty)"
        check(len(lines) == 1 and lines[0].endswith(want) and "[INFO]" in lines[0],
              f"F1b: one INFO line naming that directory ({lines})")


def f3(hooks):
    with tempfile.TemporaryDirectory(prefix="fault-s1-") as w:
        d = Path(w)
        port = free_port()
        setup(d, port)
        env = dict(os.environ, TB_TEST_FAIL_DRIVER_INIT="1", FAKE_LOG=str(d / "fake.log"))
        r = subprocess.run([hooks, "-c", str(d / "t.conf"), "-r", "r.txt"], cwd=d, env=env, capture_output=True,
                           text=True, timeout=60)
        out = r.stdout + r.stderr
        i = out.find("Error: Memory allocation failed")
        j = out.find("Error: Failed to initialize server")
        check(r.returncode == 1 and 0 <= i < j, f"F3: exit 1, the S1 line then the init failure (rc {r.returncode})")
        check("HTTP server listening" not in out, "F3: never listening")
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
            bound = True
        except OSError:
            bound = False
        check(not bound, "F3: the port is free after the exit (the missing listening line above is the real evidence)")
        r = subprocess.run([hooks, "-d", "-c", str(d / "t.conf"), "-r", "r.txt"], cwd=d, env=env, capture_output=True,
                           text=True, timeout=90)
        out = r.stdout + r.stderr
        i = out.find("Error: Memory allocation failed")
        j = out.find("Error: Failed to initialize server")
        check(r.returncode == 1 and 0 <= i < j and "HTTP server listening" not in out,
              f"F3 -d: the launcher shows both lines in order, no listening line, exits 1 (rc {r.returncode})")


def main():
    binary, hooks = (str(Path(a).resolve()) for a in sys.argv[1:3])
    if os.geteuid() == 0:
        print("  skip  F1 needs an ordinary user (root ignores the 0500 directory)")
    else:
        f1(binary)
    f1b(binary)
    f3(hooks)
    print("OK" if not FAILURES else f"FAILED: {len(FAILURES)}")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
