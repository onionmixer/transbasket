"""V3 (server part) of PLAN_OUT_OF_SCOPE.md: codex slots are first come, first served.

    python3 -I tests/test_codex_fifo.py <transbasket> [<pre-fix transbasket>]

One slot (CODEX_MAX_CONCURRENCY=1), CODEX_TIMEOUT=2, four slow requests (the fake
codex sleeps 4 s) sent 0.3 s apart. In arrival order the answers must be
504 (holds the slot, times out), 504 (gets the slot next, times out), 503, 503 (their
slot wait runs out first) - three rounds. The log's "Starting codex translation"
order confirms the arrival order at the backend; that line comes just before the
queue is joined, and with 0.3 s between requests the queue order is the same (the
exact join order is proven by tests/test_slot_gate.c, which observes joins). A
pre-fix binary is only reported: its waiters poll, so which one gets the freed
slot varies - it may also give the FIFO answers by chance, so this test alone
does not prove FIFO.
"""
import json
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FAILURES = []
FAKE = """#!/usr/bin/env python3
import sys, time
from pathlib import Path
a = sys.argv[1:]
if a == ["--version"]: print("codex-cli 0.167.0"); sys.exit(0)
if a[:2] == ["exec", "--help"]: print("--skip-git-repo-check --color --ephemeral --ignore-user-config -o"); sys.exit(0)
if a[:3] == ["mcp", "list", "--json"]: print("[]"); sys.exit(0)
if a and a[0] == "exec":
    p = sys.stdin.read()
    if "SLOW" in p: time.sleep(4)
    Path(a[a.index("-o") + 1]).write_text("ok")
    sys.exit(0)
sys.exit(3)
"""


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        FAILURES.append(what)


def uid(n):
    return f"550e8400-e29b-41d4-a716-4466554{n:05d}"


def round_trip(binary, d, r):
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    (d / "codex").write_text(FAKE)
    (d / "codex").chmod(0o700)
    (d / "r.txt").write_text("role")
    (d / "run").mkdir(exist_ok=True)
    (d / "t.conf").write_text(
        f'MODEL_TYPE="codex"\nCODEX_BIN="{d}/codex"\nCODEX_RUN_DIR="{d}/run"\nCODEX_TIMEOUT="2"\nCODEX_MAX_CONCURRENCY="1"\n'
        f'TRANSLATION_TOTAL_TIMEOUT="20"\nLISTEN="127.0.0.1"\nPORT="{port}"\nPROMPT_PREFIX_FILE="{ROOT}/PROMPT_PREFIX.txt"\n'
        f'LANGUAGE_TABLE_FILE="{ROOT}/language_tables.conf"\nTRANS_CACHE_TYPE="text"\nTRANS_CACHE_FILE="{d}/c{r}.jsonl"\n')
    log = open(d / f"log{r}", "wb")
    p = subprocess.Popen([binary, "-c", str(d / "t.conf"), "-r", "r.txt"], cwd=d, stdout=log, stderr=subprocess.STDOUT)
    for _ in range(200):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.02)
    status = {}

    def post(i):
        body = json.dumps({"timestamp": "2026-11-07T01:02:03Z", "uuid": uid(i), "from": "kor", "to": "eng",
                           "text": f"SLOW 요청 {r}-{i}"}).encode()
        with socket.create_connection(("127.0.0.1", port), timeout=30) as c:
            c.sendall(b"POST /translate HTTP/1.0\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
            data = c.recv(200)
        status[i] = int(data.split(b" ")[1])
    th = []
    for i in range(4):
        t = threading.Thread(target=post, args=(i,))
        t.start()
        th.append(t)
        time.sleep(0.3)
    for t in th:
        t.join(30)
    p.terminate()
    p.wait(timeout=20)
    log.close()
    text = (d / f"log{r}").read_text(errors="replace")
    started = [int(m.group(1)[-5:]) for m in re.finditer(r"\[(550e8400-e29b-41d4-a716-4466554\d{5})\] Starting codex translation", text)]
    return [status.get(i) for i in range(4)], started


def main():
    binary = str(Path(sys.argv[1]).resolve())
    old = str(Path(sys.argv[2]).resolve()) if len(sys.argv) > 2 else None
    with tempfile.TemporaryDirectory(prefix="fifo-") as w:
        w = Path(w)
        rounds = [round_trip(binary, w, r) for r in range(3)]
        for r, (st, started) in enumerate(rounds):
            check(started == [0, 1, 2, 3] and st == [504, 504, 503, 503],
                  f"round {r + 1}: arrival order {started}, answers {st} (want [504, 504, 503, 503])")
        if old:
            seen = [round_trip(old, w, 10 + r)[0] for r in range(5)]
            print(f"  info  pre-fix binary, 5 rounds: {seen}")
    print("OK" if not FAILURES else f"FAILED: {len(FAILURES)}")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
