"""Cleanup-days overflow regression tests (PLAN_CLEANUP_DAYS_AND_NUL.md section 5 V5).
Every expected value is computed here in python; the C side only reports.

    python3 tests/test_cleanup_days.py <harness> <cache_tool> <transbasket>
"""
import hashlib
import json
import random
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HARNESS, CACHE_TOOL, SERVER = (str(Path(p).resolve()) for p in sys.argv[1:4])
INT_MAX, INT_MIN = 2**31 - 1, -2**31
I64_MAX, I64_MIN = 2**63 - 1, -2**63
DAY = 86400

# Day counts around every boundary found in section 1.1, plus the extremes.
DAYS = [1, 30, 365, 20762, 20763, 24855, 24856, 36500, 49710, 49711, 60000, 74566, 99999, INT_MAX, 0, -1]


def cutoff(now, days):
    return max(I64_MIN, min(I64_MAX, now - days * DAY))


def interval(days):
    return max(3600, min(INT_MAX - 3600, (days * DAY) // 10 if days >= 0 else -((-days * DAY) // 10)))


def ages():
    """last_used values: half-day offsets so that seconds of clock drift between
    python and C never move a row across the cutoff, plus far past/future rows."""
    now = int(time.time())
    rows = [now - int((k + 0.5) * DAY) for k in (0, 29, 100, 400, 20761, 24855, 36499, 60000)]
    return rows + [-10**12, -10**15, 10**12]


def atoi_glibc(text):
    """glibc atoi = (int)strtol(s, NULL, 10); here saturated, which is the new contract."""
    i = 0
    while i < len(text) and text[i] in " \t\n\v\f\r":
        i += 1
    j = i + (1 if i < len(text) and text[i] in "+-" else 0)
    k = j
    while k < len(text) and text[k].isdigit() and text[k].isascii():
        k += 1
    if k == j:
        return 0, False
    v = int(text[i:k])
    plain = k == len(text) and INT_MIN <= v <= INT_MAX
    return max(INT_MIN, min(INT_MAX, v)), plain


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Arithmetic(unittest.TestCase):
    def test_cutoff_and_interval(self):
        rnd = random.Random(20261106)
        nows = [0, 1, -1, int(time.time()), I64_MAX, I64_MIN, I64_MAX - 86399, I64_MIN + 86399]
        nows += [rnd.randrange(I64_MIN, I64_MAX) for _ in range(300)]
        days = DAYS + [INT_MIN, INT_MIN + 1, INT_MAX - 1] + [rnd.randrange(INT_MIN, INT_MAX + 1) for _ in range(300)]
        lines, expected = [], []
        for n in nows:
            for d in rnd.sample(days, 40) + [INT_MAX, INT_MIN, 1, -1]:
                lines.append(f"cutoff {n} {d}")
                expected.append(str(cutoff(n, d)))
        for d in days + list(range(-50, 50)) + [416, 417, 24855 * 10, 248550, 248551, 248552]:
            lines.append(f"interval {d}")
            expected.append(str(interval(d)))
        out = subprocess.run([HARNESS, "arith"], input="\n".join(lines) + "\n", capture_output=True, text=True, check=True)
        got = out.stdout.split()
        self.assertEqual(len(got), len(expected))
        bad = [(l, g, e) for l, g, e in zip(lines, got, expected) if g != e]
        self.assertEqual(bad[:5], [], f"{len(bad)} of {len(lines)} mismatched")
        print(f"  arithmetic: {len(lines)} cases, 0 mismatches")


class Backends(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="cleanup-days-")
        self.addCleanup(self.temp.cleanup)
        self.d = Path(self.temp.name)

    def write_text_cache(self, path, last_used):
        with open(path, "w") as f:
            for i, t in enumerate(last_used, 1):
                src = f"s{i}"
                f.write(json.dumps({"id": i, "hash": hashlib.sha256(f"kor|eng|{src}".encode()).hexdigest(),
                                    "from": "kor", "to": "eng", "source": src, "target": "t", "count": 1,
                                    "last_used": t, "created_at": t}) + "\n")

    def remaining(self, backend, path):
        if backend == "text":
            with open(path) as f:
                return sorted(json.loads(line)["source"] for line in f)
        db = sqlite3.connect(path)
        try:
            return sorted(r[0] for r in db.execute("SELECT source_text FROM trans_cache"))
        finally:
            db.close()

    def test_cleanup_and_stats(self):
        checked = 0
        for backend in ("text", "sqlite"):
            for days in DAYS:
                lu = ages()
                path = self.d / f"{backend}_{days}.{'jsonl' if backend == 'text' else 'db'}"
                if backend == "text":
                    self.write_text_cache(path, lu)
                else:
                    subprocess.run([HARNESS, "prepare", "sqlite", str(path), str(len(lu))], check=True, capture_output=True)
                    db = sqlite3.connect(path)
                    with db:
                        for i, t in enumerate(lu, 1):
                            db.execute("UPDATE trans_cache SET last_used=? WHERE source_text=?", (t, f"s{i}"))
                    db.close()
                now = int(time.time())
                c = cutoff(now, days)
                expired = [f"s{i}" for i, t in enumerate(lu, 1) if t < c]
                removed = expired if days > 0 else []          # cleanup ignores days <= 0
                out = subprocess.run([HARNESS, "run", backend, str(path), str(days)], capture_output=True, text=True)
                self.assertEqual(out.returncode, 0, out.stderr)
                line = out.stdout.strip().splitlines()[-1]
                self.assertEqual(line, f"expired={len(expired)} removed={len(removed)}", f"{backend} days={days}")
                keep = sorted(f"s{i}" for i in range(1, len(lu) + 1) if f"s{i}" not in removed)
                self.assertEqual(self.remaining(backend, path), keep, f"{backend} days={days}")
                checked += 1
        print(f"  backends: {checked} cleanup+stats runs, all as computed")


class CacheToolArgs(unittest.TestCase):
    def run_tool(self, *args):
        with tempfile.TemporaryDirectory(prefix="cleanup-days-tool-") as d:
            p = Path(d) / "c.jsonl"
            Backends.write_text_cache(None, p, ages()[:4])
            before = p.read_bytes()
            r = subprocess.run([CACHE_TOOL, "-f", str(p), *args], capture_output=True, text=True)
            with open(p) as f:
                ids = [json.loads(line)["id"] for line in f]
            return r.returncode, ids, before == p.read_bytes(), r.stderr

    def test_cleanup_arguments(self):
        # ages()[:4] are 0.5, 29.5, 100.5, 400.5 days old
        for arg, want in [("30", [1, 2]), ("365", [1, 2, 3]), ("1", [1]), ("24856", [1, 2, 3, 4]),
                          ("2147483647", [1, 2, 3, 4])]:
            rc, ids, _, err = self.run_tool("cleanup", arg)
            self.assertEqual((rc, ids), (0, want), f"cleanup {arg}: {err}")
        for arg in ["99999999999", "2147483648", "30days", "+30", " 30", "30 ", "", "0", "-5", "abc", "0x1e"]:
            rc, ids, unchanged, err = self.run_tool("cleanup", arg)
            self.assertEqual((rc != 0, ids, unchanged), (True, [1, 2, 3, 4], True), f"cleanup {arg!r}")
            self.assertIn("must be a decimal integer", err)

    def test_delete_arguments(self):
        rc, ids, _, err = self.run_tool("delete", "2")
        self.assertEqual((rc, ids), (0, [1, 3, 4]), err)
        for arg in ["4294967297", "1abc", "2147483648", "0", "-1", "+1", " 1", ""]:
            rc, ids, unchanged, err = self.run_tool("delete", arg)
            self.assertEqual((rc != 0, ids, unchanged), (True, [1, 2, 3, 4], True), f"delete {arg!r}")
            self.assertIn("must be a decimal integer", err)


class ConfigValue(unittest.TestCase):
    def effective(self, raw):
        with tempfile.TemporaryDirectory(prefix="cleanup-days-conf-") as d:
            d = Path(d)
            port = free_port()
            (d / "role.txt").write_text("role", encoding="utf-8")
            conf = d / "t.conf"
            conf.write_text(
                f'OPENAI_BASE_URL="http://127.0.0.1:9/v1"\nOPENAI_MODEL="m"\nOPENAI_API_KEY="k"\nLISTEN="127.0.0.1"\n'
                f'PORT="{port}"\nPROMPT_PREFIX_FILE="{ROOT}/PROMPT_PREFIX.txt"\nLANGUAGE_TABLE_FILE="{ROOT}/language_tables.conf"\n'
                f'TRANS_CACHE_TYPE="text"\nTRANS_CACHE_FILE="{d}/c.jsonl"\nTRANS_CACHE_CLEANUP_ENABLED="true"\n'
                f'TRANS_CACHE_CLEANUP_DAYS="{raw}"\n', encoding="utf-8")
            log = d / "log"
            with open(log, "w") as fp:
                p = subprocess.Popen([SERVER, "-c", str(conf), "-r", "role.txt"], cwd=d, stdout=fp, stderr=subprocess.STDOUT)
                try:
                    for _ in range(200):
                        if "Cache background thread started" in log.read_text(errors="replace"):
                            break
                        if p.poll() is not None:
                            break
                        time.sleep(0.02)
                finally:
                    p.terminate()
                    p.wait(timeout=10)
            text = log.read_text(errors="replace")
            line = [l for l in text.splitlines() if "Cache background thread started" in l]
            self.assertTrue(line, text[-2000:])
            return line[0], "Warning: TRANS_CACHE_CLEANUP_DAYS" in text

    def test_values(self):
        vt, ff = chr(11), chr(12)
        for raw in ["60", "120days", "99999999999", "2147483647", "2147483648", "-5", "0", "abc", " 30",
                    vt + "30", ff + "30", "030", "0x30", "+45", "-2147483649", "9" * 30, "24856"]:
            days, plain = atoi_glibc(raw)
            if days <= 0:
                days = 60
            line, warned = self.effective(raw)
            self.assertIn(f"cleanup check every {interval(days)} seconds, cleanup after {days} days", line, repr(raw))
            self.assertEqual(warned, not plain, repr(raw))
        print("  config: 17 values, effective days, interval and warning as computed")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], "-v"])
