"""cache_tool source_hint commands (PLAN_SOURCE_HINT_METADATA.md section 8).

    python3 tests/test_cache_tool_hints.py ./cache_tool obj/cache_tool_hint_v2

The second binary is the same tool built with SOURCE_HINT_VERSION=2, used only
to exercise backfill's "upgrade an older script hint" path, which v1 cannot
reach. Expected script hints come from tests/source_hint_ref.py.
"""
import hashlib
import json
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import source_hint_ref as R  # noqa: E402

TOOL, TOOL_V2 = sys.argv[1], sys.argv[2]
ROWS = [  # (from, to, text, translation, hint) - hint = (lang, mixed, origin, version) or None
    ("auto", "eng", "안녕하세요 반갑습니다", "Hello", None),
    ("auto", "eng", "明日の会議は午後三時です", "Tomorrow", ("jpn", 0, "model", 1)),
    ("auto", "eng", "Le serveur redémarre", "The server restarts", ("xyz", 0, "model", 1)),   # invalid code
    ("kor", "eng", "감사합니다", "Thanks", None),
]


def key(f, t, text):
    return hashlib.sha256(f"{f}|{t}|{text}".encode()).hexdigest()


def script_hint(text):
    lang, mixed = R.classify(text.encode())
    return (lang, int(mixed), "script")


