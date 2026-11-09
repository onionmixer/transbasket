"""V3b/V4/V5 of PLAN_BACKEND_MODULES.md: an old and a new transbasket must behave the same.

    python3 -I tests/test_backend_parity.py <old transbasket> <new transbasket>

Each scenario starts both binaries with the same configuration against the same offline
fakes, sends the same requests (fixed uuids and timestamps) and stops them with SIGTERM.
Compared, old vs new:
  - HTTP status, Retry-After and the body (an error body's own `timestamp` is masked:
    error responses carry the current time, json_handler.c);
  - every codex exec call: full argv (per-request temp paths replaced by <CWD>, after
    checking that -o and --output-schema live inside the -C directory), stdin prompt
    (random delimiter masked) and the raw bytes of the schema file;
  - every upstream request body (normal backend);
  - the server log: lines naming a request uuid in order per uuid, the background cache
    thread's lines as a multiset, all other lines in order - after masking timestamps,
    ports, temp paths, elapsed_ms and delimiters;
  - the DEBUG trace files (names and masked contents).
A scenario marked "unordered" (slot exhaustion: slot waits poll sem_trywait, so which waiter
wins is not fixed) is compared as sorted, uuid-, text- and group-number-masked text instead,
and its statuses must equal the expected multiset.
Servers bind 127.0.0.1; nothing leaves the machine.
"""
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FAILURES = []

FAKE_CODEX = r'''#!/usr/bin/env python3
import json, os, sys, time
from pathlib import Path
a = sys.argv[1:]
E = os.environ
if a == ["--version"]:
    if E.get("P_VERSION_FAIL"): sys.exit(1)
    print("codex-cli 0.167.0"); sys.exit(0)
if a[:2] == ["exec", "--help"]:
    if E.get("P_NO_EXEC"): sys.exit(1)
    print(E.get("P_HELP", "--skip-git-repo-check --color --ephemeral --ignore-user-config -o --output-last-message --output-schema"))
    sys.exit(0)
if a[:3] == ["mcp", "list", "--json"]:
    print(E.get("P_MCP", "[]")); sys.exit(0)
if a and a[0] == "exec":
    prompt = sys.stdin.read()
    cwd = a[a.index("-C") + 1] if "-C" in a else None
    out = a[a.index("-o") + 1] if "-o" in a else None
    schema = a[a.index("--output-schema") + 1] if "--output-schema" in a else None
    rec = {"argv": a, "cwd": cwd, "prompt": prompt,
           "schema_hex": Path(schema).read_bytes().hex() if schema else None}
    with open(E["P_CAPTURE"], "a", encoding="utf-8") as f:
        f.write(json.dumps(rec, ensure_ascii=False) + "\n")
    text = prompt
    mode = "ok"
    for m in ("FAIL", "SLEEP", "NULOUT", "BADJSON"):
        if "MODE_" + m in text: mode = m
    if mode == "FAIL":
        sys.stderr.write("prompt echo...\nERROR: fixture refused the model\n"); sys.exit(1)
    if mode == "SLEEP":
        time.sleep(4)
    if schema:
        body = "{not json" if mode == "BADJSON" else json.dumps({"source_language": "kor", "mixed": False, "translation": "번역 결과"})
    else:
        body = "앞\x00뒤" if mode == "NULOUT" else "번역 결과"
    Path(out).write_bytes(body.encode())
    sys.exit(0)
sys.exit(3)
'''


class Upstream:
    """OpenAI-compatible fake: the reply is chosen by a marker in the request body."""

    def __init__(self):
        self.bodies = []
        outer = self

        class H(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_POST(self):
                raw = self.rfile.read(int(self.headers["Content-Length"]))
                outer.bodies.append(raw)
                code, ctype, body = 200, "application/json", json.dumps(
                    {"choices": [{"message": {"role": "assistant", "content": "translated"}}]}).encode()
                if b"MODE_E400" in raw:
                    code, body = 400, b'{"error":{"message":"bad request"}}'
                elif b"MODE_E500" in raw:
                    code, body = 500, b'{"error":{"message":"upstream down"}}'
                elif b'"stream":true' in raw.replace(b" ", b""):
                    ctype = "text/event-stream"
                    if b"MODE_SSEERR" in raw:
                        body = b'data: {"choices":[{"delta":{"content":"a"}}]}\n\nevent: error\ndata: {"error":"x"}\n\n'
                    else:
                        body = b'data: {"choices":[{"delta":{"content":"tr"}}]}\n\ndata: {"choices":[{"delta":{"content":"anslated"}}]}\n\ndata: [DONE]\n\n'
                self.send_response(code)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *a):
                pass
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()


