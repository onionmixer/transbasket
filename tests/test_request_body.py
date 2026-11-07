"""V1 of PLAN_OUT_OF_SCOPE.md: the /translate request body.

    python3 -I tests/test_request_body.py <transbasket> [<pre-fix transbasket>]

- 1 MiB limit at the exact byte, with Content-Length and with chunked encoding
  (limit: 200; limit + 1: 413);
- "Expect: 100-continue" with a Content-Length over the limit: 413 right away, no
  "100 Continue", nothing uploaded; under the limit: 100 then 200;
- an over-limit body without Expect is drained and answered 413 on the same
  connection, which then serves the next request (keep-alive);
- memory: 64 MiB bodies, one and four at once, are 413 and grow the server's
  peak RSS by only a few MiB (the old code kept them: 160 MB for one, ~4 s);
- time: a body just under the limit sent in 512-byte writes is answered within
  2 s - a sanity bound only: below 1 MiB the old accumulation was fast too
  (measured 2026-11-07), its superlinear cost showed from 16 MiB on, which the
  limit now refuses. Socket write sizes do not set MHD's callback chunk sizes;
- a raw NUL byte anywhere is 422 (after the root value it used to be 200); over
  the limit 413 wins;
- 413 and 422 reach neither the upstream nor the cache: the refused bodies carry
  source texts never sent before; the upstream call count is compared before
  and after, and none of those texts is in the cache the server saves at exit;
- the same valid body in writes of 1 byte .. 64 KiB gives the same answer;
- an upload cut off midway leaves the server healthy.
Servers bind 127.0.0.1; the upstream is an offline fake.
"""
import json
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIMIT = 1024 * 1024
FAILURES = []


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
                self.rfile.read(int(self.headers["Content-Length"]))
                outer.calls += 1
                b = b'{"choices":[{"message":{"content":"ok"}}]}'
                self.send_response(200)
                self.send_header("Content-Length", str(len(b)))
                self.end_headers()
                self.wfile.write(b)

            def log_message(self, *a):
                pass
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()


UP = Upstream()


class Server:
    def __init__(self, binary):
        self.d = Path(tempfile.mkdtemp(prefix="body-"))
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        self.port = s.getsockname()[1]
        s.close()
        (self.d / "r.txt").write_text("role")
        (self.d / "t.conf").write_text(
            f'OPENAI_BASE_URL="http://127.0.0.1:{UP.server.server_port}/v1"\nOPENAI_MODEL="m"\nOPENAI_API_KEY="k"\n'
            f'LISTEN="127.0.0.1"\nPORT="{self.port}"\nPROMPT_PREFIX_FILE="{ROOT}/PROMPT_PREFIX.txt"\n'
            f'TRANS_CACHE_TYPE="text"\nTRANS_CACHE_FILE="{self.d}/c.jsonl"\nTRANS_CACHE_THRESHOLD="1"\n')
        self.p = subprocess.Popen([binary, "-c", str(self.d / "t.conf"), "-r", "r.txt"], cwd=self.d,
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(200):
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.02)

    def hwm(self):
        return int(re.search(r"VmHWM:\s+(\d+)", open(f"/proc/{self.p.pid}/status").read()).group(1))

    def healthy(self):
        try:
            with socket.create_connection(("127.0.0.1", self.port), timeout=2) as c:
                c.sendall(b"GET /health HTTP/1.0\r\n\r\n")
                return b" 200 " in c.recv(100)
        except OSError:
            return False

    def stop(self):
        self.p.terminate()
        self.p.wait(timeout=20)


def read_response(c):
    """One HTTP/1.1 response with Content-Length from a socket; (status, headers, body)."""
    data = b""
    while b"\r\n\r\n" not in data:
        x = c.recv(65536)
        if not x:
            break
        data += x
    head, _, rest = data.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    if not lines or not lines[0]:
        return None, {}, b""
    status = int(lines[0].split()[1])
    hdr = {l.split(b":", 1)[0].strip().lower(): l.split(b":", 1)[1].strip() for l in lines[1:] if b":" in l}
    n = int(hdr.get(b"content-length", b"0"))
    while len(rest) < n:
        x = c.recv(65536)
        if not x:
            break
        rest += x
    return status, hdr, rest[:n]


