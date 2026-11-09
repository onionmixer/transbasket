"""HTTP_MAX_CONNECTIONS through the real server (PLAN_CONNECTION_LIMIT.md 3, 4).

    python3 -I tests/test_connection_limit.py [transbasket binary]

Offline: a local fake OpenAI upstream that holds chosen requests until released.
Servers bind loopback only. Over the limit, connections wait in the listen
backlog (no 503); "limit reached" / "below the limit" log lines pair up and are
rate limited to one report per 60 s (one case waits for that, ~65 s).
Server output goes to a file, never to an unread pipe.
"""
import json
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import http.server
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else str(ROOT / "transbasket")
MAX = 1020
REACHED = re.compile(r"HTTP connection limit (\d+) reached; new connections wait in the listen backlog"
                     r"(?: \((\d+) more times since the last report\))?$")
BELOW = re.compile(r"HTTP connection count below the limit again after (\d+)\.(\d) s( \(during shutdown\))?$")
failures = 0


def check(cond, what):
    global failures
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        failures += 1
    return cond


class Upstream:
    """Fake /v1/chat/completions. Texts starting with "hold" wait for release();
    texts starting with "slow" take `slow_s`; everything else answers at once."""

    def __init__(self):
        self.gate = threading.Event()
        self.lock = threading.Lock()
        self.arrived = []
        self.slow_s = 1.0
        up = self

        class H(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                text = body["messages"][-1]["content"]
                with up.lock:
                    up.arrived.append((time.monotonic(), text))
                if "hold" in text:
                    up.gate.wait(60)
                elif "slow" in text:
                    time.sleep(up.slow_s)
                out = json.dumps({"choices": [{"message": {"content": "T:" + text[-12:]}}]}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(out)))
                self.end_headers()
                self.wfile.write(out)

            def log_message(self, *a):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.server.daemon_threads = True
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def held(self):
        with self.lock:
            return sum(1 for _, t in self.arrived if "hold" in t)

    def reset(self):
        self.gate.clear()
        with self.lock:
            self.arrived.clear()


UP = Upstream()


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def body(text, uuid_tail="000"):
    return json.dumps({"timestamp": "2026-11-09T01:02:03Z", "uuid": f"550e8400-e29b-41d4-a716-446655440{uuid_tail}",
                       "from": "kor", "to": "eng", "text": text}).encode()


def translate_raw(text):
    b = body(text)
    return (b"POST /translate HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Type: application/json\r\n"
            b"Content-Length: %d\r\n\r\n" % len(b) + b)


HEALTH = b"GET /health HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"


def keepalive_raw(text):
    """POST /translate without Connection: close. Answered on the last call, so the
    connection stays open; /health is answered on the first call, which makes
    libmicrohttpd close it (discard_request, connection.c keepalive_possible())."""
    b = body(text)
    return (b"POST /translate HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
            b"Content-Length: %d\r\n\r\n" % len(b) + b)


def exchange(host, port, raw, timeout=60, family=socket.AF_INET):
    """Send one request on a new connection, read to EOF. -> (status or error, body, seconds)."""
    t0 = time.monotonic()
    try:
        with socket.socket(family, socket.SOCK_STREAM) as c:
            c.settimeout(timeout)
            c.connect((host, port))
            c.sendall(raw)
            data = b""
            while True:
                x = c.recv(65536)
                if not x:
                    break
                data += x
        if not data:
            return "empty", b"", time.monotonic() - t0
        return data[9:12].decode(), data.split(b"\r\n\r\n", 1)[-1], time.monotonic() - t0
    except Exception as e:  # noqa: BLE001 - report the kind
        return type(e).__name__, b"", time.monotonic() - t0


class Async:
    """exchange() on a thread; .done tells whether a response came back yet."""

    def __init__(self, *args, **kw):
        self.result = None
        self.t = threading.Thread(target=self._run, args=args, kwargs=kw, daemon=True)
        self.t.start()

    def _run(self, *args, **kw):
        self.result = exchange(*args, **kw)

    @property
    def done(self):
        return self.result is not None

    def wait(self, secs=60):
        self.t.join(secs)
        return self.result


