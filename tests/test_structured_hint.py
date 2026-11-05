"""SOURCE_HINT_MODE="model" through the real server with an offline codex
fixture (tests/fake_codex_structured.py) - PLAN_SOURCE_HINT_METADATA.md 4.3, 11.

    python3 tests/test_structured_hint.py

Structural violations must fail the request with 502 and cache nothing;
semantic hint errors must keep the translation and fall back to the script
classifier (expected values from tests/source_hint_ref.py).
"""
import json
import os
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
import source_hint_ref as R  # noqa: E402

SCHEMA = {"type": "object", "additionalProperties": False, "required": ["source_language", "mixed", "translation"],
          "properties": {"source_language": {"type": "string"}, "mixed": {"type": "boolean"},
                         "translation": {"type": "string"}}}
OK = "번역 결과"
HOOKS_BIN = os.environ.get("TB_HOOKS_BIN")   # transbasket built with -DTRANSBASKET_TEST_HOOKS


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def obj(source_language="kor", mixed=False, translation=OK):
    return json.dumps({"source_language": source_language, "mixed": mixed, "translation": translation},
                      ensure_ascii=False).encode()


class Server:
    def __init__(self, extra=None, run_dir=None, binary=None):
        self.binary = binary or str(ROOT / "transbasket")
        self.temp = tempfile.TemporaryDirectory(prefix="tb-structured-")
        self.dir = Path(self.temp.name)
        self.run_dir = Path(run_dir) if run_dir else self.dir / "run"
        self.run_dir.mkdir(parents=True, exist_ok=True)
        self.port = free_port()
        self.cache = self.dir / "cache.db"
        self.reply = self.dir / "reply.bin"
        self.capture = self.dir / "capture.jsonl"
        fake = self.dir / "codex"
        fake.write_bytes((ROOT / "tests/fake_codex_structured.py").read_bytes())
        fake.chmod(0o700)
        (self.dir / "role.txt").write_text("You are a translator.", encoding="utf-8")
        (self.dir / "explicit.txt").write_text("EXPLICIT Translate to [TARGET LANGUAGE]", encoding="utf-8")
        values = {"OPENAI_BASE_URL": "http://127.0.0.1:9/v1", "OPENAI_MODEL": "m", "OPENAI_API_KEY": "k",
                  "LISTEN": "127.0.0.1", "PORT": str(self.port), "MODEL_TYPE": "codex", "CODEX_INTERFACE": "exec",
                  "PROMPT_PREFIX_FILE": "explicit.txt", "LANGUAGE_TABLE_FILE": str(ROOT / "language_tables.conf"),
                  "TRANS_CACHE_TYPE": "sqlite", "TRANS_CACHE_SQLITE_PATH": str(self.cache),
                  "TRANS_CACHE_THRESHOLD": "1", "TRANS_CACHE_CLEANUP_ENABLED": "false", "CODEX_BIN": str(fake),
                  "CODEX_RUN_DIR": str(self.run_dir), "CODEX_TIMEOUT": "10", "CODEX_MAX_CONCURRENCY": "1",
                  "SOURCE_HINT_MODE": "model"}
        values.update(extra or {})
        self.conf = self.dir / "t.conf"
        self.conf.write_text("".join(f'{k}="{v}"\n' for k, v in values.items()))
        self.env = {**os.environ, "TB_STRUCT_CAPTURE": str(self.capture), "TB_STRUCT_REPLY": str(self.reply)}

    def __enter__(self):
        self.log = open(self.dir / "server.log", "w")
        self.proc = subprocess.Popen([self.binary, "-c", str(self.conf), "-r", "role.txt"],
                                     cwd=self.dir, stdout=self.log, stderr=self.log, env=self.env)
        for _ in range(200):
            try:
                urlopen(f"http://127.0.0.1:{self.port}/health", timeout=0.2).close()
                return self
            except OSError:
                time.sleep(0.02)
        raise RuntimeError("server did not start: " + (self.dir / "server.log").read_text())

    def __exit__(self, *exc):
        self.proc.terminate()
        self.proc.wait(timeout=8)
        self.log.close()
        self.temp.cleanup()

    def ask(self, text, reply=None, source="auto", env=None):
        if reply is not None:
            self.reply.write_bytes(reply)
        payload = {"from": source, "to": "eng", "text": text, "timestamp": "2026-11-06T01:23:45.678Z",
                   "uuid": "550e8400-e29b-41d4-a716-446655440000"}
        req = Request(f"http://127.0.0.1:{self.port}/translate", json.dumps(payload).encode(),
                      {"Content-Type": "application/json"})
        try:
            r = urlopen(req, timeout=30)
        except HTTPError as e:
            r = e
        with r:
            return r.status, json.load(r)

    def hint(self, text, source="auto"):
        db = sqlite3.connect(self.cache)
        try:
            return db.execute("SELECT h.source_hint, h.mixed, h.origin FROM trans_cache c LEFT JOIN "
                              "trans_cache_source_hint h ON h.hash = c.hash WHERE c.source_text = ? AND c.from_lang = ?",
                              (text, source)).fetchone()
        finally:
            db.close()

    def runs(self):
        return [json.loads(l) for l in self.capture.read_text(encoding="utf-8").splitlines()] if self.capture.exists() else []

    def leftovers(self):
        return sorted(p.name for p in self.run_dir.iterdir())