def body_of(size, text="가나다"):
    """A valid request padded to exactly `size` bytes (python counts)."""
    base = {"timestamp": "2026-11-07T01:02:03Z", "uuid": "550e8400-e29b-41d4-a716-446655440000",
            "from": "kor", "to": "eng", "text": text}
    empty = json.dumps({**base, "pad": ""}, ensure_ascii=False).encode()
    pad = size - len(empty)
    assert pad >= 0
    b = json.dumps({**base, "pad": "x" * pad}, ensure_ascii=False).encode()
    assert len(b) == size
    return b


def request_head(length=None, chunked=False, expect=False, close=True):
    h = b"POST /translate HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
    if chunked:
        h += b"Transfer-Encoding: chunked\r\n"
    if length is not None:
        h += b"Content-Length: %d\r\n" % length
    if expect:
        h += b"Expect: 100-continue\r\n"
    h += b"Connection: close\r\n" if close else b""
    return h + b"\r\n"


def chunked(body, piece=65536):
    return b"".join(b"%x\r\n%s\r\n" % (len(body[i:i + piece]), body[i:i + piece]) for i in range(0, len(body), piece)) + b"0\r\n\r\n"


def send(port, body, chunk=False, piece=65536, close=True):
    with socket.create_connection(("127.0.0.1", port), timeout=60) as c:
        if chunk:
            c.sendall(request_head(chunked=True, close=close))
            payload = chunked(body, piece)
        else:
            c.sendall(request_head(len(body), close=close))
            payload = body
        try:
            for i in range(0, len(payload), max(piece, 1)):
                c.sendall(payload[i:i + max(piece, 1)])
        except (BrokenPipeError, ConnectionResetError):
            pass
        return read_response(c)


