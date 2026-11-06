"""V3 of PLAN_LISTEN_ADDRESS.md: LISTEN through the real server.

    python3 -I tests/test_listen_address.py <transbasket> [--old <pre-fix transbasket>]

The driver re-executes itself inside an unprivileged network + PID namespace
(unshare -rn --pid --fork --kill-child=KILL) with lo and a dummy interface d0 =
198.18.0.1/24, so no test server is ever reachable from outside and every child
dies with the namespace. Without unshare only the loopback rows run.
Expected values are computed here from the rules in section 3.1; a plain python
socket bound the same way is measured first as the baseline for each row.
"""
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import listen_address_ref as ref  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
D0 = "198.18.0.1"
TARGETS = ["127.0.0.1", D0, "::1"]
INNER = "--inner"
FAILURES = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        FAILURES.append(what)


# ---- namespace --------------------------------------------------------------

def reexec_in_namespace(argv):
    if not shutil.which("unshare"):
        return None
    probe = subprocess.run(["unshare", "-rn", "--pid", "--fork", "--kill-child=KILL", "--mount-proc", "true"], capture_output=True)
    if probe.returncode != 0:
        print(f"unshare not usable here ({probe.stderr.decode(errors='replace').strip()}); loopback rows only")
        return None
    cmd = ["unshare", "-rn", "--pid", "--fork", "--kill-child=KILL", "--mount-proc", sys.executable, "-I", __file__, INNER] + argv
    return subprocess.run(cmd, timeout=900).returncode


def setup_interfaces():
    for c in (["ip", "link", "set", "lo", "up"], ["ip", "link", "add", "d0", "type", "dummy"],
              ["ip", "addr", "add", f"{D0}/24", "dev", "d0"], ["ip", "link", "set", "d0", "up"]):
        subprocess.run(c, check=True)
    time.sleep(0.2)


# ---- measurements -----------------------------------------------------------

def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def connects(target, port, http=False):
    fam = socket.AF_INET6 if ":" in target else socket.AF_INET
    try:
        with socket.socket(fam, socket.SOCK_STREAM) as s:
            s.settimeout(2)
            s.connect((target, port))
            if not http:
                return True
            s.sendall(b"GET /health HTTP/1.0\r\nHost: x\r\n\r\n")
            data = b""
            while b"\r\n" not in data:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
            return data.startswith(b"HTTP/1.") and b" 200 " in data.split(b"\r\n", 1)[0]
    except OSError:
        return False


def listening(port):
    out = subprocess.run(["ss", "-ltnH"], capture_output=True, text=True).stdout
    return sorted(line.split()[3] for line in out.splitlines() if line.split()[3].rsplit(":", 1)[1] == str(port))


def baseline_socket(addr, mode, port):
    if mode == ref.IPV4:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    else:
        s = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
        s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0 if mode == ref.DUAL_STACK else 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((addr, port))
    s.listen(16)
    return s


def capabilities():
    caps = {}
    try:
        socket.socket(socket.AF_INET6, socket.SOCK_STREAM).close()
        caps["inet6"] = True
    except OSError:
        caps["inet6"] = False
    for name, addr, mode in (("bind ::1", "::1", ref.IPV6_ONLY), ("dual ::", "::", ref.DUAL_STACK)):
        try:
            baseline_socket(addr, mode, 0).close()
            caps[name] = True
        except OSError:
            caps[name] = False
    return caps


def expected_reach(addr, mode, target):
    """Section 3.1: who can connect to a listener of this kind."""
    if ":" in target:
        return mode != ref.IPV4 and addr in ("::", target)
    if mode == ref.DUAL_STACK:
        return True
    return mode == ref.IPV4 and addr in ("0.0.0.0", target)


def expected_log(addr, mode, port):
    if mode == ref.IPV4:
        return f"HTTP server listening on {addr}:{port} (IPv4)"
    return f"HTTP server listening on [{addr}]:{port} ({'IPv4+IPv6' if mode == ref.DUAL_STACK else 'IPv6'})"


