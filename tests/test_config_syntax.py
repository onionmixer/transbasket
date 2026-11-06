"""PLAN_OPEN_ISSUES.md H: configuration line syntax and the tracked templates.

    python3 -I tests/test_config_syntax.py <transbasket> <harness built with -DWITH_CONFIG_LOADER>

1. parse_config_line() (through the harness) against tests/config_line_ref.py:
   fixed cases plus generated lines; return code, key and value must agree.
2. Through the real server: Q8 lines stop startup with the error, the lines Q8 keeps
   start as before, and every tracked template starts unmodified.
Server rows run inside an unprivileged network + PID namespace (the templates bind
0.0.0.0), with a fake codex first on PATH for the codex template.
"""
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import config_line_ref as ref  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
INNER = "--inner"
TEMPLATES = ["transbasket.conf", "transbasket_codex.conf", "transbasket_example_cache_file.conf",
             "transbasket_example_cache_sqlite.conf"]
FAILURES = []

FAKE_CODEX = """#!/usr/bin/env python3
import sys
a = sys.argv[1:]
if a == ["--version"]:
    print("codex-cli 0.166.0")
elif a[:2] == ["exec", "--help"]:
    print("--skip-git-repo-check --color --ephemeral --ignore-user-config -o --output-last-message --output-schema -s -c -m -C")
elif a[:2] == ["mcp", "list"]:
    print("[]")
else:
    sys.exit(3)
"""


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        FAILURES.append(what)


def differential(harness):
    r = random.Random(20261106)
    toks = [b"KEY", b"SEED", b"A_B", b"key", b"=", b"==", b'"', b"'", b" ", b"\t", b"#", b"x", b"1", b"\r",
            b"\xea\xb0\x80", b"x" * 300, b'""', b"''"]
    lines = [c for c, _ in ref.CASES]
    lines += [b"".join(r.choice(toks) for _ in range(r.randrange(0, 8))) for _ in range(30000)]
    lines += [b"KEY=" + b"".join(r.choice([b'"', b"'", b"a", b" ", b"#"]) for _ in range(r.randrange(0, 6)))
              for _ in range(20000)]
    lines = [ln for ln in lines if b"\x00" not in ln and b"\n" not in ln and len(ln) < 1023]
    out = subprocess.run([harness], input="".join(f"P {ln.hex()}\n" for ln in lines),
                         capture_output=True, text=True, check=True).stdout.splitlines()
    assert len(out) == len(lines)
    bad, mix = [], {}
    for ln, o in zip(lines, out):
        rc, key, _scan, val = o.split()
        rc = int(rc)
        want = ref.parse(ln)
        got = (rc, None if key == "-" else key.encode(), bytes.fromhex(val[1:]) if val != "-" else None)
        exp = (want[0], want[1] if want[0] in (1, -3) else None, want[2] if want[0] == 1 else None)
        if got != exp:
            bad.append((ln[:60], got, exp))
        mix[rc] = mix.get(rc, 0) + 1
    hand = [(c, e) for c, e in ref.CASES if ref.parse(c)[0] != e]
    check(not hand, f"hand-written verdicts agree with the specification ({len(ref.CASES)} cases)")
    check(not bad, f"C parse_config_line() = specification on {len(lines)} lines, rc mix {dict(sorted(mix.items()))}"
          + (f" first {bad[:3]}" if bad else ""))


def reexec_in_namespace(argv):
    if not shutil.which("unshare"):
        return None
    cmd = ["unshare", "-rn", "--pid", "--fork", "--kill-child=KILL", "--mount-proc"]
    if subprocess.run(cmd + ["true"], capture_output=True).returncode != 0:
        print("unshare not usable here; server rows skipped")
        return None
    return subprocess.run(cmd + [sys.executable, "-I", __file__, INNER] + argv, timeout=900).returncode


def health(port):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as s:
            s.sendall(b"GET /health HTTP/1.0\r\n\r\n")
            return b" 200 " in s.recv(200).split(b"\r\n", 1)[0]
    except OSError:
        return False


