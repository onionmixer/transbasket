"""V2 of PLAN_OUT_OF_SCOPE.md: NORMAL_UNESCAPE (normal backend).

    python3 -I tests/test_normal_unescape.py <transbasket>

The fake upstream returns translations with every escape unescape_string() knows
(\\n \\t \\r \\\\ \\" \\') and some it leaves alone (\\u, \\/, an unknown escape, a trailing
backslash). Expected bytes come from tests/strip_ref.py: default and "yes" =
strip(unescape(x)) (unchanged behaviour), "no" = strip(x); an invalid value warns and
means "yes". Also: escaped tabs only ("yes": nothing left -> 502; "no": the text as sent), escapes split across
SSE events, what is cached, and that a cached translation made under "yes" is still
served after a restart with "no" (the cache key is the source text only).
"""
import json
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import strip_ref as ref  # noqa: E402

FAILURES = []
B = "\\"
TEXT = f"C:{B}new{B}table {B}{B}server {B}\"q{B}\" it{B}'s{B}tTAB{B}rCR {B}u0041 a{B}/b {B}q end{B}"
REPLIES = {"plain": TEXT, "spaces": f"{B}t{B}r {B}t", "sse": TEXT}


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
                key = next(k for k in REPLIES if f"CASE_{k}".encode() in raw)
                content = REPLIES[key]
                if b'"stream":true' in raw.replace(b" ", b""):
                    # split the content so that escapes straddle event boundaries
                    parts = [content[i:i + 3] for i in range(0, len(content), 3)]
                    body = b"".join(b"data: " + json.dumps({"choices": [{"delta": {"content": p}}]}).encode() + b"\n\n"
                                    for p in parts) + b"data: [DONE]\n\n"
                    ctype = "text/event-stream"
                else:
                    body = json.dumps({"choices": [{"message": {"content": content}}]}).encode()
                    ctype = "application/json"
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *a):
                pass
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()


UP = Upstream()


def run(binary, d, setting, stream, requests):
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    (d / "r.txt").write_text("role")
    conf = (f'OPENAI_BASE_URL="http://127.0.0.1:{UP.server.server_port}/v1"\nOPENAI_MODEL="m"\nOPENAI_API_KEY="k"\n'
            f'LISTEN="127.0.0.1"\nPORT="{port}"\nPROMPT_PREFIX_FILE="{ROOT}/PROMPT_PREFIX.txt"\nTRANS_CACHE_TYPE="text"\n'
            f'TRANS_CACHE_FILE="{d}/c.jsonl"\nTRANS_CACHE_THRESHOLD="1"\nSTREAM="{"true" if stream else "false"}"\n')
    if setting is not None:
        conf += f'NORMAL_UNESCAPE="{setting}"\n'
    (d / "t.conf").write_text(conf)
    log = open(d / "log", "wb")
    p = subprocess.Popen([binary, "-c", str(d / "t.conf"), "-r", "r.txt"], cwd=d, stdout=log, stderr=subprocess.STDOUT)
    for _ in range(200):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.02)
    out = []
    for text in requests:
        body = json.dumps({"timestamp": "2026-11-07T01:02:03Z", "uuid": "550e8400-e29b-41d4-a716-446655440000",
                           "from": "kor", "to": "eng", "text": text}).encode()
        with socket.create_connection(("127.0.0.1", port), timeout=30) as c:
            c.sendall(b"POST /translate HTTP/1.0\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
            data = b""
            while True:
                x = c.recv(65536)
                if not x:
                    break
                data += x
        status = int(data.split(b" ")[1])
        j = json.loads(data.split(b"\r\n\r\n", 1)[1])
        out.append((status, j.get("translatedText")))
    p.terminate()
    p.wait(timeout=20)
    log.close()
    return out, (d / "log").read_text(errors="replace")


def main():
    binary = str(Path(sys.argv[1]).resolve())
    yes = ref.strip(ref.unescape(TEXT.encode())).decode()
    no = ref.strip(TEXT.encode()).decode()
    assert yes != no
    tabs = REPLIES["spaces"].encode()
    assert ref.strip(ref.unescape(tabs)) == b""            # nothing left under "yes"
    tabs_no = ref.strip(tabs).decode()                      # the text as sent under "no"
    with tempfile.TemporaryDirectory(prefix="unesc-") as w:
        w = Path(w)
        for setting, want, warn in ((None, yes, False), ("yes", yes, False), ("no", no, False), ("NO", no, False),
                                    ("0", no, False), ("maybe", yes, True)):
            for stream in (False, True):
                d = Path(tempfile.mkdtemp(dir=w))
                key = "sse" if stream else "plain"
                got, log = run(binary, d, setting, stream, [f"CASE_{key} 원문", "CASE_spaces 공백"])
                cached = (d / "c.jsonl").read_text(encoding="utf-8")
                want_tabs = (200, tabs_no) if want == no else (502, None)
                ok = got[0] == (200, want) and got[1] == want_tabs and json.dumps(want, ensure_ascii=False)[1:-1] in cached
                ok = ok and (("Warning: Invalid NORMAL_UNESCAPE 'maybe', using 'yes'" in log) == warn)
                check(ok, f"NORMAL_UNESCAPE={setting!s:6} {'SSE ' if stream else 'JSON'}: reply and cache = "
                          f"{'strip(unescape(x))' if want == yes else 'strip(x)'}, escaped tabs -> {got[1][0]}")
        # a translation cached under "yes" is still served after switching to "no"
        d = Path(tempfile.mkdtemp(dir=w))
        run(binary, d, "yes", False, ["CASE_plain 같은 원문"])
        calls = UP.calls
        got, _ = run(binary, d, "no", False, ["CASE_plain 같은 원문"])
        check(got[0] == (200, yes) and UP.calls == calls,
              "a translation cached under yes is served unchanged after a restart with no (no upstream call)")
    print("OK" if not FAILURES else f"FAILED: {len(FAILURES)}")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