class Server:
    def __init__(self, d, extra="", listen="127.0.0.1", args=(), nofile=None, mode="normal"):
        self.d = d
        self.port = free_port()
        (d / "r.txt").write_text("role")
        conf = (f'LISTEN="{listen}"\nPORT="{self.port}"\nPROMPT_PREFIX_FILE="{ROOT}/PROMPT_PREFIX.txt"\n'
                f'LANGUAGE_TABLE_FILE="{ROOT}/language_tables.conf"\nTRANS_CACHE_TYPE="text"\n'
                f'TRANS_CACHE_FILE="{d}/c.jsonl"\nTRANS_CACHE_THRESHOLD="1"\nTRANS_CACHE_CLEANUP_ENABLED="false"\n')
        if mode == "normal":
            conf += (f'OPENAI_BASE_URL="http://127.0.0.1:{UP.server.server_port}/v1"\nOPENAI_MODEL="m"\n'
                     f'OPENAI_API_KEY="k"\n')
        else:
            conf += f'MODEL_TYPE="codex"\nCODEX_BIN="/bin/false"\nCODEX_RUN_DIR="{d}/run"\n'
        (d / "t.conf").write_text(conf + extra)
        self.log_path = d / "server.log"
        self.out = open(self.log_path, "wb")
        cmd = [BINARY, "-c", str(d / "t.conf"), "-r", "r.txt", *args]
        if nofile:
            cmd = ["bash", "-c", f'ulimit -n {nofile}; exec "$@"', "bash", *cmd]
        self.p = subprocess.Popen(cmd, cwd=d, stdout=self.out, stderr=subprocess.STDOUT)

    def up(self, secs=10):
        end = time.time() + secs
        while time.time() < end:
            if self.p.poll() is not None:
                return False
            if "HTTP connection limit:" in self.log():
                return True
            time.sleep(0.05)
        return False

    def log(self):
        return self.log_path.read_text(errors="replace")

    def lines(self, rx):
        out = []
        for l in self.log().splitlines():
            m = rx.search(l)
            if m:
                out.append(m)
        return out

    def stop(self):
        if self.p.poll() is None:
            self.p.send_signal(signal.SIGTERM)
            try:
                self.p.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()
        self.out.close()


def wait_for(cond, secs):
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.02)
    return cond()


def bound(port):
    try:
        socket.create_connection(("127.0.0.1", port), timeout=0.3).close()
        return True
    except OSError:
        return False


def tmp():
    return tempfile.TemporaryDirectory(prefix="connlimit-")


# --------------------------------------------------------------------------- cases

def unset():
    print("unset: libmicrohttpd's default, nothing limited at 6")
    UP.reset()
    with tmp() as w:
        s = Server(Path(w))
        try:
            check(s.up(), "server up")
            check("HTTP connection limit: 1020 (libmicrohttpd default)" in s.log(), "start line names the default")
            reqs = [Async("127.0.0.1", s.port, translate_raw(f"hold unset {i}")) for i in range(6)]
            check(wait_for(lambda: UP.held() == 6, 10), f"6 translations reached the upstream together ({UP.held()})")
            UP.gate.set()
            res = [r.wait() for r in reqs]
            check(all(r[0] == "200" for r in res), f"all 200 ({[r[0] for r in res]})")
            check(not s.lines(REACHED) and not s.lines(BELOW), "no limit lines")
        finally:
            UP.gate.set()
            s.stop()


def saturate_two(s, tag):
    """Two held translations on two connections; returns them once both reached the upstream."""
    UP.reset()
    reqs = [Async("127.0.0.1", s.port, translate_raw(f"hold {tag} {i}")) for i in range(2)]
    ok = wait_for(lambda: UP.held() == 2, 10)
    return reqs, ok