UP = Upstream()


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def http(port, payload, timeout=60):
    body = json.dumps(payload, ensure_ascii=False).encode()
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.sendall(b"POST /translate HTTP/1.0\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
        data = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    head, _, rest = data.partition(b"\r\n\r\n")
    lines = head.decode().split("\r\n")
    status = int(lines[0].split()[1])
    hdr = {l.split(":", 1)[0].lower(): l.split(":", 1)[1].strip() for l in lines[1:] if ":" in l}
    try:
        j = json.loads(rest)
    except ValueError:
        j = {"raw": rest.decode(errors="replace")}
    if status != 200 and isinstance(j, dict) and "timestamp" in j:
        j["timestamp"] = "<now>"
    return {"status": status, "retry_after": hdr.get("retry-after"), "body": j}


def req(i, text, frm="kor"):
    return {"timestamp": "2026-11-06T01:02:03.456Z", "uuid": f"550e8400-e29b-41d4-a716-4466554{i:05d}",
            "from": frm, "to": "eng", "text": text}


MASKS = [
    (re.compile(r"^\[\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\]"), "[T]"),
    (re.compile(r"elapsed_ms=\d+"), "elapsed_ms=N"),
    (re.compile(r"===SRC-[0-9a-f]{32}==="), "===SRC-D==="),
    (re.compile(r"tb-[A-Za-z0-9]{6}"), "tb-XXXXXX"),
]
BACKGROUND = ("Cache background thread", "Saving translation cache", "Translation cache saved", "SQLite cache")
# The one start line added by PLAN_CONNECTION_LIMIT.md (none of the scenarios sets HTTP_MAX_CONNECTIONS): it is
# taken out before the comparison, and main() checks that the new binary printed it exactly once and the old
# one at most once (a binary from before 2026-11-09 prints none).
LIMIT_LINE = "[T][INFO] HTTP connection limit: 1020 (libmicrohttpd default)"


def norm(text, d, port):
    out = text.replace(str(d), "<DIR>").replace(str(port), "<PORT>").replace(str(UP.server.server_port), "<UPORT>")
    lines = []
    for l in out.split("\n"):
        for rx, rep in MASKS:
            l = rx.sub(rep, l)
        lines.append(l)
    return lines


def split_log(lines):
    per_uuid, other, bg = {}, [], Counter()
    for l in lines:
        m = re.search(r"550e8400-e29b-41d4-a716-4466554\d{5}", l)
        if m:
            per_uuid.setdefault(m.group(0), []).append(l)
        elif any(b in l for b in BACKGROUND):
            bg[l] += 1
        else:
            other.append(l)
    return per_uuid, other, bg


def run(binary, scen, workroot):
    d = Path(tempfile.mkdtemp(dir=workroot))
    port = free_port()
    (d / "codex").write_text(FAKE_CODEX)
    (d / "codex").chmod(0o700)
    (d / "role.txt").write_text("role")
    (d / "explicit.txt").write_text("EXPLICIT {SOURCE_LANG} {SOURCE_CODE} {TARGET_LANG} {TARGET_CODE}\n{TEXT}", encoding="utf-8")
    (d / "run").mkdir(mode=0o700)
    conf = {"OPENAI_BASE_URL": f"http://127.0.0.1:{UP.server.server_port}/v1", "OPENAI_MODEL": "m", "OPENAI_API_KEY": "k",
            "LISTEN": "127.0.0.1", "PORT": str(port), "PROMPT_PREFIX_FILE": "explicit.txt",
            "LANGUAGE_TABLE_FILE": str(ROOT / "language_tables.conf"), "TRANS_CACHE_TYPE": "text",
            "TRANS_CACHE_FILE": str(d / "c.jsonl"), "TRANS_CACHE_THRESHOLD": "1", "TRANS_CACHE_CLEANUP_ENABLED": "false",
            "CODEX_BIN": str(d / "codex"), "CODEX_RUN_DIR": str(d / "run"), "CODEX_TIMEOUT": "2",
            "TRANSLATION_TOTAL_TIMEOUT": "20", "DEBUG": "yes", "CODEX_MODEL": "fixture-model",
            "CODEX_REASONING_EFFORT": "low", "CODEX_MAX_CONCURRENCY": "1"}
    conf.update(scen["conf"])
    (d / "t.conf").write_text("".join(f'{k}="{v}"\n' for k, v in conf.items()))
    env = {**os.environ, "P_CAPTURE": str(d / "capture.jsonl"), **scen.get("env", {})}
    UP.bodies.clear()
    log = open(d / "log", "wb")
    p = subprocess.Popen([binary, "-c", str(d / "t.conf"), "-r", str(d / "role.txt")], cwd=d, stdout=log,
                         stderr=subprocess.STDOUT, env=env)
    for _ in range(300):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.02)
    replies = {}
    for group in scen["requests"]:            # requests in one group are sent concurrently
        ts = []
        for i, text, frm in group:
            def go(i=i, text=text, frm=frm):
                replies[i] = http(port, req(i, text, frm))
            t = threading.Thread(target=go)
            t.start()
            ts.append(t)
            time.sleep(0.3)                     # deterministic order of arrival within a group
        for t in ts:
            t.join(60)
    p.terminate()
    p.wait(timeout=30)
    log.close()
    calls = []
    cap = d / "capture.jsonl"
    for line in (cap.read_text(encoding="utf-8").splitlines() if cap.exists() else []):
        r = json.loads(line)
        cwd = r["cwd"]
        argv = r["argv"]
        if cwd:
            for flag in ("-o", "--output-schema"):
                if flag in argv:
                    v = argv[argv.index(flag) + 1]
                    if not v.startswith(cwd + "/"):
                        FAILURES.append(f"{scen['name']}: {flag} {v} is not inside -C {cwd}")
            argv = [x.replace(cwd, "<CWD>") for x in argv]
        calls.append({"argv": argv, "prompt": MASKS[2][0].sub("===SRC-D===", r["prompt"]), "schema_hex": r["schema_hex"]})
    calls.sort(key=lambda c: c["prompt"])     # concurrent calls may start in either order
    traces = {}
    if (d / "trace").is_dir():
        for f in sorted((d / "trace").iterdir()):
            traces[f.name] = norm(f.read_text(errors="replace"), d, port)
    lines = norm((d / "log").read_text(errors="replace"), d, port)
    limit_lines = lines.count(LIMIT_LINE)
    lines = [l for l in lines if l != LIMIT_LINE]
    bodies = sorted(MASKS[2][0].sub("===SRC-D===", b.decode(errors="replace")) for b in UP.bodies)
    shutil.rmtree(d)
    return {"replies": replies, "calls": calls, "log": split_log(lines), "traces": traces, "upstream": bodies,
            "limit_lines": limit_lines}


