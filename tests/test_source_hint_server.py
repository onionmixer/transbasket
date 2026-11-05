"""Server-level source_hint tests (PLAN_SOURCE_HINT_METADATA.md section 11).

    python3 tests/test_source_hint_server.py

Reuses the offline fixtures of tests/test_source_auto.py (fake OpenAI upstream,
fake codex). Hint writes are counted by a test-only SQLite trigger that logs
every INSERT/UPDATE on trans_cache_source_hint, so "exactly one write" is
measured, not inferred from row counts.
"""
from concurrent.futures import ThreadPoolExecutor
import json
import sqlite3
import sys
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import source_hint_ref as R  # noqa: E402
import test_source_auto as A  # noqa: E402


class HintServerTests(A.AutoTests):
    @classmethod
    def setUpClass(cls):
        cls.calls = []
        cls.upstream_status = 200
        cls.delay = 0.0

        class Upstream(BaseHTTPRequestHandler):
            def do_POST(self):
                data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                cls.calls.append(data)
                time.sleep(cls.delay)
                self.send_response(cls.upstream_status)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(json.dumps({"choices": [{"message": {"content": "번역 결과"}}]}).encode())

            def log_message(self, *args):
                pass

        cls.upstream = ThreadingHTTPServer(("127.0.0.1", 0), Upstream)
        cls.thread = threading.Thread(target=cls.upstream.serve_forever, daemon=True)
        cls.thread.start()

    def setUp(self):
        super().setUp()
        type(self).delay = 0.0

    def instrument(self):
        """Create the DB through the server, then add the write-counting trigger."""
        with self.server():
            pass
        db = sqlite3.connect(self.cache)
        db.executescript("""
            CREATE TABLE IF NOT EXISTS hint_writes(kind TEXT);
            CREATE TRIGGER IF NOT EXISTS count_hint_insert AFTER INSERT ON trans_cache_source_hint
              BEGIN INSERT INTO hint_writes VALUES ('insert'); END;
            CREATE TRIGGER IF NOT EXISTS count_hint_update AFTER UPDATE ON trans_cache_source_hint
              BEGIN INSERT INTO hint_writes VALUES ('update'); END;""")
        db.close()

    def query(self, sql):
        db = sqlite3.connect(self.cache)
        try:
            return db.execute(sql).fetchall()
        finally:
            db.close()

    def test_auto_request_stores_script_hint(self):
        self.config()
        self.instrument()
        with self.server():
            self.assertEqual(self.request(source="auto"), 200)
            self.assertEqual(self.request(source="auto"), 200)        # cache hit
            self.assertEqual(self.request(source="eng", text="Hello there"), 200)
        expected = R.classify(A.SOURCE.encode())
        rows = self.query("SELECT c.from_lang, h.source_hint, h.mixed, h.origin, h.hint_version "
                          "FROM trans_cache c JOIN trans_cache_source_hint h ON h.hash = c.hash")
        self.assertEqual(rows, [("auto", expected[0], int(expected[1]), "script", 1)])
        self.assertEqual(len(type(self).calls), 2, "the hit must not reach the engine")
        self.assertEqual(self.query("SELECT count(*) FROM hint_writes"), [(1,)], "one write; hit and explicit write none")
        view = self.query("SELECT from_lang, effective_source FROM trans_cache_effective_source ORDER BY id")
        self.assertEqual(view, [("auto", expected[0]), ("eng", "eng")])

    def test_mode_off_stores_no_hint(self):
        self.config()
        self.config_path.write_text(self.config_path.read_text() + 'SOURCE_HINT_MODE="off"\n')
        with self.server():
            self.assertEqual(self.request(source="auto"), 200)
        self.assertEqual(self.query("SELECT count(*) FROM trans_cache"), [(1,)])
        self.assertEqual(self.query("SELECT count(*) FROM trans_cache_source_hint"), [(0,)])

    def test_coalesced_requests_write_one_hint(self):
        self.config()
        self.instrument()
        type(self).delay = 1.0
        with self.server():
            with ThreadPoolExecutor(8) as pool:
                statuses = list(pool.map(lambda _: self.request(source="auto"), range(8)))
        self.assertEqual(statuses, [200] * 8)
        self.assertEqual(len(type(self).calls), 1, "one engine call for the group")
        self.assertEqual(self.query("SELECT count(*) FROM hint_writes"), [(1,)], "one hint write for the group")

    def test_text_backend_persists_hint(self):
        self.config(cache="text")
        with self.server():
            self.assertEqual(self.request(source="auto"), 200)
            self.assertEqual(self.request(source="eng", text="Hello there"), 200)
        rows = [json.loads(line) for line in self.cache.read_text(encoding="utf-8").splitlines()]
        expected = R.classify(A.SOURCE.encode())
        by_from = {r["from"]: r for r in rows}
        self.assertEqual((by_from["auto"]["source_hint"], by_from["auto"]["hint_mixed"],
                          by_from["auto"]["hint_origin"], by_from["auto"]["hint_version"]),
                         (expected[0], expected[1], "script", 1))
        self.assertNotIn("source_hint", by_from["eng"])


if __name__ == "__main__":
    names = [n for n in vars(HintServerTests) if n.startswith("test_")]
    suite = unittest.TestSuite(HintServerTests(n) for n in names)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.exit(0 if result.wasSuccessful() else 1)