def core():
    print("N=2: the third connection waits, then gets its answer; reached/below pair")
    with tmp() as w:
        s = Server(Path(w), 'HTTP_MAX_CONNECTIONS="2"\n')
        try:
            check(s.up(), "server up")
            check("HTTP connection limit: 2 (HTTP_MAX_CONNECTIONS)" in s.log(), "start line names the setting")
            t_first = time.monotonic()
            UP.reset()
            reqs = [Async("127.0.0.1", s.port, translate_raw("hold core 0"))]
            check(wait_for(lambda: UP.held() == 1, 10), "1 translation held at the upstream")
            time.sleep(0.5)
            check(not s.lines(REACHED), "one slot of 2 taken: no 'reached' line yet")
            reqs.append(Async("127.0.0.1", s.port, translate_raw("hold core 1")))
            ok = wait_for(lambda: UP.held() == 2, 10)
            check(ok, "2 translations held at the upstream")
            t_full = time.monotonic()
            check(wait_for(lambda: len(s.lines(REACHED)) == 1, 3), "one 'reached' line once both slots are taken")
            h = Async("127.0.0.1", s.port, HEALTH)
            time.sleep(1.5)
            check(not h.done, "a third connection (/health) gets no answer while both slots are taken")
            check(UP.held() == 2, "and no third request reached the upstream")
            t_rel = time.monotonic()
            UP.gate.set()
            hr = h.wait(10)
            res = [r.wait(10) for r in reqs]
            t_end = time.monotonic()
            check(hr is not None and hr[0] == "200", f"the waiting /health itself then answers 200 ({hr and hr[0]})")
            check(all(r and r[0] == "200" for r in res), "both translations 200")
            check(wait_for(lambda: len(s.lines(BELOW)) == 1, 5), "one 'below' line")
            r = s.lines(REACHED)
            b = s.lines(BELOW)
            if r and b:
                check(r[0].group(1) == "2" and r[0].group(2) is None, "reached line: limit 2, no suppressed count")
                x = int(b[0].group(1)) + int(b[0].group(2)) / 10
                lo, hi = (t_rel - t_full) - 0.15, (t_end - t_first) + 0.5
                check(lo <= x <= hi and b[0].group(3) is None,
                      f"below after {x} s, inside [{lo:.2f}, {hi:.2f}] (held to released .. first connect to last answer)")
                log = s.log()
                check(log.index(r[0].group(0)) < log.index(b[0].group(0)), "reached comes before below")
            n = len(UP.arrived)
            h2 = exchange("127.0.0.1", s.port, HEALTH, timeout=5)
            check(h2[0] == "200" and h2[2] < 1.0, f"below the limit again: /health at once ({h2[0]}, {h2[2]:.2f} s)")
            check(len(UP.arrived) == n, "nothing else went upstream")
        finally:
            UP.gate.set()
            s.stop()


def cache_hit():
    print("N=2: a cache hit waits for a slot too")
    with tmp() as w:
        s = Server(Path(w), 'HTTP_MAX_CONNECTIONS="2"\n')
        try:
            check(s.up(), "server up")
            UP.reset()
            r0 = exchange("127.0.0.1", s.port, translate_raw("cached sentence"), timeout=10)
            check(r0[0] == "200", f"primed the cache ({r0[0]})")
            reqs, ok = saturate_two(s, "cache")
            check(ok, "2 translations held")
            n = len(UP.arrived)
            c = Async("127.0.0.1", s.port, translate_raw("cached sentence"))
            time.sleep(1.5)
            check(not c.done, "the cache hit gets no answer while both slots are taken")
            UP.gate.set()
            cr = c.wait(10)
            check(cr is not None and cr[0] == "200" and b"T:" in cr[1], f"then 200 from the cache ({cr and cr[0]})")
            check(len(UP.arrived) == n, "the cache hit did not go upstream")
            [r.wait(10) for r in reqs]
        finally:
            UP.gate.set()
            s.stop()