FULL = "--skip-git-repo-check --color --ephemeral --ignore-user-config -o --output-last-message --output-schema"
CODEX_REQS = [[(1, "안녕하세요", "kor")], [(2, "明日の会議は午後三時です", "auto")], [(3, "MODE_FAIL 실패", "kor")],
              [(4, "MODE_NULOUT 널", "kor")], [(5, "MODE_BADJSON 自動", "auto")],
              [(6, "MODE_SLEEP 느림", "kor"), (7, "슬롯 대기", "kor")], [(1, "안녕하세요", "kor")]]
NORMAL_REQS = [[(11, "안녕하세요", "kor")], [(12, "MODE_E400 x", "kor")], [(13, "明日", "auto")], [(14, "MODE_E500 y", "kor")]]
SCENARIOS = [
    {"name": "normal", "conf": {"MODEL_TYPE": "normal"}, "requests": NORMAL_REQS},
    {"name": "normal + model hint mode", "conf": {"MODEL_TYPE": "normal", "SOURCE_HINT_MODE": "model"}, "requests": NORMAL_REQS[:1]},
    {"name": "normal stream", "conf": {"MODEL_TYPE": "normal", "STREAM": "true"},
     "requests": [[(21, "안녕하세요", "kor")], [(22, "MODE_SSEERR z", "kor")]]},
    {"name": "codex full CLI, model hints", "conf": {"MODEL_TYPE": "codex", "SOURCE_HINT_MODE": "model"},
     "env": {"P_HELP": FULL}, "requests": CODEX_REQS},
    # Slot waits poll sem_trywait (no FIFO), so which waiter wins is not fixed: four slow requests on one
    # slot always give two timeouts (the holders) and two slot-wait failures, in some order.
    {"name": "codex slot exhaustion", "conf": {"MODEL_TYPE": "codex"}, "env": {"P_HELP": FULL}, "unordered": [503, 503, 504, 504],
     "requests": [[(31, "MODE_SLEEP 일", "kor"), (32, "MODE_SLEEP 이", "kor"), (33, "MODE_SLEEP 삼", "kor"), (34, "MODE_SLEEP 사", "kor")]]},
    {"name": "codex isolated", "conf": {"MODEL_TYPE": "codex", "CODEX_IGNORE_USER_CONFIG": "true"},
     "env": {"P_HELP": FULL}, "requests": CODEX_REQS[:3]},
    {"name": "codex --version fails", "conf": {"MODEL_TYPE": "codex"}, "env": {"P_VERSION_FAIL": "1"}, "requests": CODEX_REQS[:1]},
    {"name": "codex no exec", "conf": {"MODEL_TYPE": "codex"}, "env": {"P_NO_EXEC": "1"}, "requests": []},
    {"name": "codex no --ephemeral", "conf": {"MODEL_TYPE": "codex"},
     "env": {"P_HELP": "--skip-git-repo-check --ignore-user-config -o --output-schema"}, "requests": CODEX_REQS[:1]},
    {"name": "codex isolation flag missing", "conf": {"MODEL_TYPE": "codex", "CODEX_IGNORE_USER_CONFIG": "true"},
     "env": {"P_HELP": "--skip-git-repo-check --ephemeral -o"}, "requests": []},
    {"name": "codex model hints without --output-schema", "conf": {"MODEL_TYPE": "codex", "SOURCE_HINT_MODE": "model"},
     "env": {"P_HELP": "--skip-git-repo-check --ephemeral --ignore-user-config -o"}, "requests": CODEX_REQS[1:2]},
    {"name": "codex MCP servers, not isolated", "conf": {"MODEL_TYPE": "codex"},
     "env": {"P_HELP": FULL, "P_MCP": json.dumps([{"name": "srvA", "enabled": True}, {"name": "srvB", "enabled": True}])},
     "requests": []},
    {"name": "codex MCP servers, isolated", "conf": {"MODEL_TYPE": "codex", "CODEX_IGNORE_USER_CONFIG": "true"},
     "env": {"P_HELP": FULL, "P_MCP": json.dumps([{"name": "srvA", "enabled": True}])}, "requests": []},
]


