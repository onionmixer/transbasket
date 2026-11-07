"""V4 of PLAN_OUT_OF_SCOPE.md: cache_tool leaves a database's journal mode alone.

    python3 -I tests/test_cache_tool_journal.py <cache_tool> <pragma harness> <transbasket> [<pre-fix cache_tool>]

cache_tool opens SQLite in three places (migrate source, migrate destination -
new or existing -, backfill-hints incl. --dry-run). Each must keep a DELETE
database in DELETE and a WAL one in WAL, with correct data (backfill writes one
script hint per auto row, --dry-run writes none and leaves every row as it
was); a database cache_tool creates starts in DELETE and becomes WAL when the
server (default WAL) opens it; migrating a SQLite file onto itself - same path,
symlink, hard link, relative path, "missing-dir/.." (SQLite folds it), or one
that does not exist yet - is refused;
a backfill runs to the end while the server holds the same DELETE-mode database. Only WAL persists in a
file, so DELETE vs WAL is what a new connection can tell apart.
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


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        FAILURES.append(what)


def jm(db):
    c = sqlite3.connect(str(db))
    try:
        return c.execute("PRAGMA journal_mode").fetchone()[0]
    finally:
        c.close()


def rows(db):
    c = sqlite3.connect(str(db))
    try:
        return c.execute("SELECT count(*) FROM trans_cache").fetchone()[0]
    finally:
        c.close()


def data(db):
    """The rows of the cache and hint tables (opening the database re-creates
    the trigger and view, so the file's bytes are not a fair comparison)."""
    c = sqlite3.connect(str(db))
    try:
        return (c.execute("SELECT * FROM trans_cache ORDER BY id").fetchall(),
                c.execute("SELECT * FROM trans_cache_source_hint ORDER BY 1").fetchall())
    finally:
        c.close()


def hints(db):
    c = sqlite3.connect(str(db))
    try:
        return c.execute("SELECT count(*) FROM trans_cache_source_hint").fetchone()[0]
    finally:
        c.close()


def make_db(harness, db, mode, n=3):
    """Schema through the real backend (DELETE or WAL), then n (<= 3) auto rows."""
    p = subprocess.run([harness, str(db), mode, "NORMAL"], capture_output=True, text=True, timeout=30)
    assert p.returncode == 0 and not p.stderr, p.stderr
    c = sqlite3.connect(str(db))
    for i, text in enumerate(("안녕하세요", "Hello there", "こんにちは")[:n]):
        c.execute("INSERT INTO trans_cache (hash, from_lang, to_lang, source_text, translated_text, count, last_used, created_at) "
                  "VALUES (?, 'auto', 'eng', ?, 'x', 1, 1, 1)", (f"{i:064x}", text))
    c.commit()
    c.close()
    assert jm(db) == mode.lower(), (jm(db), mode)


def tool(cache_tool, *args):
    return subprocess.run([cache_tool, *args], capture_output=True, text=True, timeout=60)


def main():
    cache_tool, harness, server = (str(Path(a).resolve()) for a in sys.argv[1:4])
    old = str(Path(sys.argv[4]).resolve()) if len(sys.argv) > 4 else None
    with tempfile.TemporaryDirectory(prefix="tooljm-") as w:
        w = Path(w)
        for mode in ("DELETE", "WAL"):
            for dry in (True, False):
                db = w / f"bf_{mode}_{dry}.db"
                make_db(harness, db, mode)
                before = data(db)
                p = tool(cache_tool, "backfill-hints", "--backend", "sqlite", "--config", str(db), *(["--dry-run"] if dry else []))
                data_ok = data(db) == before if dry else hints(db) == 3
                check(p.returncode == 0 and jm(db) == mode.lower() and rows(db) == 3 and data_ok,
                      f"backfill{' --dry-run' if dry else ''} on a {mode} database keeps {mode.lower()}, "
                      f"{'rows unchanged' if dry else '3 hints written'} (rc {p.returncode}, now {jm(db)}, hints {hints(db)})")
            src = w / f"src_{mode}.db"
            dst = w / f"dst_{mode}.db"
            make_db(harness, src, mode)
            p = tool(cache_tool, "migrate", "--from", "sqlite", "--from-config", str(src), "--to", "sqlite", "--to-config", str(dst))
            check(p.returncode == 0 and jm(src) == mode.lower() and dst.exists() and jm(dst) == "delete" and rows(dst) == 3,
                  f"migrate from a {mode} database: source keeps {mode.lower()}, new destination starts delete, 3 rows copied "
                  f"(rc {p.returncode}, source {jm(src)}, destination {jm(dst) if dst.exists() else 'missing'})")
            # an existing destination keeps its own mode. Its one row has a made-up hash,
            # migrate computes the real ones, so 1 + 3 rows afterwards
            for dmode in ("DELETE", "WAL"):
                dst2 = w / f"dst2_{mode}_{dmode}.db"
                make_db(harness, dst2, dmode, n=1)
                p = tool(cache_tool, "migrate", "--from", "sqlite", "--from-config", str(src), "--to", "sqlite", "--to-config", str(dst2))
                check(p.returncode == 0 and jm(src) == mode.lower() and jm(dst2) == dmode.lower() and rows(dst2) == 4,
                      f"migrate {mode} -> existing {dmode} destination: both keep their mode, 1 + 3 rows (rc {p.returncode}, "
                      f"destination {jm(dst2)}, rows {rows(dst2)})")
        dst = w / "dst_DELETE.db"
        p = subprocess.run([harness, str(dst), "-", "-"], capture_output=True, text=True, timeout=30)   # server default
        check(jm(dst) == "wal", f"the server's default then switches the new database to wal ({jm(dst)})")
        same = w / "same.db"
        make_db(harness, same, "DELETE")
        p = tool(cache_tool, "migrate", "--from", "sqlite", "--from-config", str(same), "--to", "sqlite", "--to-config", str(same))
        alias = w / "alias.db"
        alias.symlink_to(same)
        hard = w / "hard.db"
        os.link(same, hard)
        refused = {}
        for name, to in (("same path", str(same)), ("symlink", str(alias)), ("hard link", str(hard))):
            refused[name] = tool(cache_tool, "migrate", "--from", "sqlite", "--from-config", str(same), "--to", "sqlite", "--to-config", to)
        (w / "sub").mkdir(exist_ok=True)
        rel = subprocess.run([cache_tool, "migrate", "--from", "sqlite", "--from-config", "same.db", "--to", "sqlite",
                              "--to-config", "./sub/../same.db"], capture_output=True, text=True, timeout=60, cwd=w)
        refused["relative path"] = rel
        assert not (w / "missing").exists()
        # SQLite folds "missing/.." itself although the directory does not exist
        refused["through a missing directory"] = tool(cache_tool, "migrate", "--from", "sqlite", "--from-config", str(same),
                                                      "--to", "sqlite", "--to-config", str(w / "missing" / ".." / "same.db"))
        ghost = w / "not_yet.db"
        refused["not existing yet"] = tool(cache_tool, "migrate", "--from", "sqlite", "--from-config", str(ghost),
                                           "--to", "sqlite", "--to-config", str(w / "sub" / ".." / "not_yet.db"))
        bad = [k for k, r in refused.items() if r.returncode == 0 or "same SQLite file" not in r.stderr]
        check(not bad and rows(same) == 3 and not ghost.exists(),
              f"migrating a SQLite file onto itself is refused ({', '.join(refused)}), data untouched, nothing created "
              f"(not refused: {bad})")
        # two different files that do not exist yet are not the same
        p = tool(cache_tool, "migrate", "--from", "sqlite", "--from-config", str(w / "ghost_a.db"),
                 "--to", "sqlite", "--to-config", str(w / "ghost_b.db"))
        check(p.returncode == 0 and "same SQLite file" not in p.stderr, f"two different new files are not refused (rc {p.returncode})")
        # backfill while the server holds the same DELETE-mode database
        live = w / "live.db"
        make_db(harness, live, "DELETE")
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        (w / "r.txt").write_text("role")
        (w / "t.conf").write_text(f'OPENAI_BASE_URL="http://127.0.0.1:9/v1"\nOPENAI_MODEL="m"\nOPENAI_API_KEY="k"\nLISTEN="127.0.0.1"\n'
                                  f'PORT="{port}"\nPROMPT_PREFIX_FILE="{ROOT}/PROMPT_PREFIX.txt"\nTRANS_CACHE_TYPE="sqlite"\n'
                                  f'TRANS_CACHE_SQLITE_PATH="{live}"\nTRANS_CACHE_SQLITE_JOURNAL_MODE="DELETE"\n')
        srv = subprocess.Popen([server, "-c", str(w / "t.conf"), "-r", "r.txt"], cwd=w, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        up = False
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                up = True
                break
            except OSError:
                time.sleep(0.05)
        check(up and srv.poll() is None, "the server is up on the DELETE database")
        t0 = time.time()
        p = tool(cache_tool, "backfill-hints", "--backend", "sqlite", "--config", str(live))
        took = time.time() - t0
        srv.terminate()
        srv.wait(timeout=20)
        check(p.returncode == 0 and took < 30 and jm(live) == "delete",
              f"backfill while the server holds the DELETE database finishes ({took:.1f} s, rc {p.returncode}, {jm(live)})")
        if old:
            db = w / "old.db"
            make_db(harness, db, "DELETE")
            tool(old, "backfill-hints", "--backend", "sqlite", "--config", str(db), "--dry-run")
            print(f"  info  pre-fix cache_tool: backfill --dry-run turned a DELETE database into {jm(db)!r}")
    print("OK" if not FAILURES else f"FAILED: {len(FAILURES)}")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
