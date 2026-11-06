"""Server-level NUL guard tests (PLAN_CLEANUP_DAYS_AND_NUL.md section 5 V4).
In payload notation "~" stands for a backslash. Offline: fake codex + fake upstream.

    python3 tests/test_nul_guard.py [transbasket binary]
"""
import json
import os
import socket
import sqlite3
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
BINARY = str(Path(sys.argv[1]).resolve()) if len(sys.argv) > 1 else str(ROOT / "transbasket")
BS = b"\x5c"


def bs(text):
    return text.encode("utf-8").replace(b"~", BS)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Upstream:
    def __init__(self):
        self.reply, self.calls = b"", []
        outer = self

        class H(BaseHTTPRequestHandler):
            def do_POST(self):
                outer.calls.append(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(outer.reply)

            def log_message(self, *a):
                pass
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()


UP = Upstream()


def chat(content_json_literal):
    """A non-streaming reply whose content is the given JSON string literal (bytes, quoted)."""
    return b'{"choices":[{"message":{"role":"assistant","content":' + content_json_literal + b'}}]}'


def sse(*payloads):
    return b"".join(b"data: " + p + b"\n\n" for p in payloads) + b"data: [DONE]\n\n"


def delta(lit):
    return b'{"choices":[{"delta":{"content":' + lit + b'}}]}'


class NulGuard(unittest.TestCase):
    def run_case(self, backend, body, env=None, stream=False):
        with tempfile.TemporaryDirectory(prefix="nul-guard-") as d:
            d = Path(d)
            port = free_port()
            (d / "p.txt").write_text("Translate to [TARGET LANGUAGE]", encoding="utf-8")
            (d / "r.txt").write_text("role", encoding="utf-8")
            fake = d / "codex"
            fake.write_bytes((ROOT / "tests/fake_codex_nul.py").read_bytes())
            fake.chmod(0o700)
            v = {"OPENAI_BASE_URL": f"http://127.0.0.1:{UP.server.server_port}/v1", "OPENAI_MODEL": "m", "OPENAI_API_KEY": "k",
                 "LISTEN": "127.0.0.1", "PORT": str(port), "MODEL_TYPE": "normal" if backend == "normal" else "codex",
                 "PROMPT_PREFIX_FILE": "p.txt", "LANGUAGE_TABLE_FILE": str(ROOT / "language_tables.conf"),
                 "TRANS_CACHE_TYPE": "sqlite", "TRANS_CACHE_SQLITE_PATH": str(d / "c.db"), "TRANS_CACHE_THRESHOLD": "1",
                 "TRANS_CACHE_CLEANUP_ENABLED": "false", "CODEX_BIN": str(fake),
                 "CODEX_INTERFACE": "exec", "CODEX_RUN_DIR": str(d), "CODEX_TIMEOUT": "5",
                 "TRANSLATION_TOTAL_TIMEOUT": "8", "STREAM": "true" if stream else "false"}
            (d / "t.conf").write_text("".join(f'{k}="{x}"\n' for k, x in v.items()))
            with open(d / "log", "w") as log:
                p = subprocess.Popen([BINARY, "-c", str(d / "t.conf"), "-r", "r.txt"], cwd=d, stdout=log, stderr=log,
                                     env={**os.environ, **(env or {})})
                try:
                    for _ in range(250):
                        try:
                            urlopen(f"http://127.0.0.1:{port}/health", timeout=0.2).close()
                            break
                        except OSError:
                            time.sleep(0.02)
                    UP.calls.clear()
                    req = Request(f"http://127.0.0.1:{port}/translate", data=body, headers={"Content-Type": "application/json"})
                    try:
                        with urlopen(req, timeout=20) as r:
                            status, out = r.status, json.loads(r.read())
                    except HTTPError as e:
                        status, out = e.code, json.loads(e.read())
                finally:
                    p.terminate()
                    p.wait(timeout=10)
            db = sqlite3.connect(d / "c.db")
            try:
                rows = db.execute("SELECT source_text, translated_text FROM trans_cache").fetchall()
            finally:
                db.close()
            return status, out, rows, list(UP.calls)

    @staticmethod
    def body(text_lit=b'"x"', uuid_lit=None, extra=b""):
        u = uuid_lit or ('"%s"' % uuid.uuid4()).encode()
        return (b'{"timestamp":"2026-11-06T01:02:03.456Z","uuid":' + u + b',"from":"kor","to":"eng","text":' + text_lit + b"}" + extra)

    # ---- requests: 422, upstream never called, nothing cached ----
    def test_request_rejected(self):
        UP.reply = chat(b'"ok"')
        cases = {
            "text \\u0000": self.body(bs('"원문~u0000숨김"')),
            "text \\u00ZZ": self.body(bs('"원문~u00ZZ숨김"')),
            "uuid suffix": self.body(uuid_lit=('"%s' % uuid.uuid4()).encode() + bs('~u0000junk"')),
            "key text\\u0000x": bs('{"timestamp":"2026-11-06T01:02:03.456Z","uuid":"%s","from":"kor","to":"eng","text~u0000x":"a"}' % uuid.uuid4()),
            "from kor\\u0000": bs('{"timestamp":"2026-11-06T01:02:03.456Z","uuid":"%s","from":"kor~u0000xx","to":"eng","text":"a"}' % uuid.uuid4()),
        }
        for name, body in cases.items():
            status, out, rows, calls = self.run_case("normal", body)
            self.assertEqual((status, rows, len(calls)), (422, [], 0), name)
            self.assertEqual(out.get("errorCode"), "VALIDATION_ERROR", name)

    def test_request_controls(self):
        UP.reply = chat(b'"ok"')
        controls = [  # (text literal, text the upstream must receive)
            (bs('"a~~u0000b"'), "a" + chr(92) + "u0000b"),
            (bs('"caf~u00e9"'), "caf" + chr(0xE9)),
            (bs('"x~uD83D~uDE00y"'), "x" + chr(0x1F600) + "y"),
            (b'"plain"', "plain"),
        ]
        for lit, sent in controls:
            status, out, rows, calls = self.run_case("normal", self.body(lit))
            self.assertEqual(status, 200, lit)
            self.assertEqual(len(calls), 1, lit)
            user = [m["content"] for m in calls[0]["messages"] if m["role"] == "user"][-1]
            self.assertIn(sent, user, lit)
        # bytes after the root value are ignored by cJSON, before and after the change
        status, _, rows, calls = self.run_case("normal", self.body(extra=bs(" ~u0000")))
        self.assertEqual((status, len(calls)), (200, 1))

    # ---- exec plain ----
    def test_exec_plain(self):
        status, out, rows, _ = self.run_case("exec", self.body(), {"TB_NUL_EXEC_HEX": "앞\x00뒤".encode().hex()})
        self.assertEqual((status, rows), (502, []))
        self.assertIn("NUL", json.dumps(out))
        status, out, rows, _ = self.run_case("exec", self.body(), {"TB_NUL_EXEC_HEX": "앞뒤".encode().hex()})
        self.assertEqual((status, out.get("translatedText"), len(rows)), (200, "앞뒤", 1))

    # ---- normal backend ----
    def test_normal_nonstream(self):
        for lit in (bs('"앞~u0000뒤"'), bs('"앞~u00ZZ뒤"')):
            UP.reply = chat(lit)
            status, out, rows, calls = self.run_case("normal", self.body())
            self.assertEqual((status, rows, len(calls)), (502, [], 1), lit)   # not retried
        for reply, want in [(chat(bs('"lit~~u0000"')), "lit" + chr(92) + "u0000"),
                            (chat(b'"OK"') + bs(" ~u0000"), "OK")]:
            UP.reply = reply
            status, out, rows, calls = self.run_case("normal", self.body())
            self.assertEqual((status, out.get("translatedText"), len(rows)), (200, want, 1), reply)

    def test_normal_stream(self):
        UP.reply = sse(delta(b'"A"'), delta(bs('"~u0000"')), delta(b'"B"'))
        status, _, rows, calls = self.run_case("normal", self.body(), stream=True)
        self.assertEqual((status, rows, len(calls)), (502, [], 1), "good -> NUL -> good")
        # PLAN_OPEN_ISSUES.md D/E: a malformed event used to be skipped (200 "AB", cached with a hole);
        # it now fails the whole reply like the NUL case.
        UP.reply = sse(delta(b'"A"'), b'{"choices":[{"delta":{"content":"broken"', delta(b'"B"'))
        status, out, rows, calls = self.run_case("normal", self.body(), stream=True)
        self.assertEqual((status, rows, len(calls)), (502, [], 1), "good -> malformed -> good")
        UP.reply = b": diagnostic " + bs("~u0000") + b"\n\n" + sse(delta(b'"OK"'))
        status, out, rows, _ = self.run_case("normal", self.body(), stream=True)
        self.assertEqual((status, out.get("translatedText")), (200, "OK"), "SSE comment")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], "-v"])