UUID = re.compile(r"550e8400-e29b-41d4-a716-4466554\d{5}")


def unordered(r):
    """For scenarios whose request order is not fixed: everything as sorted, uuid-masked text."""
    m = lambda x: UUID.sub("<UUID>", json.dumps(x, ensure_ascii=False, sort_keys=True))
    rx = re.compile(r"(MODE_SLEEP|슬롯) \S+")
    grp = re.compile(r"Translation group \d+")
    m2 = lambda x: grp.sub("Translation group N", rx.sub("<TEXT>", m(x)))
    return {"replies": sorted(m2(v) for v in r["replies"].values()), "calls": sorted(m2(c) for c in r["calls"]),
            "traces": sorted(m2(v) for v in r["traces"].values()), "upstream": r["upstream"],
            "log": (sorted(m2(v) for v in r["log"][0].values()), sorted(m2(l) for l in r["log"][1]), r["log"][2])}


def compare(name, a, b):
    ok = True
    for key in ("replies", "calls", "traces", "upstream"):
        if a[key] != b[key]:
            ok = False
            print(f"  DIFF {name}: {key}")
            print("    old:", json.dumps(a[key], ensure_ascii=False)[:600])
            print("    new:", json.dumps(b[key], ensure_ascii=False)[:600])
    for part, label in zip(range(3), ("per-uuid log", "other log (ordered)", "background log (multiset)")):
        if a["log"][part] != b["log"][part]:
            ok = False
            print(f"  DIFF {name}: {label}")
            print("    old:", str(a["log"][part])[:800])
            print("    new:", str(b["log"][part])[:800])
    return ok


def main():
    old, new = (str(Path(x).resolve()) for x in sys.argv[1:3])
    only = sys.argv[3] if len(sys.argv) > 3 else None
    with tempfile.TemporaryDirectory(prefix="parity-") as work:
        for scen in SCENARIOS:
            if only and only not in scen["name"]:
                continue
            a, b = run(old, scen, work), run(new, scen, work)
            same = compare(scen["name"], *((unordered(a), unordered(b)) if scen.get("unordered") else (a, b)))
            statuses = [a["replies"][k]["status"] for k in sorted(a["replies"])]
            print(f"{'ok  ' if same else 'FAIL'} {scen['name']}: statuses {statuses}, exec calls {len(a['calls'])}, "
                  f"upstream {len(a['upstream'])}, traces {len(a['traces'])}, log lines "
                  f"{sum(map(len, a['log'][0].values())) + len(a['log'][1]) + sum(a['log'][2].values())}")
            if scen.get("unordered") and sorted(statuses) != scen["unordered"]:
                FAILURES.append(f"{scen['name']}: statuses {sorted(statuses)}, expected {scen['unordered']}")
            if b["limit_lines"] != 1 or a["limit_lines"] > 1:
                FAILURES.append(f"{scen['name']}: connection limit start line old {a['limit_lines']}x, "
                                f"new {b['limit_lines']}x (want new 1, old 0 or 1)")
            if not same:
                FAILURES.append(scen["name"])
    print("parity:", "OK" if not FAILURES else f"FAILED {FAILURES}")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
