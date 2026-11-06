"""PLAN_OPEN_ISSUES.md B: no forged log lines, no source text in the log, safe trace files.

    python3 -I tests/test_log_safety.py <transbasket> <log_escape harness>

Every server binds 127.0.0.1; upstream and codex are offline fakes.
"""
import json
import os
import random
import re
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
if __name__ == "__main__":
    BINARY, ESCAPE = (str(Path(a).resolve()) for a in sys.argv[1:3])
    del sys.argv[1:3]
FORGED = "[2026-01-01 00:00:00][INFO] FORGED"
SECRET = "sk-TEST-SECRET-not-a-real-key-7f3a"
LOG_LINE = re.compile(r"^\[\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\]\[(INFO|DEBUG)\] ")

FAKE_EXEC_FAIL = """#!/usr/bin/env python3
import sys
if "--version" in sys.argv: print("codex-cli 0.166.0")
elif "--help" in sys.argv: print("exec --ephemeral --ignore-user-config -o")
elif sys.argv[1:3] == ["mcp", "list"]: print("[]")
elif sys.argv[1] == "exec":
    sys.stdin.read()
    sys.stderr.write("ERROR: boom\\x1b[31m red \\x07 bell\\n")
    sys.exit(1)
"""


def log_escape_spec(b, max_in, out_size):
    out, cut = b"", False
    for i, c in enumerate(b):
        if i >= max_in:
            cut = True
            break
        t = {10: b"\\n", 13: b"\\r", 9: b"\\t", 92: b"\\\\"}.get(c)
        if t is None:
            t = b"\\x%02x" % c if c < 0x20 or c == 0x7F else bytes([c])
        if len(out) + len(t) + 4 > out_size:
            cut = True
            break
        out += t
    if cut and len(out) + 4 <= out_size:
        out += b"..."
    return out


class Upstream:
    def __init__(self):
        self.reply = b'{"choices":[{"message":{"role":"assistant","content":"ok"}}]}'
        outer = self

        class H(BaseHTTPRequestHandler):
            def do_POST(self):
                self.rfile.read(int(self.headers["Content-Length"]))
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(outer.reply)))
                self.end_headers()
                self.wfile.write(outer.reply)

            def log_message(self, *a):
                pass
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()


UP = Upstream()


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server:
    def __init__(self, workdir, extra=None, debug=False):
        self.d = Path(workdir)
        self.port = free_port()
        (self.d / "p.txt").write_text("Translate to [TARGET LANGUAGE]", encoding="utf-8")
        (self.d / "r.txt").write_text("role", encoding="utf-8")
        v = {"OPENAI_BASE_URL": f"http://127.0.0.1:{UP.server.server_port}/v1", "OPENAI_MODEL": "m",
             "OPENAI_API_KEY": SECRET, "LISTEN": "127.0.0.1", "PORT": str(self.port), "PROMPT_PREFIX_FILE": "p.txt",
             "LANGUAGE_TABLE_FILE": str(ROOT / "language_tables.conf"), "TRANS_CACHE_TYPE": "text",
             "TRANS_CACHE_FILE": str(self.d / "c.jsonl"), "TRANS_CACHE_THRESHOLD": "1", "DEBUG": "yes" if debug else "no",
             "TRANSLATION_TOTAL_TIMEOUT": "20"}
        v.update(extra or {})
        (self.d / "t.conf").write_text("".join(f'{k}="{x}"\n' for k, x in v.items()))
        self.logf = open(self.d / "log", "wb")
        self.p = subprocess.Popen([BINARY, "-c", str(self.d / "t.conf"), "-r", "r.txt"], cwd=self.d,
                                  stdout=self.logf, stderr=subprocess.STDOUT)
        for _ in range(300):
            try:
                urlopen(f"http://127.0.0.1:{self.port}/health", timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.02)

    def post(self, body):
        req = Request(f"http://127.0.0.1:{self.port}/translate", data=body, headers={"Content-Type": "application/json"})
        try:
            with urlopen(req, timeout=60) as r:
                return r.status
        except HTTPError as e:
            return e.code

    def stop(self):
        self.p.terminate()
        self.p.wait(timeout=15)
        self.logf.close()
        return (self.d / "log").read_text(errors="replace")


def request(**over):
    r = {"timestamp": "2026-11-06T01:02:03.456Z", "uuid": str(uuid.uuid4()), "from": "kor", "to": "eng", "text": "x"}
    r.update(over)
    return json.dumps(r).encode()