def read_one_response(c):
    """One HTTP/1.1 response with Content-Length on a keep-alive socket."""
    data = b""
    while b"\r\n\r\n" not in data:
        x = c.recv(65536)
        if not x:
            return None
        data += x
    head, rest = data.split(b"\r\n\r\n", 1)
    m = re.search(rb"(?i)content-length:\s*(\d+)", head)
    need = int(m.group(1)) if m else 0
    while len(rest) < need:
        x = c.recv(65536)
        if not x:
            break
        rest += x
    return head[9:12].decode()


def keepalive_and_early_close():
    print("N=1: an idle keep-alive connection holds the slot; one closed before its first request does not")
    with tmp() as w:
        s = Server(Path(w), 'HTTP_MAX_CONNECTIONS="1"\n')
        try:
            check(s.up(), "server up")
            time.sleep(0.3)
            a = socket.create_connection(("127.0.0.1", s.port), timeout=10)
            a.sendall(keepalive_raw("keepalive sentence"))
            check(read_one_response(a) == "200", "keep-alive translation answered")
            a.settimeout(0.5)
            try:
                still_open = a.recv(1) != b""
            except socket.timeout:
                still_open = True
            a.settimeout(10)
            check(still_open, "and the server kept that connection open")
            b = Async("127.0.0.1", s.port, HEALTH)
            time.sleep(1.5)
            check(not b.done, "another connection waits while the idle keep-alive one holds the slot")
            a.close()
            br = b.wait(10)
            check(br is not None and br[0] == "200", f"closing the idle one lets it through ({br and br[0]})")
            for i in range(5):
                socket.create_connection(("127.0.0.1", s.port), timeout=5).close()
                r = exchange("127.0.0.1", s.port, HEALTH, timeout=5)
                if not check(r[0] == "200" and r[2] < 3, f"connect+close without a request #{i + 1}, then /health "
                                                          f"({r[0]}, {r[2]:.2f} s)"):
                    break
        finally:
            s.stop()


def dual_stack():
    print("LISTEN=:: with N=2: IPv4 and IPv6 share one limit")
    try:
        with socket.socket(socket.AF_INET6) as t:
            t.bind(("::1", 0))
    except OSError:
        print("  skip  no IPv6 loopback")
        return
    with tmp() as w:
        s = Server(Path(w), 'HTTP_MAX_CONNECTIONS="2"\n', listen="::")
        try:
            check(s.up(), "server up")
            UP.reset()
            r4 = Async("127.0.0.1", s.port, translate_raw("hold v4"))
            r6 = Async("::1", s.port, translate_raw("hold v6"), family=socket.AF_INET6)
            check(wait_for(lambda: UP.held() == 2, 10), "one held over IPv4, one over IPv6")
            h = Async("127.0.0.1", s.port, HEALTH)
            h6 = Async("::1", s.port, HEALTH, family=socket.AF_INET6)
            time.sleep(1.5)
            check(not h.done and not h6.done, "a third connection waits on either family")
            UP.gate.set()
            check(all(x.wait(10) and x.result[0] == "200" for x in (r4, r6, h, h6)), "all four answer 200 after release")
        finally:
            UP.gate.set()
            s.stop()