def main():
    binary = str(Path(sys.argv[1]).resolve())
    old = str(Path(sys.argv[2]).resolve()) if len(sys.argv) > 2 else None
    s = Server(binary)
    res = {}
    for name, size in (("limit - 1", LIMIT - 1), ("limit", LIMIT), ("limit + 1", LIMIT + 1)):
        for mode in ("Content-Length", "chunked"):
            res[(name, mode)] = send(s.port, body_of(size), chunk=(mode == "chunked"))[0]
    check(all(res[(n, m)] == 200 for n in ("limit - 1", "limit") for m in ("Content-Length", "chunked")) and
          res[("limit + 1", "Content-Length")] == 413 and res[("limit + 1", "chunked")] == 413,
          f"1 MiB limit at the byte, Content-Length and chunked {res}")

    with socket.create_connection(("127.0.0.1", s.port), timeout=10) as c:
        c.sendall(request_head(LIMIT + 1, expect=True))
        st, _, body = read_response(c)
        c.settimeout(2)
        try:
            tail = c.recv(100)
        except (socket.timeout, ConnectionResetError):
            tail = b"?"
    check(st == 413 and json.loads(body).get("errorCode") == "PAYLOAD_TOO_LARGE" and tail == b"",
          f"Expect: 100-continue over the limit: 413 at once, no 100, connection closed ({st}, tail {tail!r})")
    with socket.create_connection(("127.0.0.1", s.port), timeout=10) as c:
        ok = body_of(300)
        c.sendall(request_head(len(ok), expect=True))
        first = c.recv(100)
        c.sendall(ok)
        st, _, _ = read_response(c)
    check(first.startswith(b"HTTP/1.1 100") and st == 200, f"Expect: 100-continue under the limit: 100 then 200 ({first[:20]!r}, {st})")

    with socket.create_connection(("127.0.0.1", s.port), timeout=30) as c:
        big = body_of(LIMIT + 1)
        c.sendall(request_head(chunked=True, close=False) + chunked(big))
        st1, _, _ = read_response(c)
        small = body_of(300)
        c.sendall(request_head(len(small), close=False) + small)
        st2, _, _ = read_response(c)
        c.sendall(request_head(len(big), close=False) + big)
        st3, _, _ = read_response(c)
        c.sendall(request_head(len(small), close=False) + small)
        st4, _, _ = read_response(c)
    check([st1, st2, st3, st4] == [413, 200, 413, 200],
          f"over-limit bodies are drained and answered 413, the connection serves the next request ({[st1, st2, st3, st4]})")

    base = s.hwm()
    big_status = [send(s.port, b"x" * (64 << 20), piece=1 << 20)[0]]
    one = s.hwm() - base

    def big(_):
        big_status.append(send(s.port, b"y" * (64 << 20), piece=1 << 20)[0])
    th = [threading.Thread(target=big, args=(i,)) for i in range(4)]
    [t.start() for t in th]
    [t.join() for t in th]
    four = s.hwm() - base
    check(big_status == [413] * 5 and one < 8 * 1024 and four < 16 * 1024,
          f"64 MiB bodies: {big_status}, peak RSS +{one} KiB (one), +{four} KiB (four at once)")

    t0 = time.time()
    st, _, _ = send(s.port, body_of(LIMIT - 1), piece=512)
    dt = time.time() - t0
    check(st == 200 and dt < 2.0, f"a body just under the limit in 512-byte writes: {st} in {dt:.2f} s")

    # refused bodies: texts never sent before, so a parse would mean a new upstream call
    calls0 = UP.calls
    fresh = {k: body_of(300, f"가나다 새 원문 {i}") for i, k in enumerate(("after the root", "inside a string", "first byte"))}
    nul = {"after the root": fresh["after the root"] + b"\x00junk",
           "inside a string": fresh["inside a string"].replace(b"\xea\xb0\x80", b"\x00", 1),
           "first byte": b"\x00" + fresh["first byte"]}
    assert nul["inside a string"] != fresh["inside a string"]
    got = {k: send(s.port, v)[0] for k, v in nul.items()}
    big_nul = body_of(LIMIT + 1, "가나다 새 원문 큰 본문")
    big_nul = big_nul[:LIMIT] + b"\x00" + big_nul[LIMIT + 1:]
    got["beyond the limit"] = send(s.port, big_nul)[0]
    got["over the limit"] = send(s.port, body_of(LIMIT + 1, "가나다 새 원문 상한"))[0]
    got["over the limit, chunked"] = send(s.port, body_of(LIMIT + 1, "가나다 새 원문 chunked"), chunk=True)[0]
    check(got == {"after the root": 422, "inside a string": 422, "first byte": 422, "beyond the limit": 413,
                  "over the limit": 413, "over the limit, chunked": 413},
          f"raw NUL -> 422, 413 wins over the limit {got}")
    upstream_after_refused = UP.calls - calls0
    ok = body_of(300)

    answers = set()
    for piece in (1, 7, 512, 65536):
        st, _, b = send(s.port, ok, piece=piece)
        answers.add((st, json.loads(b).get("translatedText")))
    check(answers == {(200, "ok")}, f"same valid body in pieces of 1 .. 64 KiB: same answer {answers}")

    with socket.create_connection(("127.0.0.1", s.port), timeout=10) as c:
        c.sendall(request_head(LIMIT // 2) + b"x" * 1000)
    time.sleep(0.3)
    check(s.healthy(), "an upload cut off midway leaves the server healthy")
    s.stop()
    saved = [json.loads(line) for line in (s.d / "c.jsonl").read_text(encoding="utf-8").splitlines() if line.strip()]
    texts = {v for e in saved for v in e.values() if isinstance(v, str)}
    check(saved and "가나다" in texts, f"the cache was saved at exit ({len(saved)} entries)")
    check(upstream_after_refused == 0 and not any("새 원문" in t for t in texts),
          f"413 and 422 reach neither the upstream ({upstream_after_refused} calls) nor the cache")

    if old:
        o = Server(old)
        st_nul = send(o.port, ok + b"\x00junk")[0]
        st_big = send(o.port, body_of(LIMIT + 1))[0]
        t0 = time.time()
        send(o.port, body_of(LIMIT - 1), piece=512)
        dt = time.time() - t0          # before stop(): shutdown itself takes seconds
        o.stop()
        print(f"  info  pre-fix binary: JSON + NUL + junk -> {st_nul}, limit + 1 -> {st_big}, "
              f"just-under-limit in 512-byte writes {dt:.2f} s")
    print("OK" if not FAILURES else f"FAILED: {len(FAILURES)}")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
