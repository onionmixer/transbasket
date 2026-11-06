"""PLAN_OPEN_ISSUES.md C, D, E: the normal backend's reply handling through the real server.

    python3 -I tests/test_normal_output.py <transbasket>

A fake OpenAI-compatible upstream on 127.0.0.1 returns prepared bodies; the expected
translation is computed with tests/strip_ref.py (cJSON decoding, then unescape, then
strip), never taken from the C side.
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

sys.path.insert(0, str(Path(__file__).resolve().parent))
import strip_ref as ref  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
BINARY = str(Path(sys.argv.pop(1)).resolve()) if __name__ == "__main__" else None


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
                self.send_header("Content-Length", str(len(outer.reply)))
                self.end_headers()
                try:
                    self.wfile.write(outer.reply)
                except (BrokenPipeError, ConnectionResetError):
                    pass   # the client stopped reading (size limit)

            def log_message(self, *a):
                pass
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()


UP = Upstream()


def chat_raw(content_literal):
    return b'{"choices":[{"message":{"role":"assistant","content":' + content_literal + b"}}]}"


def chat(text):
    return chat_raw(json.dumps(text, ensure_ascii=False).encode())


class Server:
    def __init__(self, stream=False):
        self.dir = tempfile.TemporaryDirectory(prefix="normal-out-")
        d = Path(self.dir.name)
        self.port = free_port()
        (d / "p.txt").write_text("Translate to [TARGET LANGUAGE]", encoding="utf-8")
        (d / "r.txt").write_text("role", encoding="utf-8")
        v = {"OPENAI_BASE_URL": f"http://127.0.0.1:{UP.server.server_port}/v1", "OPENAI_MODEL": "m", "OPENAI_API_KEY": "k",
             "LISTEN": "127.0.0.1", "PORT": str(self.port), "MODEL_TYPE": "normal", "PROMPT_PREFIX_FILE": "p.txt",
             "LANGUAGE_TABLE_FILE": str(ROOT / "language_tables.conf"), "TRANS_CACHE_TYPE": "sqlite",
             "TRANS_CACHE_SQLITE_PATH": str(d / "c.db"), "TRANS_CACHE_THRESHOLD": "1", "TRANS_CACHE_CLEANUP_ENABLED": "false",
             "TRANSLATION_TOTAL_TIMEOUT": "20", "STREAM": "true" if stream else "false"}
        (d / "t.conf").write_text("".join(f'{k}="{x}"\n' for k, x in v.items()))
        self.log = open(d / "log", "w")
        self.p = subprocess.Popen([BINARY, "-c", str(d / "t.conf"), "-r", "r.txt"], cwd=d, stdout=self.log,
                                  stderr=subprocess.STDOUT)
        for _ in range(300):
            try:
                urlopen(f"http://127.0.0.1:{self.port}/health", timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.02)

    def translate(self, text="x"):
        UP.calls.clear()
        body = json.dumps({"timestamp": "2026-11-06T01:02:03.456Z", "uuid": str(uuid.uuid4()), "from": "kor", "to": "eng",
                           "text": text}).encode()
        req = Request(f"http://127.0.0.1:{self.port}/translate", data=body, headers={"Content-Type": "application/json"})
        try:
            with urlopen(req, timeout=60) as r:
                return r.status, r.read(), len(UP.calls)
        except HTTPError as e:
            return e.code, e.read(), len(UP.calls)

    def rows(self):
        db = sqlite3.connect(Path(self.dir.name) / "c.db")
        try:
            return db.execute("SELECT translated_text FROM trans_cache").fetchall()
        finally:
            db.close()

    def close(self):
        self.p.terminate()
        self.p.wait(timeout=15)
        self.log.close()
        self.dir.cleanup()


def translated_bytes(body):
    """The translatedText value as raw bytes (the body may carry invalid UTF-8)."""
    key = b'"translatedText":"'
    i = body.index(key) + len(key)
    j = i
    while body[j:j + 1] != b'"':
        j += 2 if body[j:j + 1] == b"\\" else 1
    lit = body[i - 1:j + 1]
    return json.loads(lit.decode("utf-8", "surrogateescape")).encode("utf-8", "surrogateescape")


class NormalOutput(unittest.TestCase):
    def test_no_truncation_and_colons(self):
        srv = Server()
        try:
            texts = ["at 10:30 today, ratio 3:1, see http://example.com/a?x=1 and [2001:db8::1]. Note: done :) " * 250,
                     "가나다 10:30 " * 2500, "emoji :smile: and 😀 removed; :+1: kept", "a:b:c Q: 왜? A: 이유",
                     "literal ~n stays? " .replace("~", "\\")]
            for t in texts:
                UP.reply = chat(t)
                status, body, calls = srv.translate(t[:50] + str(len(t)))
                want = ref.strip(ref.unescape(t.encode()))
                self.assertEqual(status, 200, body[:200])
                got = translated_bytes(body)
                self.assertEqual(got, want, f"{len(t.encode())} bytes in, {len(got)} out, {len(want)} expected")
            print(f"  normal: {len(texts)} replies up to {max(len(t.encode()) for t in texts)} bytes, exact")
        finally:
            srv.close()

    def test_invalid_utf8_kept(self):
        srv = Server()
        try:
            for raw in (b"ok \xf0", b"\xf0\x82\x98\x80 end", b"x\xed\xa0\x80y", b"\x80\x80:smile:"):
                UP.reply = chat_raw(b'"' + raw + b'"')
                status, body, _ = srv.translate(raw.hex())
                self.assertEqual(status, 200, body[:200])
                self.assertEqual(translated_bytes(body), ref.strip(ref.unescape(raw)), raw)
        finally:
            srv.close()

    def test_stream(self):
        srv = Server(stream=True)
        try:
            def sse(*events):
                return b"".join(b"data: " + e + b"\n\n" for e in events) + b"data: [DONE]\n\n"

            def delta(t):
                return b'{"choices":[{"delta":{"content":' + json.dumps(t, ensure_ascii=False).encode() + b"}}]}"
            piece = "가나다 10:30 " * 200                       # 3,200 bytes per event
            body = sse(*[delta(piece) for _ in range(30)])     # 96,000 bytes of content, past the old 64 KB stop
            UP.reply = body
            status, out, calls = srv.translate("long stream")
            want = ref.strip(ref.unescape((piece * 30).encode()))
            self.assertEqual((status, translated_bytes(out) == want, calls), (200, True, 1), out[:200])
            UP.reply = (b"data: " + delta("A") + b"\r\n\r\ndata: " + delta("B") + b"\r\n\r\n")
            status, out, _ = srv.translate("crlf")
            self.assertEqual((status, translated_bytes(out)), (200, b"AB"))
            before = len(srv.rows())
            for name, reply in [("error event", sse(delta("A"), b'{"error":{"message":"quota"}}')),
                                ("junk after JSON", b"data: " + delta("A") + b"BROKEN\n\n"),
                                ("raw NUL between events", sse(delta("A")) + b"\x00" + sse(delta("B"))),
                                ("content not a string", sse(b'{"choices":[{"delta":{"content":5}}]}'))]:
                UP.reply = reply
                status, out, calls = srv.translate(name)
                self.assertEqual((status, calls), (502, 1), f"{name}: {out[:200]}")
            self.assertEqual(len(srv.rows()), before, "nothing cached")
            print(f"  stream: {len(want)}-byte translation whole, CRLF framing, 4 failure kinds -> 502 uncached")
        finally:
            srv.close()

    def test_size_limit(self):
        for stream in (False, True):
            srv = Server(stream=stream)
            try:
                big = "y" * (4 * 1024 * 1024)
                UP.reply = chat(big) if not stream else b"data: " + b'{"choices":[{"delta":{"content":"' + big.encode() + b'"}}]}\n\n'
                status, out, calls = srv.translate("big")
                self.assertEqual((status, calls, srv.rows()), (502, 1, []), out[:200])   # one call: not retried
                self.assertIn(b"Upstream response too large", out)
                limit = 4 * 1024 * 1024
                fits = "z" * (limit - len(chat("")) - 16)
                UP.reply = chat(fits) if not stream else b'data: {"choices":[{"delta":{"content":"' + fits.encode() + b'"}}]}'
                self.assertLessEqual(len(UP.reply), limit)
                status, out, calls = srv.translate("fits")
                self.assertEqual((status, calls), (200, 1), out[:200])
            finally:
                srv.close()
        print("  size limit: 4 MB + n -> 502 once, uncached; just under 4 MB -> 200 (both modes)")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], "-v"])