def validation():
    print("values: invalid ones stop startup (both backends, foreground and -d), valid ones start")
    msg = f"Error: HTTP_MAX_CONNECTIONS must be an integer from 1 to {MAX}"
    bad = ['"0"', '"-1"', '"+1"', '"abc"', '" 1"', '"1 "', f'"{MAX + 1}"', '"99999999999"', "", '""', '"1.5"', '"0x10"']
    for v in bad:
        with tmp() as w:
            s = Server(Path(w), f"HTTP_MAX_CONNECTIONS={v}\n")
            try:
                rc = s.p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                rc = None
            check(rc == 1 and msg in s.log() and not bound(s.port), f"HTTP_MAX_CONNECTIONS={v}: exit 1, message, not bound")
            s.stop()
    for v, shown in (('"1"', 1), (f'"{MAX}"', MAX), ('"0010"', 10), ("7", 7)):
        with tmp() as w:
            s = Server(Path(w), f"HTTP_MAX_CONNECTIONS={v}\n")
            try:
                ok = s.up() and f"HTTP connection limit: {shown} (HTTP_MAX_CONNECTIONS)" in s.log()
                check(ok and exchange("127.0.0.1", s.port, HEALTH, timeout=5)[0] == "200",
                      f"HTTP_MAX_CONNECTIONS={v}: starts with limit {shown}")
            finally:
                s.stop()
    for lines, ok in (('HTTP_MAX_CONNECTIONS="0"\nHTTP_MAX_CONNECTIONS="5"\n', True),
                      ('HTTP_MAX_CONNECTIONS="5"\nHTTP_MAX_CONNECTIONS="0"\n', False)):
        with tmp() as w:
            s = Server(Path(w), lines)
            try:
                started = s.up(5)
                check(started == ok, f"the last of two lines wins ({lines.strip().replace(chr(10), ' then ')})"
                                     f" -> {'starts' if ok else 'fails'}")
            finally:
                s.stop()
    with tmp() as w:
        s = Server(Path(w), 'HTTP_MAX_CONNECTIONS="0"\n', mode="codex")
        try:
            rc = s.p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            rc = None
        check(rc == 1 and msg in s.log(), "codex backend: same check (exit 1, message)")
        s.stop()
    with tmp() as w:
        d = Path(w)
        s = Server(d, 'HTTP_MAX_CONNECTIONS="0"\n', args=("-d", "-l", str(d / "daemon.log")))
        try:
            rc = s.p.wait(timeout=20)
        except subprocess.TimeoutExpired:
            rc = None
        dl = (d / "daemon.log").read_text(errors="replace") if (d / "daemon.log").exists() else ""
        check(rc == 1 and not bound(s.port) and (msg in dl or msg in s.log()),
              f"-d with an invalid value: launcher exit 1, nothing bound ({rc})")
        s.stop()
    with tmp() as w:
        d = Path(w)
        s = Server(d, 'HTTP_MAX_CONNECTIONS="3"\n', args=("-d", "-l", str(d / "daemon.log")))
        try:
            rc = s.p.wait(timeout=20)
        except subprocess.TimeoutExpired:
            rc = None
        dl = (d / "daemon.log").read_text(errors="replace") if (d / "daemon.log").exists() else ""
        check(rc == 0 and "HTTP connection limit: 3 (HTTP_MAX_CONNECTIONS)" in dl,
              f"-d with a valid value: launcher exit 0, start line in the -l file ({rc})")
        pids = [int(p) for p in os.listdir("/proc") if p.isdigit() and _cmd_has(p, str(d / "t.conf"))]
        for pid in pids:
            os.kill(pid, signal.SIGTERM)
        check(len(pids) == 1 and wait_for(lambda: not os.path.exists(f"/proc/{pids[0]}"), 20),
              f"the daemon (pid {pids}) stopped")
        s.stop()


def _cmd_has(pid, needle):
    try:
        return needle in open(f"/proc/{pid}/cmdline", "rb").read().decode(errors="replace")
    except OSError:
        return False


def rate_limit():
    print("N=2: one report per 60 s, the suppressed count, pairs only for reported episodes (~65 s)")
    with tmp() as w:
        s = Server(Path(w), 'HTTP_MAX_CONNECTIONS="2"\n')
        try:
            check(s.up(), "server up")
            time.sleep(0.3)

            def episode():
                socks = []
                for _ in range(2):
                    c = socket.create_connection(("127.0.0.1", s.port), timeout=10)
                    c.sendall(keepalive_raw("episode sentence"))
                    socks.append(c)
                ok = all(read_one_response(c) == "200" for c in socks)   # both accepted (STARTED ran)
                for c in socks:
                    c.close()
                return ok

            check(episode(), "episode 1: two keep-alive connections answered")
            t_first = time.monotonic()
            check(wait_for(lambda: len(s.lines(BELOW)) == 1, 5) and len(s.lines(REACHED)) == 1,
                  "episode 1 reported: one reached, one below")
            time.sleep(2)
            check(episode(), "episode 2 (within 60 s)")
            time.sleep(2)
            check(len(s.lines(REACHED)) == 1 and len(s.lines(BELOW)) == 1, "episode 2: no lines")
            time.sleep(max(0.0, t_first + 61 - time.monotonic()))
            check(episode(), "episode 3 (61 s after the first report)")
            check(wait_for(lambda: len(s.lines(BELOW)) == 2, 5), "episode 3: its below line")
            r = s.lines(REACHED)
            check(len(r) == 2 and r[1].group(2) == "1", f"episode 3 reached line carries '(1 more times ...)' "
                                                        f"({[m.group(0) for m in r]})")
        finally:
            s.stop()


