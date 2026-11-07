"""PLAN_CODE_ISSUES_4.md 1: TRANS_CACHE_SQLITE_JOURNAL_MODE / _SYNC are applied.

    python3 -I tests/test_sqlite_pragmas.py <harness> <transbasket> [<pre-fix transbasket>]

A. The backend on its own connection (tests/test_sqlite_pragmas.c): every journal x
   synchronous value (mixed case) is in effect, NULL options mean WAL / NORMAL, a
   journal mode SQLite refuses is a warning, only WAL persists in the file, risky
   modes warn, and names the loader would refuse are refused by the backend too.
B. Through the server: the configured values reach the backend (startup line),
   invalid values keep today's loader warning and WAL / NORMAL, the last line wins,
   the text cache ignores the keys, and an SQL fragment does nothing. With a pre-fix
   binary the configured DELETE is ignored (the file stays in WAL).
Servers bind 127.0.0.1; no network.
"""
import os
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FAILURES = []
JOURNAL = ["DELETE", "TRUNCATE", "PERSIST", "MEMORY", "WAL", "OFF"]
SYNC = ["OFF", "NORMAL", "FULL", "EXTRA"]


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        FAILURES.append(what)


def harness(h, db, jm, sy):
    p = subprocess.run([h, str(db), jm, sy], capture_output=True, text=True, timeout=30)
    assert p.returncode == 0 and not p.stderr, (p.returncode, p.stderr[-500:])   # e.g. a sanitizer report
    out = p.stdout
    last = out.strip().splitlines()[-1]
    return last, out


def seed_row(db):
    """One valid cache row, so a test can show that it survives."""
    c = sqlite3.connect(str(db))
    c.execute("INSERT INTO trans_cache (hash, from_lang, to_lang, source_text, translated_text, count, last_used, created_at) "
              "VALUES (?, 'kor', 'eng', 'seed', 'seed', 1, 1, 1)", ("a" * 64,))
    c.commit()
    c.close()


def rows(db):
    c = sqlite3.connect(str(db))
    try:
        return c.execute("SELECT count(*) FROM trans_cache").fetchone()[0]
    finally:
        c.close()


def file_journal(db):
    c = sqlite3.connect(str(db))
    try:
        return c.execute("PRAGMA journal_mode").fetchone()[0]
    finally:
        c.close()


def part_a(h, d):
    mixed = lambda s, i: s.lower() if i % 2 else s.capitalize()
    n = 0
    for i, jm in enumerate(JOURNAL):
        for k, sy in enumerate(SYNC):
            db = d / f"a_{jm}_{sy}.db"
            last, out = harness(h, db, mixed(jm, i), mixed(sy, k))
            want = f"journal_mode={jm.lower()} synchronous={k}"
            risky = jm in ("MEMORY", "OFF") or sy == "OFF"
            ok = last == want and ("can corrupt the cache" in out) == risky and "Warning: SQLite journal_mode is" not in out
            ok = ok and f"SQLite journal_mode={jm.lower()}, synchronous={sy}" in out
            reopened = file_journal(db)
            kept = (jm == "WAL") == (reopened == "wal")
            if not ok:
                print("      ", jm, sy, repr(out[-300:]))
            if not kept:
                print("       persistence", jm, reopened)
            n += ok and kept
    check(n == len(JOURNAL) * len(SYNC), f"{n}/{len(JOURNAL) * len(SYNC)} journal x synchronous combinations in effect on the backend's connection, "
                                         f"risk warning exactly for MEMORY/OFF/synchronous OFF, only WAL left in the file")
    last, out = harness(h, d / "a_null.db", "-", "-")
    check(last == "journal_mode=wal synchronous=1" and "Warning" not in out, f"NULL options: WAL / NORMAL, no warning ({last})")
    last, out = harness(h, ":memory:", "WAL", "NORMAL")
    check(last == "journal_mode=memory synchronous=1" and "Warning: SQLite journal_mode is memory, not the configured WAL" in out,
          f"refused mode (in-memory DB asked for WAL) is a warning, cache still opens ({last})")
    db = d / "a_inj.db"
    harness(h, db, "-", "-")
    seed_row(db)
    last, out = harness(h, db, "WAL; DELETE FROM trans_cache", "NORMAL; DROP TABLE trans_cache")
    check(last == "journal_mode=wal synchronous=1" and out.count("Warning: unsupported SQLite") == 2 and rows(db) == 1,
          "names outside the list are refused by the backend too (SQL fragment: warning, WAL / NORMAL, existing row kept)")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def server_log(binary, d, lines):
    port = free_port()
    (d / "role.txt").write_text("role")
    base = [("OPENAI_BASE_URL", "http://127.0.0.1:9/v1"), ("OPENAI_MODEL", "m"), ("OPENAI_API_KEY", "k"),
            ("LISTEN", "127.0.0.1"), ("PORT", str(port)), ("PROMPT_PREFIX_FILE", str(ROOT / "PROMPT_PREFIX.txt")),
            ("TRANS_CACHE_SQLITE_PATH", str(d / "c.db")), ("TRANS_CACHE_FILE", str(d / "c.jsonl"))]
    (d / "t.conf").write_text("".join(f'{k}="{v}"\n' for k, v in base + lines))
    with open(d / "log", "wb") as log:
        p = subprocess.Popen([binary, "-c", str(d / "t.conf"), "-r", "role.txt"], cwd=d, stdout=log, stderr=subprocess.STDOUT)
        for _ in range(200):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                break
            except OSError:
                if p.poll() is not None:
                    break
                time.sleep(0.05)
        p.terminate()
        p.wait(timeout=20)
    return (d / "log").read_text(errors="replace")


