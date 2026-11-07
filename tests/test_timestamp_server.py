"""PLAN_CODE_ISSUES_4.md V3/V4 through the real server.

    python3 -I tests/test_timestamp_server.py <transbasket> [<pre-fix transbasket>]

V3: a 63-byte timestamp (Z and +HH:MM forms) is answered unchanged and names the DEBUG
trace file (normal and codex); 64 bytes and impossible dates are 422; a cached source
text with a bad timestamp is 422 too; coalesced owner/waiter and a cache hit each get
their own timestamp.
V4: a missing relative prompt file logs the path that was tried (with and without -p),
not "(null)". Servers bind 127.0.0.1; upstream and codex are offline fakes.
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
FAILURES = []
BASE = "2026-11-07T01:02:03."
TS63Z = BASE + "1" * 42 + "Z"
TS63O = BASE + "2" * 37 + "+09:00"
TS64Z = BASE + "1" * 43 + "Z"
TS64O = BASE + "2" * 38 + "+09:00"
assert [len(x) for x in (TS63Z, TS63O, TS64Z, TS64O)] == [63, 63, 64, 64]


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        FAILURES.append(what)


class Upstream:
    def __init__(self):
        self.calls = 0
        outer = self

        class H(BaseHTTPRequestHandler):
            def do_POST(self):
                raw = self.rfile.read(int(self.headers["Content-Length"]))
                outer.calls += 1
                if b"SLOW" in raw:
                    time.sleep(3)
                body = json.dumps({"choices": [{"message": {"role": "assistant", "content": "translated"}}]}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
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


class Server:
    def __init__(self, binary, d, codex=False, env=None):
        self.d, self.port = Path(d), free_port()
        (self.d / "role.txt").write_text("role")
        (self.d / "run").mkdir()
        v = {"OPENAI_BASE_URL": f"http://127.0.0.1:{UP.server.server_port}/v1", "OPENAI_MODEL": "m", "OPENAI_API_KEY": "k",
             "LISTEN": "127.0.0.1", "PORT": str(self.port), "PROMPT_PREFIX_FILE": str(ROOT / "PROMPT_PREFIX.txt"),
             "LANGUAGE_TABLE_FILE": str(ROOT / "language_tables.conf"), "TRANS_CACHE_TYPE": "text",
             "TRANS_CACHE_FILE": str(self.d / "c.jsonl"), "TRANS_CACHE_THRESHOLD": "1", "DEBUG": "yes",
             "TRANSLATION_TOTAL_TIMEOUT": "20"}
        if codex:
            fake = self.d / "codex"
            fake.write_bytes((ROOT / "tests/fake_codex_auto.py").read_bytes())
            fake.chmod(0o700)
            v.update({"MODEL_TYPE": "codex", "CODEX_BIN": str(fake), "CODEX_RUN_DIR": str(self.d / "run"), "CODEX_TIMEOUT": "10"})
        (self.d / "t.conf").write_text("".join(f'{k}="{x}"\n' for k, x in v.items()))
        self.log = open(self.d / "log", "wb")
        self.p = subprocess.Popen([binary, "-c", str(self.d / "t.conf"), "-r", "role.txt"], cwd=self.d, stdout=self.log,
                                  stderr=subprocess.STDOUT,
                                  env={**os.environ, "TB_AUTO_CAPTURE": str(self.d / "cap"), **(env or {})})
        for _ in range(300):
            try:
                urlopen(f"http://127.0.0.1:{self.port}/health", timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.02)

    def post(self, ts, text, uid):
        body = json.dumps({"timestamp": ts, "uuid": uid, "from": "kor", "to": "eng", "text": text}).encode()
        req = Request(f"http://127.0.0.1:{self.port}/translate", data=body, headers={"Content-Type": "application/json"})
        try:
            with urlopen(req, timeout=30) as r:
                return r.status, json.loads(r.read())
        except HTTPError as e:
            return e.code, json.loads(e.read() or b"{}")

    def engine_calls(self, codex):
        """Translations the engine really ran (upstream requests, or codex exec runs)."""
        if not codex:
            return UP.calls
        cap = self.d / "cap"
        return len(cap.read_text(encoding="utf-8").splitlines()) if cap.exists() else 0

    def log_text(self):
        self.log.flush()
        return (self.d / "log").read_text(errors="replace")

    def traces(self):
        t = self.d / "trace"
        return sorted(p.name for p in t.iterdir()) if t.is_dir() else []

    def stop(self):
        self.p.terminate()
        self.p.wait(timeout=20)
        self.log.close()
        return (self.d / "log").read_text(errors="replace")


def uid(n):
    return f"550e8400-e29b-41d4-a716-4466554{n:05d}"


def v3(binary, work):
    for codex in (False, True):
        tag = "codex" if codex else "normal"
        s = Server(binary, tempfile.mkdtemp(dir=work), codex)
        c0 = s.engine_calls(codex)
        r1 = s.post(TS63Z, "가나다", uid(1))
        c1 = s.engine_calls(codex)
        r2 = s.post(TS63O, "라마바", uid(2))
        r3 = s.post(TS64Z, "사아자", uid(3))
        r4 = s.post(TS64O, "차카타", uid(4))
        r5 = s.post("2026-02-30T00:00:00Z", "파하", uid(5))
        r6 = s.post("2026-11-07T01:02:03+24:00", "가나다", uid(6))     # cached text, bad timestamp
        c6 = s.engine_calls(codex)
        r7 = s.post("2026-11-07T05:06:07Z", "가나다", uid(7))         # cached text, good timestamp
        c7 = s.engine_calls(codex)
        traces = s.traces()
        s.stop()
        check(c1 - c0 == 1 and c7 == c6 and r7[0] == 200 and r7[1].get("timestamp") == "2026-11-07T05:06:07Z",
              f"{tag}: the text was cached (no engine call for the repeat) and the cache hit answers with its own timestamp")
        check(r1[0] == 200 and r1[1].get("timestamp") == TS63Z and r2[0] == 200 and r2[1].get("timestamp") == TS63O,
              f"{tag}: 63-byte timestamps (Z and +HH:MM) answered unchanged")
        ext = ".codex.txt" if codex else ".txt"
        names = [TS63Z.replace(":", "-") + "_" + uid(1) + ext, TS63O.replace(":", "-") + "_" + uid(2) + ext]
        check(all(n in traces for n in names), f"{tag}: trace files named with the full timestamps")
        check([r3[0], r4[0], r5[0], r6[0]] == [422] * 4 and all(r[1].get("errorCode") == "VALIDATION_ERROR" for r in (r3, r4, r5, r6)),
              f"{tag}: 64 bytes (both forms), Feb 30 and offset 24:00 -> 422, also for a cached source text "
              f"({[r3[0], r4[0], r5[0], r6[0]]})")
    tss = ["2026-11-07T01:02:03.1Z", "2026-11-07T01:02:03.22+01:00", "2026-11-07T01:02:03.333-05:30"]
    for codex in (False, True):
        tag = "codex" if codex else "normal"
        d = Path(tempfile.mkdtemp(dir=work))
        gate = d / "release"
        # codex: the fake exec waits for the gate file, so the followers join for sure;
        # normal: the upstream sleeps 3 s for "SLOW".
        env = {"TB_INFLIGHT_GATE": str(gate), "TB_INFLIGHT_PIDS": str(d / "pids"), "TB_INFLIGHT_STAGE": "reply"} if codex else None
        s = Server(binary, d, codex, env)
        with ThreadPoolExecutor(3) as ex:
            futs = [ex.submit(s.post, tss[0], "SLOW 같은 원문", uid(10))]
            for _ in range(200):
                if s.log_text().count("role=owner"):
                    break
                time.sleep(0.02)
            for i, ts in enumerate(tss[1:], 1):
                futs.append(ex.submit(s.post, ts, "SLOW 같은 원문", uid(10 + i)))
            for _ in range(200):
                if s.log_text().count(" joined (waiters=") >= 2:
                    break
                time.sleep(0.02)
            if codex:
                gate.touch()
            got = [f.result() for f in futs]
        before_hit = s.engine_calls(codex)
        hit = s.post("2026-11-07T09:09:09Z", "SLOW 같은 원문", uid(20))
        hit_calls = s.engine_calls(codex) - before_hit
        log = s.stop()
        joined = log.count(" joined (waiters=")
        check(all(g[0] == 200 for g in got) and [g[1]["timestamp"] for g in got] == tss and joined == 2 and
              hit[0] == 200 and hit[1]["timestamp"] == "2026-11-07T09:09:09Z" and hit_calls == 0,
              f"{tag}: owner and {joined} waiters each answer with their own timestamp, and so does a cache hit (engine calls {hit_calls})")


def v4(binary, work, label):
    out = {}
    for name, extra in (("config PROMPT_PREFIX_FILE", []), ("-p", ["-p", "missing-p.txt"])):
        d = Path(tempfile.mkdtemp(dir=work))
        (d / "role.txt").write_text("role")
        (d / "t.conf").write_text(f'OPENAI_BASE_URL="http://127.0.0.1:9/v1"\nOPENAI_MODEL="m"\nOPENAI_API_KEY="k"\n'
                                  f'LISTEN="127.0.0.1"\nPORT="{free_port()}"\nPROMPT_PREFIX_FILE="missing-conf.txt"\n')
        p = subprocess.run([binary, "-c", str(d / "t.conf"), "-r", "role.txt", *extra], cwd=d, capture_output=True, text=True, timeout=20)
        line = [l for l in (p.stdout + p.stderr).splitlines() if "Could not resolve prompt prefix path" in l]
        out[name] = (p.returncode, line[0].split("path: ", 1)[1].strip() if line else None)
    return out


def main():
    binary = str(Path(sys.argv[1]).resolve())
    old = str(Path(sys.argv[2]).resolve()) if len(sys.argv) > 2 else None
    with tempfile.TemporaryDirectory(prefix="ts-server-") as work:
        v3(binary, work)
        got = v4(binary, work, "new")
        check(got == {"config PROMPT_PREFIX_FILE": (1, "missing-conf.txt"), "-p": (1, "missing-p.txt")},
              f"missing relative prompt file: the tried path is logged {got}")
        if old:
            print(f"  info  pre-fix binary: {v4(old, work, 'old')}")
    print("OK" if not FAILURES else f"FAILED: {len(FAILURES)}")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