def shutdown_at_limit():
    print("N=2: SIGTERM while at the limit -> below line says 'during shutdown'")
    with tmp() as w:
        s = Server(Path(w), 'HTTP_MAX_CONNECTIONS="2"\n')
        try:
            check(s.up(), "server up")
            reqs, ok = saturate_two(s, "stop")
            check(ok and wait_for(lambda: len(s.lines(REACHED)) == 1, 3), "at the limit")
            s.p.send_signal(signal.SIGTERM)
            time.sleep(0.5)
            UP.gate.set()
            try:
                rc = s.p.wait(timeout=30)
            except subprocess.TimeoutExpired:
                rc = None
            b = s.lines(BELOW)
            check(rc == 0 and len(b) == 1 and b[0].group(3) == " (during shutdown)",
                  f"exit 0, one below line marked during shutdown ({rc}, {[m.group(0) for m in b]})")
            [r.wait(10) for r in reqs]
        finally:
            UP.gate.set()
            s.stop()


def fd_budget():
    print("ulimit -n 64: a limit under the fd budget turns upstream-socket failures (503) into waiting")
    results = {}
    for name in ("unset", "limited"):
        with tmp() as w:
            d = Path(w)
            UP.reset()
            UP.gate.set()
            probe = Server(d, 'INFLIGHT_MAX_KEYS="1000"\n', nofile=64)
            ok = probe.up()
            base = len(os.listdir(f"/proc/{probe.p.pid}/fd")) if ok else 0
            probe.stop()
            # each translating normal connection holds 4 descriptors: its client socket, the upstream
            # socket and the socketpair libcurl 7.81 opens for every transfer (measured 2026-11-09: peak
            # 53 = 5 + 12 x 4 at a limit of 12; a model of 2 per connection let 503s through). 8 spare.
            n = (64 - base - 8) // 4
            extra = 'INFLIGHT_MAX_KEYS="1000"\n' + (f'HTTP_MAX_CONNECTIONS="{n}"\n' if name == "limited" else "")
            (d / "c.jsonl").unlink(missing_ok=True)
            s = Server(d, extra, nofile=64)
            try:
                check(s.up(), f"{name}: server up (startup fds {base}, limit {n if name == 'limited' else 'default'})")
                reqs = [Async("127.0.0.1", s.port, translate_raw(f"slow fd {name} {i}"), timeout=90) for i in range(80)]
                res = [r.wait(120) for r in reqs]
                results[name] = [r[0] if r else "none" for r in res]
            finally:
                s.stop()
    from collections import Counter
    u, l = Counter(results.get("unset", [])), Counter(results.get("limited", []))
    print(f"  info  unset {dict(u)}, limited {dict(l)}")
    check(u.get("503", 0) > 0, "baseline (unset) really runs out of descriptors: some 503")
    check(l.get("200", 0) == 80, "with the limit: all 80 answered 200")


def main():
    if not os.access(BINARY, os.X_OK):
        print(f"no binary {BINARY}")
        return 2
    for case in (unset, core, cache_hit, keepalive_and_early_close, dual_stack, validation, shutdown_at_limit,
                 fd_budget, rate_limit):
        case()
    print("OK" if not failures else f"FAILED ({failures})")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