# ---- server -----------------------------------------------------------------

class Server:
    def __init__(self, binary, listen_lines, port):
        self.dir = Path(tempfile.mkdtemp(prefix="listen-"))
        (self.dir / "role.txt").write_text("role", encoding="utf-8")
        head = (f'OPENAI_BASE_URL="http://127.0.0.1:9/v1"\nOPENAI_MODEL="m"\nOPENAI_API_KEY="k"\nPORT="{port}"\n'
                f'PROMPT_PREFIX_FILE="{ROOT}/PROMPT_PREFIX.txt"\nLANGUAGE_TABLE_FILE="{ROOT}/language_tables.conf"\n'
                f'TRANS_CACHE_TYPE="text"\nTRANS_CACHE_FILE="{self.dir}/c.jsonl"\n').encode()
        (self.dir / "t.conf").write_bytes(head + b"".join(ln + b"\n" for ln in listen_lines))
        self.logf = open(self.dir / "log", "wb")
        self.proc = subprocess.Popen([binary, "-c", str(self.dir / "t.conf"), "-r", "role.txt"], cwd=self.dir,
                                     stdout=self.logf, stderr=subprocess.STDOUT)
        deadline = time.time() + 15
        while time.time() < deadline:
            text = self.log()
            if self.proc.poll() is not None or "listening on" in text or "started successfully" in text:
                break
            time.sleep(0.05)
        time.sleep(0.1)

    def log(self):
        return (self.dir / "log").read_text(errors="replace")

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.logf.close()
        shutil.rmtree(self.dir, ignore_errors=True)
        return self.proc.returncode


def listen_line(value):
    return b'LISTEN="' + value + b'"'


# ---- rows -------------------------------------------------------------------

def reach_rows(binary, caps, in_ns, old=False):
    deviations = 0
    rows = ["0.0.0.0", "127.0.0.1", D0, "::", "::1"]
    for addr in rows:
        res = ref.parse(addr.encode(), 8889)
        mode = res[1]
        if not in_ns and addr in ("0.0.0.0", D0, "::"):
            print(f"  skip  {addr}: needs the namespace (would be reachable from outside)")
            continue
        need = {"::": "dual ::", "::1": "bind ::1"}.get(addr)
        if need and not caps[need]:
            print(f"  skip  {addr}: no '{need}' capability in this environment")
            continue
        port = free_port()
        targets = [t for t in TARGETS if (t != D0 or in_ns) and (":" not in t or caps["bind ::1"])]
        base = baseline_socket(addr, mode, port)
        base_reach = {t: connects(t, port) for t in targets}
        base_ss = listening(port)
        base.close()
        cols = [t for t in targets if base_reach[t] == expected_reach(addr, mode, t)]
        for t in set(targets) - set(cols):
            print(f"  skip  {addr} -> {t}: baseline socket disagrees with the rule ({base_reach[t]})")
        srv = Server(binary, [listen_line(addr.encode())], port)
        try:
            alive = srv.proc.poll() is None
            got = {t: connects(t, port, http=True) for t in cols} if alive else {}
            ss = listening(port)
            log = srv.log()
        finally:
            srv.stop()
        want = {t: expected_reach(addr, mode, t) for t in cols}
        if old:
            same = alive and got == want and ss == base_ss
            print(f"  old   LISTEN={addr}: reach {got} ss {ss} -> {'as specified' if same else 'DEVIATES'}")
            deviations += 0 if same else 1
            continue
        check(alive, f"LISTEN={addr}: server running")
        check(got == want, f"LISTEN={addr}: reachable {sorted(t for t in got if got[t])}, expected "
                           f"{sorted(t for t in want if want[t])}")
        check(ss == base_ss, f"LISTEN={addr}: ss {ss} == baseline {base_ss}")
        check(expected_log(addr, mode, port) in log, f"LISTEN={addr}: log '{expected_log(addr, mode, port)}'")
    return deviations