class CacheToolHints(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="cache-tool-hints-")
        self.addCleanup(self.temp.cleanup)
        self.dir = Path(self.temp.name)

    def tool(self, *args, binary=None):
        return subprocess.run([binary or TOOL, *args], capture_output=True, text=True)

    def make_sqlite(self, name, rows=ROWS):
        path = self.dir / name
        # The NEW tool creates the schema (dry run on an empty database writes nothing else).
        self.assertEqual(self.tool("backfill-hints", "--backend", "sqlite", "--config", str(path), "--dry-run").returncode, 0)
        db = sqlite3.connect(path)
        for f, t, text, out, hint in rows:
            db.execute("INSERT INTO trans_cache(hash,from_lang,to_lang,source_text,translated_text,count,last_used,created_at)"
                       " VALUES (?,?,?,?,?,1,strftime('%s','now'),strftime('%s','now'))", (key(f, t, text), f, t, text, out))
            if hint:
                db.execute("INSERT INTO trans_cache_source_hint VALUES (?,?,?,?,?,1)", (key(f, t, text), *hint))
        db.commit()
        db.close()
        return path

    def make_text(self, name, rows=ROWS):
        path = self.dir / name
        with path.open("w", encoding="utf-8") as fp:
            for i, (f, t, text, out, hint) in enumerate(rows, 1):
                row = {"id": i, "hash": key(f, t, text), "from": f, "to": t, "source": text, "target": out,
                       "count": 1, "last_used": 1700000000, "created_at": 1700000000}
                if hint:
                    row.update(source_hint=hint[0], hint_mixed=bool(hint[1]), hint_origin=hint[2], hint_version=hint[3])
                fp.write(json.dumps(row, ensure_ascii=False) + "\n")
        return path

    def sqlite_hints(self, path):
        db = sqlite3.connect(path)
        rows = db.execute("SELECT c.source_text, h.source_hint, h.mixed, h.origin, h.hint_version FROM trans_cache c "
                          "LEFT JOIN trans_cache_source_hint h ON h.hash = c.hash").fetchall()
        db.close()
        return {r[0]: (r[1:4] if r[1] else None, r[4]) for r in rows}

    def text_hints(self, path):
        rows = [json.loads(l) for l in path.read_text(encoding="utf-8").splitlines()]
        return {r["source"]: ((r["source_hint"], int(r["hint_mixed"]), r["hint_origin"]), r["hint_version"])
                if "source_hint" in r else (None, None) for r in rows}

    def check_after_backfill(self, hints, version=1):
        self.assertEqual(hints["안녕하세요 반갑습니다"], (script_hint("안녕하세요 반갑습니다"), version))
        self.assertEqual(hints["明日の会議は午後三時です"], (("jpn", 0, "model"), 1), "model hint untouched")
        self.assertEqual(hints["Le serveur redémarre"], (script_hint("Le serveur redémarre"), version), "invalid replaced")
        self.assertEqual(hints["감사합니다"][0], None, "explicit row gets no hint")

    def test_backfill_sqlite(self):
        path = self.make_sqlite("c.db")
        before = self.sqlite_hints(path)
        dry = self.tool("backfill-hints", "--backend", "sqlite", "--config", str(path), "--dry-run")
        self.assertIn("would fill (no or invalid hint): 2", dry.stdout)
        self.assertIn("kept (model hint): 1", dry.stdout)
        self.assertEqual(self.sqlite_hints(path), before, "dry run writes nothing")
        real = self.tool("backfill-hints", "--backend", "sqlite", "--config", str(path))
        self.assertEqual(real.returncode, 0, real.stdout + real.stderr)
        self.check_after_backfill(self.sqlite_hints(path))
        again = self.tool("backfill-hints", "--backend", "sqlite", "--config", str(path))
        self.assertIn("kept (current script hint): 2", again.stdout, "idempotent")

    def test_backfill_text(self):
        path = self.make_text("c.jsonl")
        real = self.tool("backfill-hints", "--backend", "text", "--config", str(path))
        self.assertEqual(real.returncode, 0, real.stdout + real.stderr)
        self.check_after_backfill(self.text_hints(path))

    def test_backfill_upgrades_older_script_hints(self):
        path = self.make_sqlite("u.db")
        self.tool("backfill-hints", "--backend", "sqlite", "--config", str(path))          # v1 script hints
        up = self.tool("backfill-hints", "--backend", "sqlite", "--config", str(path), binary=TOOL_V2)
        self.assertIn("upgraded (older script hint): 2", up.stdout, up.stdout + up.stderr)
        self.check_after_backfill(self.sqlite_hints(path), version=2)

    def test_migrate_round_trip_keeps_hints(self):
        src = self.make_sqlite("src.db")
        self.tool("backfill-hints", "--backend", "sqlite", "--config", str(src))
        mid, back = self.dir / "mid.jsonl", self.dir / "back.db"
        r1 = self.tool("migrate", "--from", "sqlite", "--from-config", str(src), "--to", "text", "--to-config", str(mid), "--no-progress")
        r2 = self.tool("migrate", "--from", "text", "--from-config", str(mid), "--to", "sqlite", "--to-config", str(back), "--no-progress")
        self.assertEqual((r1.returncode, r2.returncode), (0, 0), r1.stdout + r2.stdout)
        self.assertIn("Hints written: 3", r1.stdout)
        self.assertEqual(self.sqlite_hints(back), self.sqlite_hints(src))
        self.assertEqual({k: v[0] for k, v in self.text_hints(mid).items()}, {k: v[0] for k, v in self.sqlite_hints(src).items()})

    def test_migrate_into_populated_destination(self):
        # destination already has the Korean row with ANOTHER translation and a script hint;
        # the source has the same key with a model hint -> destination translation kept, hint upgraded.
        dest = self.make_sqlite("dest.db", [("auto", "eng", "안녕하세요 반갑습니다", "DEST", ("kor", 0, "script", 1))])
        src = self.make_text("src.jsonl", [("auto", "eng", "안녕하세요 반갑습니다", "SRC", ("kor", 1, "model", 1)),
                                           ("auto", "eng", "새 항목", "new", ("kor", 0, "model", 1))])
        r = self.tool("migrate", "--from", "text", "--from-config", str(src), "--to", "sqlite", "--to-config", str(dest), "--no-progress")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("Total migrated: 1 entries", r.stdout)
        self.assertIn("Already in destination (translation kept): 1 entries", r.stdout)
        self.assertIn("Hints written: 2", r.stdout)
        db = sqlite3.connect(dest)
        self.assertEqual(db.execute("SELECT translated_text FROM trans_cache WHERE source_text='안녕하세요 반갑습니다'").fetchone(), ("DEST",))
        db.close()
        self.assertEqual(self.sqlite_hints(dest)["안녕하세요 반갑습니다"], (("kor", 1, "model"), 1))
        # and the reverse priority: a script hint never replaces a destination model hint
        src2 = self.make_text("src2.jsonl", [("auto", "eng", "안녕하세요 반갑습니다", "SRC", ("jpn", 0, "script", 1))])
        r = self.tool("migrate", "--from", "text", "--from-config", str(src2), "--to", "sqlite", "--to-config", str(dest), "--no-progress")
        self.assertIn("Hints written: 0", r.stdout)
        self.assertEqual(self.sqlite_hints(dest)["안녕하세요 반갑습니다"], (("kor", 1, "model"), 1))

    def test_list_and_stats_show_hints(self):
        path = self.make_text("l.jsonl")
        self.tool("backfill-hints", "--backend", "text", "--config", str(path))
        listing = self.tool("-f", str(path), "list").stdout
        lang = script_hint("안녕하세요 반갑습니다")[0]
        self.assertIn(f"{lang}/s", listing)
        self.assertIn("jpn/m", listing)
        stats = self.tool("-f", str(path), "stats").stdout
        self.assertIn("source_hint of from=auto rows (3)", stats)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