def run_server(binary, conf, cwd, env, port, wait_ok=True):
    log = open(cwd / "log", "wb")
    p = subprocess.Popen([binary, "-c", str(conf), "-r", str(ROOT / "ROLS.txt")] if (ROOT / "ROLS.txt").exists()
                         else [binary, "-c", str(conf)], cwd=cwd, stdout=log, stderr=subprocess.STDOUT, env=env)
    deadline = time.time() + 20
    while time.time() < deadline:
        text = (cwd / "log").read_text(errors="replace")
        if p.poll() is not None or "listening on" in text:
            break
        time.sleep(0.05)
    ok = p.poll() is None and health(port)
    if p.poll() is None:
        p.terminate()
        try:
            p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
    log.close()
    return ok, p.returncode, (cwd / "log").read_text(errors="replace")


def server_rows(binary):
    subprocess.run(["ip", "link", "set", "lo", "up"], check=True)
    work = Path(tempfile.mkdtemp(prefix="cfgsyntax-"))
    bindir = work / "bin"
    bindir.mkdir()
    (bindir / "codex").write_text(FAKE_CODEX)
    (bindir / "codex").chmod(0o755)
    env = {"PATH": f"{bindir}:/usr/bin:/bin", "HOME": str(work)}
    try:
        for name in TEMPLATES:
            d = work / name.replace(".", "_")
            d.mkdir()
            (d / "codex_run").mkdir(mode=0o700)
            ok, rc, log = run_server(binary, ROOT / name, d, env, 8889)
            check(ok and "HTTP server listening on 0.0.0.0:8889 (IPv4)" in log,
                  f"template {name} starts unmodified" + ("" if ok else f" (rc={rc}: {[l for l in log.splitlines() if 'Error' in l][:2]})"))
        port = 18892
        head = (f'OPENAI_BASE_URL="http://127.0.0.1:9/v1"\nOPENAI_MODEL="m"\nOPENAI_API_KEY="k"\nLISTEN="127.0.0.1"\n'
                f'PORT="{port}"\nPROMPT_PREFIX_FILE="{ROOT}/PROMPT_PREFIX.txt"\nLANGUAGE_TABLE_FILE="{ROOT}/language_tables.conf"\n'
                f'TRANS_CACHE_TYPE="text"\n').encode()
        rows = [(ln, rc) for ln, rc in ref.CASES if rc == -3 and ln.startswith((b"TRANS_CACHE_FILE=", b"SEED=", b"INFLIGHT_MAX_WAITERS="))]
        rows += [(b'TRANS_CACHE_FILE="', 1), (b'TRANS_CACHE_FILE="a"tail"', 1), (b"TRANS_CACHE_FILE=\"'a\"", 1),
                 (b'SEED="7"', 1), (b"SEED=7", 1), (b'INFLIGHT_MAX_WAITERS="256"', 1)]
        for line, want in rows:
            assert ref.parse(line)[0] == want, line
            d = Path(tempfile.mkdtemp(prefix="row-", dir=work))
            (d / "t.conf").write_bytes(head + line + b"\n")
            ok, rc, log = run_server(binary, d / "t.conf", d, env, port)
            if want == -3:
                key = line.split(b"=", 1)[0].decode()
                check(not ok and rc == 1 and f"the value of {key} starts with a quote but does not end with the same quote" in log,
                      f"{line[:40]!r}: startup error (rc={rc})")
            else:
                check(ok, f"{line[:40]!r}: starts as before")
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    args = sys.argv[1:]
    inner = bool(args) and args[0] == INNER
    if inner:
        server_rows(str(Path(args[1]).resolve()))
        print("OK" if not FAILURES else f"FAILED: {len(FAILURES)}")
        return 0 if not FAILURES else 1
    binary, harness = (str(Path(a).resolve()) for a in args[:2])
    print("parse_config_line() vs specification:")
    differential(harness)
    print("server rows (namespace):")
    rc = reexec_in_namespace([binary])
    if rc is None:
        print("  skipped")
    elif rc != 0:
        FAILURES.append("server rows")
    print("OK" if not FAILURES else f"FAILED: {len(FAILURES)}")
    return 0 if not FAILURES else 1


if __name__ == "__main__":
    sys.exit(main())