def failing_start(binary, lines, port, expect, label, old=False, prebind=None):
    blocker = baseline_socket(*prebind, port) if prebind else None
    srv = Server(binary, lines, port)
    try:
        time.sleep(0.2)
        alive = srv.proc.poll() is None
        ss = listening(port)
        log = srv.log()
    finally:
        rc = srv.stop()
        if blocker:
            blocker.close()
    if old:
        return 0 if (not alive and rc == 1) else 1
    check(not alive and rc == 1, f"{label}: exit code 1 (rc={rc})")
    check(prebind is not None or ss == [], f"{label}: nothing listening on the port ({ss})")
    for e in expect:
        check(e in log, f"{label}: log contains {e!r}")
    return 0


def invalid_rows(binary, in_ns, old=False):
    deviations = 0
    values = [b"abc", b"999.1.1.1", b" 127.0.0.1", b"localhost", b"[::1]", b"fe80::1%lo", b"fe80::1",
              b"255.255.255.255", b"224.0.0.1", b"::ffff:127.0.0.1", b"127.1"]
    for v in values:
        why = ref.parse(v, 8889)[1]
        deviations += failing_start(binary, [listen_line(v)], free_port(),
                                    [f'Error: LISTEN "{v.decode()}" {why}'], f"LISTEN={v!r}", old)
    over = [(b'LISTEN="' + b"1" * 512 + b'"', "LISTEN value longer than 511 bytes", "512-byte value"),
            (b'LISTEN="' + b"1" * 1100 + b'"', "sets LISTEN but is longer than 1023 bytes", "1,111-byte line")]
    # LISTEN= / LISTEN="" parse as an empty assignment (POSIX ERE ".+?" is "(.+) optional"),
    # so only the last LISTEN counts, as for every key; an empty last one stops startup.
    over += [(b"LISTEN=", 'Error: LISTEN "', "empty LISTEN="), (b'LISTEN=""', 'Error: LISTEN "', 'LISTEN=""')]
    for line, e, label in over:
        deviations += failing_start(binary, [line], free_port(), [e], label, old)
    binds = [b"0.1.2.3", b"192.0.2.1", b"240.0.0.1"] if in_ns else [b"0.1.2.3"]
    for v in binds:
        deviations += failing_start(binary, [listen_line(v)], free_port(),
                                    ["libmicrohttpd: ", "Cannot assign requested address"], f"bind {v.decode()}", old)
    deviations += failing_start(binary, [listen_line(b"127.0.0.1")], free_port(),
                                ["libmicrohttpd: ", "Address already in use"], "port in use", old,
                                prebind=("127.0.0.1", ref.IPV4))
    return deviations


def recognizer_rows(binary, old=False):
    good = listen_line(b"127.0.0.1")
    fatal = [b'LISTEN = "127.0.0.1"', b"LISTEN =127.0.0.1", b'export LISTEN="127.0.0.1"', b'listen="127.0.0.1"',
             b"LISTEN 127.0.0.1", b'\xef\xbb\xbfLISTEN="127.0.0.1"', b'\vLISTEN="127.0.0.1"',
             b" " * 1023 + good, b" " * 1017 + good, good + b"\x00", b"LISTEN=\x00\"0.0.0.0\""]
    deviations = 0
    for bad in fatal:
        assert ref.scan(bad), bad[:40]
        for lines, where in (([good, bad], "after"), ([bad, good], "before")):
            deviations += failing_start(binary, lines, free_port(), ["Error: "],
                                        f"{where} a valid LISTEN: {bad[-30:]!r} ({len(bad)} bytes)", old)
    controls = [([good, c], None, None) for c in (b"#LISTEN=0.0.0.0", b"# LISTEN=0.0.0.0", b"LISTENX=1", b"LISTEN_PORT=1",
                                                 b'OPENAI_MODEL="' + b"m" * 1100 + b'"', b"   " + good, b"\t" + good + b"\r")]
    # A NUL byte used to make the loader discard the following line too (a LISTEN line
    # right after it was lost and 0.0.0.0 stayed); a 1023-byte line used to count as overlong.
    controls += [([b"DEBUG=no\x00junk", good], "line 9 contains a NUL byte and was ignored", None),
                 ([b"#" + b"x" * 1022, good], None, "and was ignored"),
                 ([b"#" + b"x" * 1023, good], "line 9 is longer than 1023 bytes and was ignored", None)]
    for lines, want_in, want_out in controls:
        port = free_port()
        srv = Server(binary, lines, port)
        try:
            ok = srv.proc.poll() is None and connects("127.0.0.1", port, http=True)
            log = srv.log()
        finally:
            srv.stop()
        if old:
            continue
        label = f"control {lines[0][:24]!r}+{lines[-1][:24]!r}"
        check(ok and expected_log("127.0.0.1", ref.IPV4, port) in log, f"{label}: starts on 127.0.0.1")
        if want_in:
            check(want_in in log, f"{label}: log contains {want_in!r}")
        if want_out:
            check(want_out not in log, f"{label}: log has no {want_out!r}")
    return deviations


