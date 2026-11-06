"""PLAN_REMOVE_LEGACY_MCP.md: CODEX_INTERFACE after the legacy MCP route was removed.

    python3 -I tests/test_codex_interface.py <transbasket>
    CODEX_IFACE_OLD_BIN=<pre-removal transbasket> python3 -I tests/test_codex_interface.py <transbasket>

D1 "mcp" (any case) in codex mode stops startup; D2 in normal mode it is a warning;
D3 other invalid values still warn and use exec; D4 the last CODEX_INTERFACE line wins
and MODEL_TYPE may come before or after it. Servers bind 127.0.0.1; codex is a fake.
"""
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[1]
FAILURES = []
REMOVED = 'Error: CODEX_INTERFACE "mcp" was removed'
IGNORED = 'Warning: CODEX_INTERFACE "mcp" was removed; ignored because MODEL_TYPE is normal'
INVALID = "Warning: Invalid CODEX_INTERFACE"

FAKE_CODEX = """#!/usr/bin/env python3
import sys
a = sys.argv[1:]
if a == ["--version"]:
    print("codex-cli 0.167.0")
elif a[:2] == ["exec", "--help"]:
    print("--skip-git-repo-check --color --ephemeral --ignore-user-config -o --output-last-message --output-schema")
elif a[:2] == ["mcp", "list"]:
    print("[]")
else:
    sys.exit(3)
"""


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        FAILURES.append(what)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def start(binary, d, lines):
    """Write `lines` (ordered (key, value) pairs, repeats allowed) and start the server.
    Returns (started, returncode or None, port refused after a failed start, output)."""
    port = free_port()
    base = [("OPENAI_BASE_URL", "http://127.0.0.1:9/v1"), ("OPENAI_MODEL", "m"), ("OPENAI_API_KEY", "k"),
            ("LISTEN", "127.0.0.1"), ("PORT", str(port)), ("PROMPT_PREFIX_FILE", str(ROOT / "PROMPT_PREFIX.txt")),
            ("LANGUAGE_TABLE_FILE", str(ROOT / "language_tables.conf")), ("TRANS_CACHE_TYPE", "text"),
            ("TRANS_CACHE_FILE", str(d / "c.jsonl")), ("TRANS_CACHE_THRESHOLD", "1"),
            ("CODEX_BIN", str(d / "codex")), ("CODEX_RUN_DIR", str(d))]
    conf = d / "t.conf"
    conf.write_text("".join(f'{k}="{v}"\n' for k, v in base + lines))
    log = d / "log"
    with open(log, "wb") as out:
        p = subprocess.Popen([binary, "-c", str(conf), "-r", "role.txt"], cwd=d, stdout=out, stderr=subprocess.STDOUT)
        started = False
        end = time.time() + 10
        while time.time() < end and p.poll() is None:
            try:
                urlopen(f"http://127.0.0.1:{port}/health", timeout=0.3).close()
                started = True
                break
            except OSError:
                time.sleep(0.05)
        rc = p.poll()
        if rc is None:
            p.kill()
            p.wait()
    refused = False
    if not started:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
        except ConnectionRefusedError:
            refused = True
    return started, rc, refused, log.read_text(errors="replace")


CODEX = [("MODEL_TYPE", "codex")]
NORMAL = [("MODEL_TYPE", "normal")]
I = lambda v: [("CODEX_INTERFACE", v)]  # noqa: E731

# (name, lines, expect start, must contain, must not contain)
ROWS = [
    ("codex + mcp", CODEX + I("mcp"), False, [REMOVED], []),
    ("codex + MCP (case)", CODEX + I("MCP"), False, [REMOVED], []),
    ("mcp before MODEL_TYPE=codex", I("mcp") + CODEX, False, [REMOVED], []),
    ("codex: mcp then exec (last wins)", CODEX + I("mcp") + I("exec"), True, ["Codex Interface: exec"], [REMOVED, INVALID]),
    ("codex: exec then mcp (last wins)", CODEX + I("exec") + I("mcp"), False, [REMOVED], []),
    ("codex: mcp then invalid (last wins)", CODEX + I("mcp") + I("bogus"), True, [INVALID + " 'bogus'"], [REMOVED]),
    ("codex + exec", CODEX + I("exec"), True, ["Codex Interface: exec", "(interface=exec)"], [REMOVED, INVALID, "Warning: CODEX_INTERFACE"]),
    ("codex + EXEC (case)", CODEX + I("EXEC"), True, ["Codex Interface: exec"], [REMOVED, INVALID]),
    ("codex + invalid value (unchanged)", CODEX + I("bogus"), True, [INVALID + " 'bogus', using 'exec'"], [REMOVED]),
    ("codex, no CODEX_INTERFACE line", CODEX, True, ["Codex Interface: exec"], [REMOVED, INVALID]),
    ("normal + mcp", NORMAL + I("mcp"), True, [IGNORED], [REMOVED]),
    ("mcp before MODEL_TYPE=normal", I("mcp") + NORMAL, True, [IGNORED], [REMOVED]),
    ("normal: mcp then exec", NORMAL + I("mcp") + I("exec"), True, [], [IGNORED, REMOVED, INVALID]),
]


def main():
    binary = str(Path(sys.argv[1]).resolve())
    old = os.environ.get("CODEX_IFACE_OLD_BIN")
    with tempfile.TemporaryDirectory(prefix="codex-iface-") as work:
        for name, lines, want_start, need, avoid in ROWS:
            d = Path(tempfile.mkdtemp(dir=work))
            (d / "role.txt").write_text("role")
            (d / "codex").write_text(FAKE_CODEX)
            (d / "codex").chmod(0o700)
            started, rc, refused, out = start(binary, d, lines)
            ok = started == want_start and (want_start or (rc == 1 and refused))
            if want_start:  # the backend the row is about really came up
                codex = ("MODEL_TYPE", "codex") in lines
                need = need + (["codex translator initialized", "(interface=exec)"] if codex
                               else ["OpenAI translator initialized"])
            ok = ok and all(n in out for n in need) and not any(a in out for a in avoid)
            if IGNORED in need:
                ok = ok and out.count(IGNORED) == 1
            check(ok, f"{name}: {'starts' if want_start else 'exit 1, no listener'}"
                      f" (started={started}, rc={rc})")
            if not ok:
                print("      " + "\n      ".join(out.splitlines()[-8:]))
            if old and name == "codex + mcp":
                started_old, _, _, out_old = start(old, d, lines)
                print(f"  info  pre-removal binary, {name}: started={started_old}, "
                      f"interface line: {[l for l in out_old.splitlines() if 'Interface' in l or 'interface=' in l][:2]}")
    print(f"{len(ROWS)} rows: " + ("OK" if not FAILURES else f"FAILED {len(FAILURES)}"))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