class LogSafety(unittest.TestCase):
    def test_log_escape_function(self):
        r = random.Random(20261106)
        cases = [(bytes(r.randrange(1, 256) for _ in range(r.randrange(0, 90))), r.choice([0, 1, 5, 64, 300]),
                  r.choice([4, 5, 8, 16, 64, 300])) for _ in range(20000)]
        cases += [(b"a\nb", 64, 64), (b"\\", 64, 64), (b"\x1b[31m", 64, 64), ("가".encode() * 30, 64, 300), (b"", 64, 4)]
        reqs = "".join(f"{m} {o} {b.hex()}\n" for b, m, o in cases)
        out = subprocess.run([ESCAPE], input=reqs, capture_output=True, text=True, check=True).stdout.splitlines()
        self.assertEqual(len(out), len(cases), "one output line per input")
        bad = [(b, m, o, g) for (b, m, o), g in zip(cases, out) if bytes.fromhex(g[1:]) != log_escape_spec(b, m, o)]
        self.assertEqual(bad[:3], [])
        lines = sum(1 for (b, m, o), g in zip(cases, out) if b"\n" in bytes.fromhex(g[1:]) or b"\r" in bytes.fromhex(g[1:]))
        self.assertEqual(lines, 0)
        print(f"  log_escape: {len(cases)} inputs = specification, none contains CR/LF")

    def test_no_forged_lines_and_no_text(self):
        with tempfile.TemporaryDirectory(prefix="logsafe-") as d:
            fake = Path(d) / "codex"
            fake.write_bytes((ROOT / "tests/fake_codex_nul.py").read_bytes())
            fake.chmod(0o700)
            srv = Server(d)
            marker = "UNIQUE-SOURCE-MARKER-" + uuid.uuid4().hex
            statuses = [
                srv.post(request(text=marker + "\n" + FORGED + " via text")),
                srv.post(request(uuid="x\n" + FORGED + " via uuid")),
                srv.post(request(timestamp="x\r\n" + FORGED + " via timestamp")),
                srv.post(request(**{"from": "x\n" + FORGED + " via from"})),
                srv.post(request(to="x\n" + FORGED + " via to")),
                srv.post(b'{"timestamp":"2026", BROKEN\n' + FORGED.encode() + b" via parse error tail"),
            ]
            log = srv.stop()
        self.assertEqual(statuses, [200, 422, 422, 422, 422, 422])
        forged = [l for l in log.splitlines() if l.startswith(FORGED)]
        self.assertEqual(forged, [], "a forged line made it into the log")
        self.assertNotIn(marker, log, "source text is in the log")
        self.assertIn(f"text_bytes={len((marker + chr(10) + FORGED + ' via text').encode())}", log)
        self.assertIn("Invalid UUID format: x\\n[2026-01-01", log)
        self.assertIn("Failed to parse JSON at byte", log)
        print("  6 forging attempts (text, uuid, timestamp, from, to, JSON tail): no forged line, no source text")

    def test_codex_errors_escaped(self):
        for iface in ("exec", "mcp"):
            with tempfile.TemporaryDirectory(prefix="logsafe-codex-") as d:
                fake = Path(d) / "codex"
                fake.write_text(FAKE_EXEC_FAIL if iface == "exec" else (ROOT / "tests/fake_codex_nul.py").read_text())
                fake.chmod(0o700)
                env_hex = ('{"jsonrpc":"2.0","id":@ID@,"error":{"code":-1,"message":"boom\\n' + FORGED + ' via mcp"}}').encode().hex()
                os.environ["TB_NUL_MCP_HEX"] = env_hex
                srv = Server(d, {"MODEL_TYPE": "codex", "CODEX_BIN": str(fake), "CODEX_INTERFACE": iface,
                                 "CODEX_RUN_DIR": d, "CODEX_TIMEOUT": "10"})
                status = srv.post(request(text="hello"))
                log = srv.stop()
            self.assertNotEqual(status, 200)
            self.assertEqual([l for l in log.splitlines() if l.startswith(FORGED)], [], iface)
            if iface == "exec":
                self.assertIn("\\x1b[31m red \\x07 bell", log)
            else:
                self.assertIn("boom\\n[2026-01-01", log)
        print("  codex exec stderr and MCP error text: control bytes escaped, no forged line")

    def test_trace_files(self):
        with tempfile.TemporaryDirectory(prefix="logsafe-trace-") as d:
            d = Path(d)
            srv = Server(d, debug=True)
            u = str(uuid.uuid4())
            self.assertEqual(srv.post(request(uuid=u, text="first")), 200)
            self.assertEqual(srv.post(request(uuid=u, text="second")), 200)    # same uuid + timestamp
            trace = d / "trace"
            name = "2026-11-06T01-02-03.456Z_" + u
            (trace / (name + "-2.txt")).symlink_to(d / "victim")             # must not be followed
            self.assertEqual(srv.post(request(uuid=u, text="third")), 200)
            log = srv.stop()
            files = sorted(p.name for p in trace.iterdir())
            self.assertEqual(files, [name + "-1.txt", name + "-2.txt", name + "-3.txt", name + ".txt"], files)
            self.assertFalse((d / "victim").exists(), "the symlink was followed")
            self.assertEqual(stat.S_IMODE(trace.stat().st_mode), 0o700)
            for f in (name + ".txt", name + "-1.txt", name + "-3.txt"):
                p = trace / f
                self.assertEqual(stat.S_IMODE(p.stat().st_mode), 0o600, f)
                text = p.read_text()
                self.assertNotIn(SECRET, text)
                self.assertIn("Bearer <redacted>", text)
            self.assertNotIn(SECRET, log)
        with tempfile.TemporaryDirectory(prefix="logsafe-trace2-") as d:
            d = Path(d)
            (d / "trace").mkdir(mode=0o755)
            os.chmod(d / "trace", 0o755)
            srv = Server(d, debug=True)
            srv.post(request(text="a"))
            srv.post(request(text="b"))
            log = srv.stop()
            self.assertEqual(log.count("is accessible to group/others (mode 755)"), 1)
            self.assertEqual(stat.S_IMODE((d / "trace").stat().st_mode), 0o755, "operator's mode untouched")
        print("  trace: 0700 dir, 0600 files, key redacted, clash -> suffix, symlink not followed, loose dir warned once")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], "-v"])