def no_ipv6_rows(binary):
    for conf in ("all", "default", "lo", "d0"):
        Path(f"/proc/sys/net/ipv6/conf/{conf}/disable_ipv6").write_text("1")
    print("  (disable_ipv6=1 in the namespace)")
    failing_start(binary, [listen_line(b"::1")], free_port(), ["libmicrohttpd: "], "no IPv6: LISTEN=::1")
    port = free_port()
    srv = Server(binary, [listen_line(b"::")], port)
    try:
        ok = srv.proc.poll() is None and connects("127.0.0.1", port, http=True) and connects(D0, port, http=True)
        log = srv.log()
    finally:
        srv.stop()
    check(ok and expected_log("::", ref.DUAL_STACK, port) in log, "no IPv6: LISTEN=:: still serves IPv4 (dual stack)")


def leftovers(binary):
    """Processes running this executable (by /proc/<pid>/exe, not by command line)."""
    found = []
    for d in Path("/proc").iterdir():
        if d.name.isdigit():
            try:
                if os.readlink(d / "exe") == binary:
                    found.append(int(d.name))
            except OSError:
                pass
    return found


def main():
    args = sys.argv[1:]
    in_ns = bool(args) and args[0] == INNER
    if not in_ns:
        rc = reexec_in_namespace(args)
        if rc is not None:
            left = [p for b in args if not b.startswith("--") for p in leftovers(str(Path(b).resolve()))]
            print(f"after the namespace exited: {len(left)} server processes on the host {left}")
            return rc if not left else 1
    else:
        args = args[1:]
        setup_interfaces()
    binary = str(Path(args[0]).resolve())
    old = str(Path(args[args.index("--old") + 1]).resolve()) if "--old" in args else None

    caps = capabilities()
    print(f"namespace: {in_ns}, capabilities: {caps}")
    print("reachability:")
    reach_rows(binary, caps, in_ns)
    print("invalid values and bind failures:")
    invalid_rows(binary, in_ns)
    print("LISTEN line recognizer:")
    recognizer_rows(binary)
    if old:
        print("pre-fix binary on the same tables (each deviation is a case this test protects):")
        d1 = reach_rows(old, caps, in_ns, old=True)
        d2 = invalid_rows(old, in_ns, old=True)
        d3 = recognizer_rows(old, old=True)
        print(f"  pre-fix deviations: reachability {d1}, invalid values {d2}, recognizer {d3}")
        check(d1 > 0 and d2 > 0 and d3 > 0, "the pre-fix binary fails all three tables")
    if in_ns and caps["inet6"]:
        print("IPv6 disabled:")
        no_ipv6_rows(binary)
    left = leftovers(binary) + (leftovers(old) if old else [])
    check(left == [], f"no server processes left ({left})")
    print("OK" if not FAILURES else f"FAILED: {len(FAILURES)} check(s)")
    return 0 if not FAILURES else 1


if __name__ == "__main__":
    sys.exit(main())
