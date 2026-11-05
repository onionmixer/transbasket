#!/usr/bin/env python3
"""Offline end-to-end tests: make test-auto (stdlib only, temporary caches)."""
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import tempfile
import threading
import time
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen
import uuid

ROOT = Path(__file__).resolve().parents[1]
TIMESTAMP = "2026-11-03T01:23:45.678Z"
SOURCE = "AUTO_SOURCE_SENTINEL {TARGET_LANG} literal\\n 안녕"


def digest(source, target, text):
    return hashlib.sha256(f"{source}|{target}|{text}".encode()).hexdigest()


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


@contextmanager
def database(path):
    db = sqlite3.connect(path)
    try:
        with db:
            yield db
    finally:
        db.close()


class AutoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.calls = []
        cls.upstream_status = 200

        class Upstream(BaseHTTPRequestHandler):
            def do_POST(self):
                data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                cls.calls.append(data)
                self.send_response(cls.upstream_status)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({"choices": [{"message": {"content": "번역 결과"}}]}).encode())

            def log_message(self, *args):
                pass

        cls.upstream = ThreadingHTTPServer(("127.0.0.1", 0), Upstream)
        cls.thread = threading.Thread(target=cls.upstream.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.upstream.shutdown()
        cls.upstream.server_close()
        cls.thread.join()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="transbasket-auto-")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        type(self).calls.clear()
        type(self).upstream_status = 200
        self.capture = self.directory / "prompts.jsonl"

    def config(self, backend="normal", cache="sqlite", threshold=1, custom=None, gemma=False):
        self.port = free_port()
        self.cache = self.directory / ("cache.db" if cache == "sqlite" else "cache.jsonl")
        (self.directory / "explicit.txt").write_text(
            "EXPLICIT_ONLY {SOURCE_LANG} {SOURCE_CODE} {TARGET_LANG} {TARGET_CODE}\n{TEXT}"
            if gemma else "EXPLICIT_ONLY Translate to [TARGET LANGUAGE]", encoding="utf-8")
        (self.directory / "role.txt").write_text("You are a translator.", encoding="utf-8")
        # This executable is copied so the test fixture need not have executable git mode.
        fake = self.directory / "codex"
        fake.write_bytes((ROOT / "tests/fake_codex_auto.py").read_bytes())
        fake.chmod(0o700)
        values = {
            "OPENAI_BASE_URL": f"http://127.0.0.1:{self.upstream.server_port}/v1",
            "OPENAI_MODEL": "offline-fixture", "OPENAI_API_KEY": "fixture",
            "LISTEN": "127.0.0.1", "PORT": str(self.port),
            "MODEL_TYPE": "normal" if backend == "normal" else "codex",
            "PROMPT_PREFIX_FILE": "explicit.txt",
            "LANGUAGE_TABLE_FILE": str(ROOT / "language_tables.conf"),
            "TRANS_CACHE_TYPE": cache, "TRANS_CACHE_FILE": str(self.cache),
            "TRANS_CACHE_SQLITE_PATH": str(self.cache), "TRANS_CACHE_THRESHOLD": str(threshold),
            "TRANS_CACHE_CLEANUP_ENABLED": "false", "CODEX_BIN": str(fake),
            "CODEX_INTERFACE": "mcp" if backend == "mcp" else "exec",
            "CODEX_RUN_DIR": str(self.directory), "CODEX_TIMEOUT": "5",
        }
        if custom is not None:
            (self.directory / "auto.txt").write_text(custom, encoding="utf-8")
            values["PROMPT_PREFIX_AUTO_FILE"] = "auto.txt"
        self.config_path = self.directory / "test.conf"
        self.config_path.write_text("".join(f'{key}="{value}"\n' for key, value in values.items()))

    @contextmanager
    def server(self):
        log_path = self.directory / "server.log"
        with log_path.open("a+") as log:
            process = subprocess.Popen(
                [str(ROOT / "transbasket"), "-c", str(self.config_path), "-r", "role.txt"],
                cwd=self.directory, stdout=log, stderr=log,
                env={**os.environ, "TB_AUTO_CAPTURE": str(self.capture)})
            try:
                for _ in range(150):
                    if process.poll() is not None:
                        self.fail(log_path.read_text())
                    try:
                        with urlopen(f"http://127.0.0.1:{self.port}/health", timeout=0.2):
                            break
                    except OSError:
                        time.sleep(0.02)
                else:
                    self.fail("server startup timeout")
                yield process
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                        self.fail("server shutdown timeout")

    def request(self, source="auto", target="kor", text=SOURCE, **updates):
        payload = {"from": source, "to": target, "text": text,
                   "timestamp": TIMESTAMP, "uuid": str(uuid.uuid4())}
        payload.update(updates)
        request = Request(f"http://127.0.0.1:{self.port}/translate",
                          json.dumps(payload).encode(), {"Content-Type": "application/json"})
        try:
            response = urlopen(request, timeout=10)
        except HTTPError as error:
            response = error
        with response:
            result = json.load(response)
            if response.status == 200:
                self.assertEqual(result, {"timestamp": payload["timestamp"], "uuid": payload["uuid"],
                                          "translatedText": "번역 결과"})
            return response.status

    def check_prompt(self, prompt, target="Korean"):
        self.assertIn("Determine the source language", prompt)
        self.assertIn(target, prompt)
        self.assertNotIn("EXPLICIT_ONLY", prompt)
        self.assertNotIn("FROM auto", prompt)
        self.assertNotIn("{SOURCE_", prompt)
        self.assertEqual(prompt.count(SOURCE), 1)

    def test_contract_cache_and_regression(self):
        self.config()
        with self.server():
            for source in ("auto", "AUTO", "Auto"):
                self.assertEqual(self.request(source), 200)
            self.assertEqual(len(self.calls), 1)
            self.check_prompt("\n".join(m["content"] for m in self.calls[0]["messages"]))
            for source in (None, 12, "", " auto", "auto ", "automatic", "aut", "xyz", "a" * 12000):
                self.assertEqual(self.request(source), 422)
            for target in ("auto", "AUTO", None, 12, "xyz"):
                self.assertEqual(self.request(target=target), 422)
            self.assertEqual(self.request(text=""), 422)
            self.assertEqual(self.request(text="a" * 10001), 422)
            self.assertEqual(self.request(uuid="invalid"), 422)
            self.assertEqual(self.request(timestamp="invalid"), 422)
            for source in ("eng", "English", "ENG"):
                self.assertEqual(self.request(source), 200)
            self.assertEqual(len(self.calls), 2)
            self.assertIn("EXPLICIT_ONLY", self.calls[-1]["messages"][1]["content"])
            for target in ("zh-Hans", "zh-Hant"):
                self.assertEqual(self.request(target=target), 200)
            self.check_prompt("\n".join(m["content"] for m in self.calls[-1]["messages"]), "Chinese (Traditional)")
        with database(self.cache) as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], 2)
            rows = db.execute("SELECT from_lang,to_lang,hash FROM trans_cache").fetchall()
            self.assertIn(("auto", "kor", digest("auto", "kor", SOURCE)), rows)
            self.assertIn(("eng", "kor", digest("eng", "kor", SOURCE)), rows)
        with self.server():
            self.assertEqual(self.request("AUTO"), 200)
        self.assertEqual(len(self.calls), 4)

    def test_threshold_text_restart_and_tools(self):
        self.config(cache="text", threshold=2, gemma=True)
        with self.server():
            for _ in range(3):
                self.assertEqual(self.request(), 200)
            self.assertEqual(len(self.calls), 2)
            self.assertEqual(self.request("eng"), 200)
            with ThreadPoolExecutor(max_workers=8) as executor:
                self.assertEqual(list(executor.map(lambda _: self.request("AUTO"), range(20))), [200] * 20)
            self.assertEqual(len(self.calls), 3)
        with self.server():
            self.assertEqual(self.request(), 200)
        self.assertEqual(len(self.calls), 3)
        tool = [str(ROOT / "cache_tool"), "-f", str(self.cache)]
        for args in (["list", "AUTO", "Korean"], ["search", "Auto", "kor", SOURCE],
                     ["export", "auto", "kor"], ["stats"]):
            result = subprocess.run(tool + args, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("auto", result.stdout)
        self.assertNotEqual(subprocess.run(tool + ["clear", "eng", "auto"], capture_output=True).returncode, 0)
        result = subprocess.run(tool + ["clear", "AUTO", "kor"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = [json.loads(line) for line in self.cache.read_text().splitlines()]
        self.assertEqual([row["from"] for row in rows], ["eng"])

    def test_custom_single_message_and_missing_language_table(self):
        self.config(gemma=True, custom="CUSTOM {TARGET_LANG} {TARGET_CODE}\n{TEXT}")
        self.config_path.write_text(self.config_path.read_text().replace(
            str(ROOT / "language_tables.conf"), str(self.directory / "no-table")))
        with self.server():
            self.assertEqual(self.request(), 200)
            messages = self.calls[-1]["messages"]
            self.assertEqual(len(messages), 2)
            self.check_prompt(messages[1]["content"])
            self.assertIn("CUSTOM Korean kor", messages[1]["content"])
            self.assertEqual(self.request("eng"), 200)

    def test_codex_exec_and_mcp_prompts(self):
        for interface in ("exec", "mcp"):
            with self.subTest(interface=interface):
                self.config(backend=interface, custom="CUSTOM {TARGET_LANG} {TARGET_CODE}\n{TEXT}", gemma=True)
                # New source text keeps the second backend from using the first backend's cache.
                with self.server():
                    self.assertEqual(self.request(text=SOURCE + interface), 200)
                    self.assertEqual(self.request("AUTO", text=SOURCE + interface), 200)
                prompts = [json.loads(line) for line in self.capture.read_text().splitlines()]
                self.assertEqual(len(prompts), 1 if interface == "exec" else 2)
                self.check_prompt(prompts[-1])
                self.assertNotIn("{TEXT}", prompts[-1])
                # Original text is enclosed by the same generated marker on both sides.
                lines = prompts[-1].splitlines()
                source_line = next(i for i, line in enumerate(lines) if SOURCE in line)
                self.assertEqual(lines[source_line - 1], lines[source_line + 1])

    def test_invalid_auto_templates_fail_startup(self):
        for custom in ("{SOURCE_LANG}", "{SOURCE_CODE}", "   ", "missing"):
            with self.subTest(custom=custom):
                self.config(custom=custom)
                if custom == "missing":
                    (self.directory / "auto.txt").unlink()
                result = subprocess.run([str(ROOT / "transbasket"), "-c", str(self.config_path),
                                         "-r", "role.txt"], capture_output=True, text=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Error:", result.stdout + result.stderr)

    def migrate(self, db_path, reverse=False):
        empty = self.directory / "empty.jsonl"
        empty.touch()
        output = self.directory / "export.jsonl"
        args = ["--from", "sqlite" if reverse else "text", "--from-config", str(db_path if reverse else empty),
                "--to", "text" if reverse else "sqlite", "--to-config", str(output if reverse else db_path)]
        return subprocess.run([str(ROOT / "cache_tool"), "migrate"] + args, capture_output=True, text=True, timeout=5)

    def old_database(self, version=0, external=False):
        path = self.directory / f"old-{version}-{external}.db"
        sql = (ROOT / "cache_sqlite_schema.sql").read_text().replace(
            "length(from_lang) = 3 OR from_lang = 'auto'", "length(from_lang) = 3").replace(
            "PRAGMA user_version = 2", f"PRAGMA user_version = {version}")
        if not external:
            sql = sql.replace("CHECK(last_used >= created_at)", "CHECK(1)")
        with database(path) as db:
            db.executescript(sql)
            db.execute("INSERT INTO trans_cache VALUES(?,?,?,?,?,?,?,?,?)",
                       (7, digest("eng", "kor", "hello"), "eng", "kor", "hello", "안녕", 9, 20, 10))
            db.execute("INSERT INTO trans_cache SELECT 100,?,'kor','eng','x','y',1,20,10", ("f" * 64,))
            db.execute("DELETE FROM trans_cache WHERE id=100")
            db.executescript("CREATE INDEX custom_source ON trans_cache(source_text);"
                             "CREATE TABLE audit(message TEXT);"
                             "CREATE TRIGGER custom_insert AFTER INSERT ON trans_cache BEGIN INSERT INTO audit VALUES(new.from_lang); END;"
                             "CREATE VIEW cache_view AS SELECT source_text FROM trans_cache;")
        return path

    def test_migration_preserves_data_objects_sequence_and_roundtrip(self):
        for version, external in ((0, False), (1, True)):
            with self.subTest(version=version):
                path = self.old_database(version, external)
                with database(path) as db:
                    before = db.execute("SELECT * FROM trans_cache").fetchall()
                result = self.migrate(path)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                with database(path) as db:
                    self.assertEqual(db.execute("SELECT * FROM trans_cache").fetchall(), before)
                    self.assertEqual(db.execute("PRAGMA user_version").fetchone(), (2,))
                    self.assertEqual(db.execute("SELECT * FROM cache_view").fetchall(), [("hello",)])
                    self.assertIsNotNone(db.execute("SELECT name FROM sqlite_master WHERE name='custom_source'").fetchone())
                    for source, target in (("abcd", "kor"), ("eng", "auto")):
                        with self.assertRaises(sqlite3.IntegrityError):
                            db.execute("INSERT INTO trans_cache(hash,from_lang,to_lang,source_text,translated_text,count,last_used,created_at) "
                                       "VALUES(?,?,?,'new','번역',1,20,10)", ("a" * 64, source, target))
                    db.execute("INSERT INTO trans_cache(hash,from_lang,to_lang,source_text,translated_text,count,last_used,created_at) "
                               "VALUES(?,'auto','kor','new','번역',1,20,10)", (digest("auto", "kor", "new"),))
                    self.assertEqual(db.execute("SELECT id FROM trans_cache WHERE from_lang='auto'").fetchone(), (101,))
                    self.assertEqual(db.execute("SELECT * FROM audit").fetchall(), [("auto",)])
                    ddl = db.execute("SELECT sql FROM sqlite_master WHERE name='trans_cache'").fetchone()[0]
                    self.assertEqual("last_used >= created_at" in ddl, external)
                self.assertEqual(self.migrate(path).returncode, 0)
                self.assertEqual(self.migrate(path, reverse=True).returncode, 0)
                rows = [json.loads(line) for line in (self.directory / "export.jsonl").read_text().splitlines()]
                self.assertIn(("auto", digest("auto", "kor", "new")), [(row["from"], row["hash"]) for row in rows])
                roundtrip = self.directory / f"roundtrip-{version}.db"
                result = subprocess.run([str(ROOT / "cache_tool"), "migrate", "--from", "text",
                    "--from-config", str(self.directory / "export.jsonl"), "--to", "sqlite",
                    "--to-config", str(roundtrip)], capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                with database(roundtrip) as db:
                    self.assertEqual(db.execute("SELECT from_lang,hash FROM trans_cache WHERE from_lang='auto'").fetchone(),
                                     ("auto", digest("auto", "kor", "new")))

    def test_new_sql_schema_and_empty_legacy_database(self):
        for current in (False, True):
            with self.subTest(current=current):
                path = self.directory / f"empty-{current}.db"
                sql = (ROOT / "cache_sqlite_schema.sql").read_text()
                if not current:
                    sql = sql.replace("length(from_lang) = 3 OR from_lang = 'auto'", "length(from_lang) = 3")
                    sql = sql.replace("PRAGMA user_version = 2", "PRAGMA user_version = 0")
                with database(path) as db:
                    db.executescript(sql)
                result = self.migrate(path)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                with database(path) as db:
                    self.assertEqual(db.execute("PRAGMA user_version").fetchone(), (2,))
                    self.assertEqual(db.execute("SELECT count(*) FROM trans_cache").fetchone(), (0,))
                    db.execute("INSERT INTO trans_cache(hash,from_lang,to_lang,source_text,translated_text,count,last_used,created_at) "
                               "VALUES(?,'auto','kor','x','y',1,20,10)", (digest("auto", "kor", "x"),))

    def test_migration_failure_is_atomic(self):
        for failure in ("future", "collision", "locked", "unknown", "copy", "readonly"):
            with self.subTest(failure=failure):
                path = self.old_database()
                with database(path) as db:
                    if failure == "future":
                        db.execute("PRAGMA user_version=99")
                    elif failure == "collision":
                        db.execute("CREATE TABLE trans_cache_auto_new(x)")
                    elif failure == "unknown":
                        db.execute("ALTER TABLE trans_cache ADD COLUMN unknown TEXT")
                    elif failure == "copy":
                        db.execute("PRAGMA ignore_check_constraints=ON")
                        db.execute("UPDATE trans_cache SET count=0")
                    original = db.execute("SELECT * FROM trans_cache").fetchall()
                    ddl = db.execute("SELECT sql FROM sqlite_master WHERE name='trans_cache'").fetchone()[0]
                with database(path) as lock:
                    if failure == "locked":
                        lock.execute("BEGIN IMMEDIATE")
                    if failure == "readonly":
                        path.chmod(0o444)
                    result = self.migrate(path)
                    path.chmod(0o600)
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                with database(path) as db:
                    self.assertEqual(db.execute("SELECT * FROM trans_cache").fetchall(), original)
                    self.assertEqual(db.execute("SELECT sql FROM sqlite_master WHERE name='trans_cache'").fetchone()[0], ddl)
                path.unlink()

    def test_normal_error_mapping(self):
        self.config()
        type(self).upstream_status = 400
        with self.server():
            self.assertEqual(self.request(), 502)
        self.assertEqual(len(self.calls), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