def part_b(binary, old, work):
    S = [("TRANS_CACHE_TYPE", "sqlite")]
    scenarios = [
        ("DELETE / FULL", S + [("TRANS_CACHE_SQLITE_JOURNAL_MODE", "DELETE"), ("TRANS_CACHE_SQLITE_SYNC", "FULL")],
         ["SQLite journal_mode=delete, synchronous=FULL"], [], "delete"),
        ("lower case truncate / extra", S + [("TRANS_CACHE_SQLITE_JOURNAL_MODE", "truncate"), ("TRANS_CACHE_SQLITE_SYNC", "extra")],
         ["SQLite journal_mode=truncate, synchronous=EXTRA"], [], "delete"),
        ("defaults (keys absent)", S, ["SQLite journal_mode=wal, synchronous=NORMAL"], ["Warning: SQLite", "can corrupt"], "wal"),
        ("invalid values: loader warning, WAL / NORMAL", S + [("TRANS_CACHE_SQLITE_JOURNAL_MODE", "fast"), ("TRANS_CACHE_SQLITE_SYNC", "2")],
         ["Warning: Invalid TRANS_CACHE_SQLITE_JOURNAL_MODE 'fast', using 'WAL'", "Warning: Invalid TRANS_CACHE_SQLITE_SYNC '2', using 'NORMAL'",
          "SQLite journal_mode=wal, synchronous=NORMAL"], [], "wal"),
        ("SQL fragment refused (existing row kept)", S + [("TRANS_CACHE_SQLITE_JOURNAL_MODE", "WAL; DELETE FROM trans_cache")],
         ["Warning: Invalid TRANS_CACHE_SQLITE_JOURNAL_MODE", "SQLite journal_mode=wal, synchronous=NORMAL"], [], "wal"),
        ("last line wins", S + [("TRANS_CACHE_SQLITE_JOURNAL_MODE", "DELETE"), ("TRANS_CACHE_SQLITE_JOURNAL_MODE", "PERSIST")],
         ["SQLite journal_mode=persist, synchronous=NORMAL"], [], "delete"),
        ("MEMORY warns", S + [("TRANS_CACHE_SQLITE_JOURNAL_MODE", "MEMORY")],
         ["SQLite journal_mode=memory, synchronous=NORMAL", "can corrupt the cache on a crash"], [], "delete"),
        ("text cache ignores the keys", [("TRANS_CACHE_TYPE", "text"), ("TRANS_CACHE_SQLITE_JOURNAL_MODE", "DELETE")],
         [], ["SQLite journal_mode", "SQLite cache initialized"], None),
    ]
    for name, lines, need, avoid, file_mode in scenarios:
        d = Path(tempfile.mkdtemp(dir=work))
        seeded = "existing row" in name
        if seeded:
            harness(H[0], d / "c.db", "-", "-")
            seed_row(d / "c.db")
        log = server_log(binary, d, lines)
        ok = all(n in log for n in need) and not any(a in log for a in avoid) and "Translation cache initialized" in log
        if file_mode is not None:
            fm = file_journal(d / "c.db")
            tables = sqlite3.connect(str(d / "c.db")).execute("SELECT count(*) FROM sqlite_master WHERE name='trans_cache'").fetchone()[0]
            ok = ok and fm == file_mode and tables == 1 and (not seeded or rows(d / "c.db") == 1)
        check(ok, f"server: {name}")
        if not ok:
            print("      " + "\n      ".join(l for l in log.splitlines() if "SQLite" in l or "Warning" in l or "cache" in l)[:1500])
    if old:
        d = Path(tempfile.mkdtemp(dir=work))
        log = server_log(old, d, S + [("TRANS_CACHE_SQLITE_JOURNAL_MODE", "DELETE")])
        fm = file_journal(d / "c.db")
        print(f"  info  pre-fix binary with JOURNAL_MODE=DELETE: file journal {fm!r}, startup line present: {'SQLite journal_mode=' in log}")


H = []


def main():
    h, binary = (str(Path(a).resolve()) for a in sys.argv[1:3])
    H.append(h)
    old = str(Path(sys.argv[3]).resolve()) if len(sys.argv) > 3 else None
    with tempfile.TemporaryDirectory(prefix="pragmas-") as work:
        work = Path(work)
        part_a(h, work)
        part_b(binary, old, work)
    print("OK" if not FAILURES else f"FAILED: {len(FAILURES)}")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
