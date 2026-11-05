#!/usr/bin/env python3
"""Deterministic local HTTP/process tests; never calls an external model."""
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import threading
import time
import unittest
from unittest.mock import patch
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen
import uuid

from test_source_auto import AutoTests, ROOT, TIMESTAMP, database


class CoalescingTests(AutoTests):
    @classmethod
    def setUpClass(cls):
        cls.calls = []
        cls.gate = threading.Event()
        cls.gate.set()
        cls.call_lock = threading.Lock()
        cls.upstream_status = 200
        cls.reply = "번역 결과"
        cls.payload_override = None

        class Upstream(BaseHTTPRequestHandler):
            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                with cls.call_lock:
                    cls.calls.append(body)
                if not cls.gate.wait(20):
                    return
                try:
                    self.send_response(cls.upstream_status)
                    self.send_header("Content-Type", "application/json")
                    self.end_headers()
                    body = cls.payload_override if cls.payload_override is not None else {"choices": [{"message": {"content": cls.reply}}]}
                    self.wfile.write(json.dumps(body).encode())
                except (BrokenPipeError, ConnectionResetError):
                    pass

            def log_message(self, *args):
                pass

        cls.upstream = ThreadingHTTPServer(("127.0.0.1", 0), Upstream)
        cls.thread = threading.Thread(target=cls.upstream.serve_forever, daemon=True)
        cls.thread.start()

    def setUp(self):
        super().setUp()
        type(self).reply = "번역 결과"
        type(self).payload_override = None
        self.gate.set()
        self.addCleanup(self.gate.set)

    def configure(self, **extra):
        with self.config_path.open("a") as config:
            for key, value in extra.items():
                config.write(f'{key}="{value}"\n')

    def log(self):
        path = self.directory / "server.log"
        return path.read_text() if path.exists() else ""

    def wait_for(self, predicate, timeout=6):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if predicate():
                return
            time.sleep(0.01)
        self.fail("condition timed out\n" + self.log()[-6000:])

    def http(self, text="same", source="auto", target="kor", allow_disconnect=False):
        payload = {"timestamp": TIMESTAMP, "uuid": str(uuid.uuid4()), "from": source,
                   "to": target, "text": text}
        request = Request(f"http://127.0.0.1:{self.port}/translate", json.dumps(payload).encode(),
                          {"Content-Type": "application/json"})
        try:
            try:
                response = urlopen(request, timeout=12)
            except HTTPError as error:
                response = error
            with response:
                result = json.load(response)
                self.assertEqual(result["uuid"], payload["uuid"])
                if response.status == 200:
                    self.assertEqual(result["timestamp"], TIMESTAMP)
                return response.status, result, response.headers.get("Retry-After")
        except (OSError, URLError):
            if not allow_disconnect:
                raise
            return 0, {}, None

    def batch(self, count=10, **kwargs):
        previous = len(self.calls)
        joined = self.log().count(" joined (waiters=")
        self.gate.clear()
        with ThreadPoolExecutor(max_workers=count) as pool:
            futures = [pool.submit(self.http, **kwargs) for _ in range(count)]
            try:
                self.wait_for(lambda: len(self.calls) > previous)
                self.wait_for(lambda: self.log().count(" joined (waiters=") >= joined + count - 1)
            finally:
                self.gate.set()
            return [future.result(timeout=12) for future in futures]

    def rows(self, process, cache):
        if cache == "sqlite":
            with database(self.cache) as db:
                return db.execute("SELECT source_text,count,translated_text FROM trans_cache ORDER BY id").fetchall()
        saves = self.log().count("SIGHUP cache save completed")
        process.send_signal(signal.SIGHUP)
        self.wait_for(lambda: self.log().count("SIGHUP cache save completed") > saves)
        return [(row["source"], row["count"], row["target"])
                for row in map(json.loads, self.cache.read_text().splitlines())]

    def test_threshold_counts_once_and_flushes(self):
        for cache in ("text", "sqlite"):
            with self.subTest(cache=cache):
                self.config(cache=cache, threshold=3)
                with self.server() as process:
                    for expected in (1, 2, 3):
                        before = len(self.calls)
                        replies = self.batch()
                        self.assertTrue(all(r[0] == 200 and r[1]["translatedText"] == "번역 결과" for r in replies))
                        self.assertEqual(len(self.calls), before + 1)
                        self.assertEqual(self.rows(process, cache), [("same", expected, "번역 결과")])
                    before = len(self.calls)
                    self.assertEqual(self.http()[0], 200)
                    self.assertEqual(len(self.calls), before)
                self.assertIn("running=0, waiters=0, live=0", self.log())

    def test_changed_result_resets_threshold(self):
        self.config(threshold=3)
        with self.server() as process:
            self.batch(count=4)
            type(self).reply = "새 번역"
            replies = self.batch(count=4)
            self.assertTrue(all(r[1]["translatedText"] == "새 번역" for r in replies))
            self.assertEqual(self.rows(process, "sqlite"), [("same", 1, "새 번역")])
            self.assertEqual(len(self.calls), 2)

    def test_normalized_aliases_and_distinct_keys(self):
        self.config(threshold=3)
        with self.server():
            for pairs in (({"source": "auto"}, {"source": "AUTO"}),
                          ({"target": "zh-Hant"}, {"target": "zht"})):
                joined = self.log().count(" joined (waiters=")
                previous = len(self.calls)
                self.gate.clear()
                with ThreadPoolExecutor(max_workers=2) as pool:
                    futures = [pool.submit(self.http, **args) for args in pairs]
                    try:
                        self.wait_for(lambda: len(self.calls) == previous + 1)
                        self.wait_for(lambda: self.log().count(" joined (waiters=") > joined)
                    finally:
                        self.gate.set()
                    self.assertEqual([f.result()[0] for f in futures], [200, 200])
            previous = len(self.calls)
            self.gate.clear()
            with ThreadPoolExecutor(max_workers=3) as pool:
                futures = [pool.submit(self.http, **args) for args in
                    ({"source": "eng"}, {"text": "different"}, {"target": "eng"})]
                try:
                    self.wait_for(lambda: len(self.calls) == previous + 3)  # no global translation lock
                finally:
                    self.gate.set()
                self.assertEqual([f.result()[0] for f in futures], [200] * 3)

    def test_failure_shared_retry_sequence_and_recovery(self):
        self.config()
        with self.server():
            type(self).upstream_status = 401
            replies = self.batch(count=6)
            self.assertTrue(all(r[0] == 502 and r[2] is None for r in replies))
            self.assertEqual(len(self.calls), 1)
            type(self).upstream_status = 503
            replies = self.batch(count=6)
            self.assertTrue(all(r[0] == 503 and r[2] == "5" for r in replies))
            self.assertEqual(len(self.calls), 4)  # one shared sequence of three attempts
            with database(self.cache) as db:
                self.assertEqual(db.execute("SELECT count(*) FROM trans_cache").fetchone(), (0,))
            type(self).upstream_status = 200
            self.assertTrue(all(r[0] == 200 for r in self.batch(count=6)))
            self.assertEqual(len(self.calls), 5)

    def test_capacity_and_cache_hit_bypass(self):
        self.config()
        self.configure(INFLIGHT_MAX_KEYS=2, INFLIGHT_MAX_WAITERS=2, INFLIGHT_MAX_WAITERS_PER_KEY=1)
        with self.server():
            self.assertEqual(self.http(text="cached")[0], 200)
            self.gate.clear()
            with ThreadPoolExecutor(max_workers=4) as pool:
                futures = [pool.submit(self.http, text=key) for key in ("a", "b")]
                try:
                    self.wait_for(lambda: len(self.calls) == 3)
                    futures += [pool.submit(self.http, text=key) for key in ("a", "b")]
                    self.wait_for(lambda: self.log().count(" joined (waiters=") == 2)
                    self.assertEqual(self.http(text="a")[0::2], (503, "5"))
                    self.assertEqual(self.http(text="new")[0::2], (503, "5"))
                    self.assertEqual(self.http(text="cached")[0], 200)
                    self.assertEqual(len(self.calls), 3)
                finally:
                    self.gate.set()
                self.assertTrue(all(f.result()[0] == 200 for f in futures))
            self.assertEqual(self.http(text="new")[0], 200)

    def test_missing_and_empty_upstream_content_is_not_cached(self):
        self.config()
        with self.server():
            for payload in ({}, {"choices": [{"message": {"content": "   "}}]}):
                type(self).payload_override = payload
                previous = len(self.calls)
                replies = self.batch(count=4)
                self.assertTrue(all(r[0] == 502 and r[2] is None for r in replies))
                self.assertEqual(len(self.calls), previous + 1)
                with database(self.cache) as db:
                    self.assertEqual(db.execute("SELECT count(*) FROM trans_cache").fetchone(), (0,))

    def test_total_deadline_and_recovery(self):
        self.config()
        self.configure(TRANSLATION_TOTAL_TIMEOUT=2)
        self.gate.clear()
        with self.server():
            start = time.monotonic()
            with ThreadPoolExecutor(max_workers=6) as pool:
                futures = [pool.submit(self.http) for _ in range(6)]
                self.wait_for(lambda: self.log().count(" joined (waiters=") == 5)
                replies = [f.result(timeout=5) for f in futures]
            self.assertTrue(all(r[0] == 503 for r in replies))
            self.assertLess(time.monotonic() - start, 4)
            self.assertEqual(len(self.calls), 1)
            self.gate.set()
            self.wait_for(lambda: "flushed" in self.log())
            self.assertEqual(self.http()[0], 200)
            self.assertEqual(len(self.calls), 2)

    def test_disconnected_owner_keeps_followers(self):
        self.config()
        self.gate.clear()
        with self.server():
            body = json.dumps({"uuid": str(uuid.uuid4()), "timestamp": TIMESTAMP,
                               "from": "auto", "to": "kor", "text": "same"}).encode()
            with socket.create_connection(("127.0.0.1", self.port)) as sock:
                sock.sendall((f"POST /translate HTTP/1.1\r\nHost: localhost\r\nContent-Length: {len(body)}\r\n"
                              "Content-Type: application/json\r\n\r\n").encode() + body)
                self.wait_for(lambda: len(self.calls) == 1)
            with ThreadPoolExecutor(max_workers=1) as pool:
                future = pool.submit(self.http)
                try:
                    self.wait_for(lambda: "joined" in self.log())
                finally:
                    self.gate.set()
                self.assertEqual(future.result()[0], 200)
            self.assertEqual(len(self.calls), 1)

    def test_cache_unavailable_and_write_failure(self):
        for unavailable in (False, True):
            with self.subTest(unavailable=unavailable):
                self.config()
                if unavailable:
                    self.configure(TRANS_CACHE_SQLITE_PATH=self.directory / "absent" / "cache.db")
                with self.server():
                    if not unavailable:
                        with database(self.cache) as db:
                            db.execute("CREATE TRIGGER fail_write BEFORE INSERT ON trans_cache BEGIN SELECT RAISE(ABORT,'fixture'); END")
                    previous = len(self.calls)
                    for _ in range(2):
                        self.assertTrue(all(r[0] == 200 for r in self.batch(count=4)))
                    self.assertEqual(len(self.calls), previous + 2)

    @contextmanager
    def codex_gate(self, stage="reply"):
        gate = self.directory / "release"
        gate.unlink(missing_ok=True)
        pids = self.directory / "pids"
        pids.unlink(missing_ok=True)
        with patch.dict(os.environ, {"TB_INFLIGHT_GATE": str(gate), "TB_INFLIGHT_PIDS": str(pids),
                                     "TB_INFLIGHT_STAGE": stage}):
            try:
                yield gate, pids
            finally:
                gate.touch()

    def assert_reaped(self, path):
        for pid in map(int, path.read_text().splitlines()):
            with self.assertRaises(ProcessLookupError):
                os.kill(pid, 0)

    def test_codex_exec_and_mcp_coalescing(self):
        for backend in ("exec", "mcp"):
            with self.subTest(backend=backend):
                self.config(backend=backend, threshold=3)
                with self.codex_gate() as (gate, pids), self.server():
                    before = self.log().count(" joined (waiters=")
                    with ThreadPoolExecutor(max_workers=6) as pool:
                        futures = [pool.submit(self.http) for _ in range(6)]
                        try:
                            self.wait_for(lambda: pids.exists())
                            self.wait_for(lambda: self.log().count(" joined (waiters=") >= before + 5)
                        finally:
                            gate.touch()
                        self.assertTrue(all(f.result()[0] == 200 for f in futures))
                    self.assertEqual(len(pids.read_text().splitlines()), 1)
                    self.assert_reaped(pids)

    def test_codex_deadlines_cover_call_initialize_and_stdin(self):
        for backend, stage in (("exec", "reply"), ("mcp", "initialize"), ("exec", "stdin"), ("mcp", "stdin")):
            with self.subTest(backend=backend, stage=stage):
                self.config(backend=backend)
                self.configure(TRANSLATION_TOTAL_TIMEOUT=2)
                with self.codex_gate(stage) as (gate, pids), self.server():
                    before = self.log().count(" joined (waiters=")
                    with ThreadPoolExecutor(max_workers=3) as pool:
                        futures = [pool.submit(self.http, text="x" * 10000) for _ in range(3)]
                        self.wait_for(lambda: pids.exists())
                        self.wait_for(lambda: self.log().count(" joined (waiters=") >= before + 2)
                        self.assertTrue(all(f.result(timeout=5)[0] == 504 for f in futures))
                    self.wait_for(lambda: "flushed" in self.log())
                    self.assertEqual(len(pids.read_text().splitlines()), 1)
                    self.wait_for(lambda: not any(Path(f"/proc/{pid}").exists() for pid in pids.read_text().splitlines()))
                    self.assert_reaped(pids)

    def test_codex_failures_are_shared(self):
        for backend in ("exec", "mcp"):
            with self.subTest(backend=backend):
                self.config(backend=backend)
                with patch.dict(os.environ, {"TB_INFLIGHT_ERROR": "1"}), self.codex_gate() as (gate, pids), self.server():
                    before = self.log().count(" joined (waiters=")
                    with ThreadPoolExecutor(max_workers=4) as pool:
                        futures = [pool.submit(self.http) for _ in range(4)]
                        try:
                            self.wait_for(lambda: pids.exists())
                            self.wait_for(lambda: self.log().count(" joined (waiters=") >= before + 3)
                        finally:
                            gate.touch()
                        replies = [f.result() for f in futures]
                        self.assertTrue(all(r[0] == 502 and r[2] is None for r in replies))
                    self.assertEqual(len(pids.read_text().splitlines()), 1)
                    self.assert_reaped(pids)

    def test_shutdown_wakes_waiters_and_stops_backends(self):
        for backend in ("normal", "exec", "mcp"):
            with self.subTest(backend=backend):
                self.config(backend=backend)
                self.gate.clear()
                with self.codex_gate() as (gate, pids), self.server() as process:
                    before = self.log().count(" joined (waiters=")
                    with ThreadPoolExecutor(max_workers=4) as pool:
                        futures = [pool.submit(self.http, allow_disconnect=True) for _ in range(4)]
                        try:
                            self.wait_for(lambda: len(self.calls) >= 1 if backend == "normal" else pids.exists())
                            self.wait_for(lambda: self.log().count(" joined (waiters=") >= before + 3)
                            process.send_signal(signal.SIGHUP)
                            start = time.monotonic()
                            process.terminate()
                            process.wait(timeout=7)
                            self.assertLess(time.monotonic() - start, 7)
                            self.assertEqual(process.returncode, 0)
                            for future in futures:
                                self.assertIn(future.result()[0], (0, 503))
                        finally:
                            self.gate.set()
                            gate.touch()
                    self.assertIn("running=0, waiters=0, live=0", self.log())
                    if backend != "normal":
                        self.assert_reaped(pids)

    def test_invalid_limits_fail_startup(self):
        for key, value in (("TRANSLATION_TOTAL_TIMEOUT", "0"), ("TRANSLATION_TOTAL_TIMEOUT", "111"),
                           ("INFLIGHT_MAX_KEYS", "0"), ("INFLIGHT_MAX_WAITERS", "-1"),
                           ("INFLIGHT_MAX_WAITERS_PER_KEY", "2junk"), ("INFLIGHT_MAX_KEYS", "99999999999999999999999")):
            with self.subTest(key=key, value=value):
                self.config()
                self.configure(**{key: value})
                result = subprocess.run([str(ROOT / "transbasket"), "-c", str(self.config_path),
                    "-r", "role.txt"], capture_output=True, text=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("INFLIGHT limits", result.stdout)


if __name__ == "__main__":
    # Reuse the existing server fixture without rerunning its inherited tests.
    names = sys.argv[1:] or [name for name in CoalescingTests.__dict__ if name.startswith("test_")]
    suite = unittest.TestSuite(CoalescingTests(name) for name in names)
    outcome = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(not outcome.wasSuccessful())