def script(text):
    lang, mixed = R.classify(text.encode())
    return (lang, int(mixed), "script")


class StructuredHint(unittest.TestCase):
    def test_accepted_and_semantic_fallbacks(self):
        cases = [  # (text, reply, expected hint)
            ("안녕하세요 반갑습니다", obj("kor"), ("kor", 0, "model")),
            ("오늘 회의 자료", obj(" Korean "), ("kor", 0, "model")),                   # name, trimmed
            ("明天的會議下午三點", obj("zh-Hant"), ("zht", 0, "model")),                 # alias
            ("2026-11-06 12:00 | 42", obj("und"), ("und", 0, "model")),
            ("회의 일정. Meeting at 3pm.", obj("kor", True), ("kor", 1, "model")),
            ("첫 줄", obj("xx"), None),                                               # unknown code
            ("둘째 줄", obj("auto"), None),
            ("셋째 줄", b'{"translation":"' + OK.encode() + b'","source_language":"kor"}', None),       # mixed missing
            ("넷째 줄", b'{"translation":"' + OK.encode() + b'","source_language":"kor","mixed":null}', None),
            ("다섯째 줄", b'{"translation":"' + OK.encode() + b'","source_language":"kor","mixed":1}', None),
            ("여섯째 줄", b'{"translation":"' + OK.encode() + b'","source_language":"kor","mixed":"false"}', None),
            ("일곱째 줄", b'{"translation":"' + OK.encode() + b'","source_language":7,"mixed":false}', None),
            ("한글로만 된 문장입니다", obj("jpn"), None),                                # contradicted
        ]
        with Server() as s:
            for text, reply, expected in cases:
                status, body = s.ask(text, reply)
                self.assertEqual((status, body.get("translatedText")), (200, OK), text)
                self.assertEqual(s.hint(text), expected or script(text), text)
            self.assertEqual(s.leftovers(), [], "temp dirs and schema files removed")
            runs = s.runs()
            self.assertTrue(all("--output-schema" in r["argv"] and r["schema"] == SCHEMA for r in runs))
            self.assertTrue(all("Return a JSON object matching the provided output schema" in r["prompt"] and
                                "Return ONLY the JSON object described above" in r["prompt"] for r in runs))

    def test_literal_backslash_u0000_is_kept(self):
        with Server() as s:
            status, body = s.ask("역슬래시 시험", b'{"translation":"a\\\\u0000b","source_language":"kor","mixed":false}')
            self.assertEqual((status, body.get("translatedText")), (200, "a\\u0000b"))

    def test_structural_violations_fail_and_cache_nothing(self):
        bad = {
            "non-JSON": b"hello",
            "trailing garbage": obj() + b" trailing",
            "trailing control byte": obj() + b"\x01",
            "invalid UTF-8": b'{"translation":"\xff","source_language":"kor","mixed":false}',
            "NUL escape in value": b'{"translation":"a\\u0000b","source_language":"kor","mixed":false}',
            "NUL escape in key": b'{"translation\\u0000junk":"WRONG","translation":"' + OK.encode() + b'"}',
            "NUL escape in hint": b'{"translation":"ok","source_language":"kor\\u0000x","mixed":false}',
            "invalid escape": b'{"translation":"before\\uZZZZafter","source_language":"kor","mixed":false}',
            "raw NUL then garbage": obj() + b"\x00garbage",
            "duplicate translation": b'{"translation":"a","translation":"b","source_language":"kor","mixed":false}',
            "root array": b'["translation"]',
            "BOM": b"\xef\xbb\xbf" + obj(),
            "translation missing": b'{"source_language":"kor","mixed":false}',
            "translation not a string": b'{"translation":1,"source_language":"kor","mixed":false}',
            "translation empty": obj(translation=""),
        }
        with Server() as s:
            for i, (name, reply) in enumerate(bad.items()):
                text = f"구조 위반 {i}"
                status, body = s.ask(text, reply)
                self.assertEqual(status, 502, name)
                self.assertNotIn("translatedText", body, name)
                self.assertIsNone(s.hint(text), f"{name}: nothing cached")
            # missing -o file (reply only on stdout) and oversized stdout with a valid file
            s.env["TB_STRUCT_NO_FILE"] = "1"
        with Server() as s:
            s.env["TB_STRUCT_NO_FILE"] = "1"
            s.proc.terminate(); s.proc.wait()
            s.__enter__()
            status, _ = s.ask("파일 없음", obj())
            self.assertEqual(status, 502, "-o file missing")
            self.assertIsNone(s.hint("파일 없음"))
        with Server() as s:
            s.env["TB_STRUCT_STDOUT"] = str(4 * 1024 * 1024 + 1)
            s.proc.terminate(); s.proc.wait()
            s.__enter__()
            status, _ = s.ask("표준출력 초과", obj())
            self.assertEqual(status, 502, "stdout overflow")
            # slots were released after every failure: CODEX_MAX_CONCURRENCY=1 and the next call works
            s.env.pop("TB_STRUCT_STDOUT")
            s.proc.terminate(); s.proc.wait()
            s.__enter__()
            t0 = time.monotonic()
            status, body = s.ask("슬롯 확인", obj("kor"))
            self.assertEqual((status, body.get("translatedText")), (200, OK))
            self.assertLess(time.monotonic() - t0, 5)
            self.assertEqual(s.leftovers(), [])

    def test_explicit_request_keeps_plain_prompt(self):
        with Server() as s:
            s.ask("감사합니다", OK.encode(), source="kor")
            run = s.runs()[-1]
            self.assertNotIn("--output-schema", run["argv"])
            self.assertIn("Return ONLY the translated text - no explanations", run["prompt"])
            self.assertIsNone(s.hint("감사합니다", source="kor")[0])

    @unittest.skipUnless(HOOKS_BIN, "needs the TRANSBASKET_TEST_HOOKS build (make test-structured-hint)")
    def test_schema_file_failure_keeps_cwd_and_falls_back_to_plain(self):
        with Server(binary=HOOKS_BIN) as s:
            s.env["TB_TEST_FAIL_SCHEMA_WRITE"] = "1"
            s.proc.terminate(); s.proc.wait()
            s.__enter__()
            text = "스키마 실패 시험"
            status, body = s.ask(text, OK.encode())
            self.assertEqual((status, body.get("translatedText")), (200, OK))
            run = s.runs()[-1]
            self.assertNotIn("--output-schema", run["argv"], "plain fallback")
            self.assertIn("-C", run["argv"], "temp cwd kept for the plain run")
            self.assertIn("Return ONLY the translated text", run["prompt"])
            self.assertEqual(s.hint(text), script(text))
            self.assertEqual(s.leftovers(), [], "partial schema file and temp cwd removed")

    @unittest.skipUnless(HOOKS_BIN, "needs the TRANSBASKET_TEST_HOOKS build (make test-structured-hint)")
    def test_prompt_failure_releases_slot_files_and_dir(self):
        with Server(binary=HOOKS_BIN) as s:
            s.env["TB_TEST_FAIL_PROMPT"] = "1"
            s.proc.terminate(); s.proc.wait()
            s.__enter__()
            for i in range(3):                     # CODEX_MAX_CONCURRENCY=1: a leaked slot would make #2 wait/503
                t0 = time.monotonic()
                status, _ = s.ask(f"프롬프트 실패 {i}", obj())
                self.assertEqual(status, 502)
                self.assertLess(time.monotonic() - t0, 3, "slot released")
            self.assertEqual(s.runs(), [], "codex never started")
            self.assertEqual(s.leftovers(), [], "schema file and temp cwd removed")

    def test_deadline_during_structured_call(self):
        with Server({"TRANSLATION_TOTAL_TIMEOUT": "1"}) as s:
            s.env["TB_STRUCT_SLEEP"] = "3"
            s.proc.terminate(); s.proc.wait()
            s.__enter__()
            status, _ = s.ask("마감 시험", obj())
            self.assertEqual(status, 504)
            self.assertIsNone(s.hint("마감 시험"))
            time.sleep(0.5)
            self.assertEqual(s.leftovers(), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
