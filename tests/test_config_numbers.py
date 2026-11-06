"""PLAN_OPEN_ISSUES.md F: numeric settings keep their meaning and no longer wrap.

    python3 -I tests/test_config_numbers.py <harness test_config_numbers> <transbasket>

Expected values are computed here: integers with a python model of glibc atoi plus
saturation (as in tests/test_cleanup_days.py); for floating point values and the two
strtol keys the number itself comes from libc atof/strtol through ctypes - what is
under test is what the loader does with it (finiteness, clamping, wrapping, defaults).
"""
import ctypes
import math
import random
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HARNESS, SERVER = (str(Path(p).resolve()) for p in sys.argv[1:3])
INT_MAX, INT_MIN = 2**31 - 1, -2**31
FAILURES = []

libc = ctypes.CDLL("libc.so.6")
libc.atof.restype, libc.atof.argtypes = ctypes.c_double, [ctypes.c_char_p]
libc.strtol.restype, libc.strtol.argtypes = ctypes.c_long, [ctypes.c_char_p, ctypes.POINTER(ctypes.c_char_p), ctypes.c_int]


def check(cond, what):
    if not cond:
        print("  FAIL  " + what)
        FAILURES.append(what)


def atoi_sat(text):
    """glibc atoi = (int)strtol(s, NULL, 10), saturated; plain as in utils.c."""
    i = 0
    while i < len(text) and text[i] in " \t\n\v\f\r":
        i += 1
    j = i + (1 if i < len(text) and text[i] in "+-" else 0)
    k = j
    while k < len(text) and "0" <= text[k] <= "9":
        k += 1
    if k == j:
        return 0, False
    v = int(text[i:k])
    return max(INT_MIN, min(INT_MAX, v)), k == len(text) and INT_MIN <= v <= INT_MAX


def strtol(text):
    b = text.encode()
    buf = ctypes.create_string_buffer(b)
    end = ctypes.c_char_p()
    n = libc.strtol(buf, ctypes.byref(end), 10)
    consumed = ctypes.cast(end, ctypes.c_void_p).value - ctypes.addressof(buf)
    return n, consumed, len(b)


def expected(key, value, codex):
    """-> ("fail", None) or ("ok", field, value, warned)."""
    if key in ("PORT", "SEED", "TRANS_CACHE_THRESHOLD"):
        n, plain = atoi_sat(value)
        if key == "PORT" and not 1 <= n <= 65535:
            return ("fail", None)
        if key == "TRANS_CACHE_THRESHOLD" and n < 1:
            n = 5
        field = {"PORT": "port", "SEED": "seed", "TRANS_CACHE_THRESHOLD": "threshold"}[key]
        return ("ok", field, n, not plain)
    if key in ("TEMPERATURE", "TOP_P"):
        d = libc.atof(value.encode())
        return ("fail", None) if not math.isfinite(d) else ("ok", key.lower(), d, False)
    if key in ("FREQUENCY_PENALTY", "PRESENCE_PENALTY"):
        d = libc.atof(value.encode())
        if math.isnan(d):
            return ("fail", None)
        return ("ok", key.lower(), max(-2.0, min(2.0, d)), False)
    if key == "CODEX_TIMEOUT":
        n, used, total = strtol(value)
        if used == 0 or used != total or n <= 0:
            t, warned = 100, True
        else:
            t, warned = min(n, INT_MAX), False
        if codex and t >= 120:
            t, warned = 110, True
        return ("ok", "codex_timeout", t, warned)
    if key == "CODEX_MAX_CONCURRENCY":
        n, used, total = strtol(value)
        c = 0 if used == 0 or used != total or n < 1 else min(n, INT_MAX)
        return ("ok", "codex_concurrency", c, False)
    raise KeyError(key)


INTS = ["8889", "8889x", "08889", " 8889", "+8889", "-1", "0", "1", "65535", "65536", "4294975185", "99999999999",
        "2147483647", "2147483648", "-2147483648", "-2147483649", "abc", "", "1e3", "0x10", " ", "9" * 30, "\t42"]
FLOATS = ["0.2", "nan", "NaN", "-nan", "inf", "-inf", "INF", "infinity", "1e999", "-1e999", "0,7", "abc", "0x1p-2",
          "nan(1)", "1e-400", " 0.5", "3", "-3", "2.5", "1e308", ""]
LONGS = ["100", "10x", " +12", "+2", "0", "-5", "119", "120", "3000000000", "9223372036854775807",
         "9223372036854775808", "99999999999999999999", "abc", "", " ", "2147483647", "2147483648"]


def run(extra, codex):
    with tempfile.TemporaryDirectory(prefix="cfgnum-") as d:
        d = Path(d)
        (d / "role.txt").write_text("r")
        body = (f'OPENAI_BASE_URL="http://127.0.0.1:9/v1"\nOPENAI_MODEL="m"\nOPENAI_API_KEY="k"\nLISTEN="127.0.0.1"\n'
                f'PROMPT_PREFIX_FILE="{ROOT}/PROMPT_PREFIX.txt"\nLANGUAGE_TABLE_FILE="{ROOT}/language_tables.conf"\n')
        if codex:
            body += f'MODEL_TYPE="codex"\nCODEX_RUN_DIR="{d}"\n'
        (d / "t.conf").write_text(body + extra)
        r = subprocess.run([HARNESS, str(d / "t.conf"), str(d / "role.txt")], capture_output=True, text=True)
        return r.stdout


def parse_result(out):
    line = [l for l in out.splitlines() if l.startswith("RESULT ")]
    if not line:
        return None
    return {k: float(v) for k, v in (kv.split("=") for kv in line[0].split()[1:])}


def config_rows():
    r = random.Random(20261106)
    cases = [(k, v) for k in ("PORT", "SEED", "TRANS_CACHE_THRESHOLD") for v in INTS]
    cases += [(k, v) for k in ("TEMPERATURE", "TOP_P", "FREQUENCY_PENALTY", "PRESENCE_PENALTY") for v in FLOATS]
    cases += [(k, v) for k in ("CODEX_TIMEOUT", "CODEX_MAX_CONCURRENCY") for v in LONGS]
    cases += [("SEED", str(r.randrange(-2**40, 2**40))) for _ in range(20)]
    cases += [("PORT", str(r.randrange(-2**33, 2**33))) for _ in range(20)]
    n = 0
    for key, value in cases:
        for codex in ((False, True) if key == "CODEX_TIMEOUT" else (False,)):
            out = run(f'{key}="{value}"\n' if value else f"{key}=\n", codex)
            exp = expected(key, value, codex)
            got = parse_result(out)
            n += 1
            label = f"{key}={value!r}{' (codex)' if codex else ''}"
            if exp[0] == "fail":
                check(got is None and "LOAD_FAIL" in out, f"{label}: startup error expected, got {got}")
                if key in ("TEMPERATURE", "TOP_P"):
                    check(f"Error: {key} '{value}' is not a finite number" in out, f"{label}: error text")
                if key.endswith("PENALTY"):
                    check(f"Error: {key} '{value}' is not a number" in out, f"{label}: error text")
                continue
            _, field, want, warned = exp
            check(got is not None, f"{label}: loaded ({out.strip()[-120:]})")
            if got is None:
                continue
            check(got[field] == want, f"{label}: {field}={got[field]!r}, expected {want!r}")
            if key in ("PORT", "SEED", "TRANS_CACHE_THRESHOLD"):
                check((f"Warning: {key} '" in out) == warned, f"{label}: warning shown = {warned}")
    print(f"  config: {n} loads, effective value / warning / startup error as computed")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def workers_rows():
    """-w and MAX_WORKERS through the real binary: the banner line '  Workers: N'."""
    n = 0
    for source in ("-w", "MAX_WORKERS"):
        for value in ["8", "8x", "0", "-5", "abc", "99999999999", " 4", "2147483648"]:
            v, plain = atoi_sat(value)
            want = v if v > 0 else 30           # 0 -> environment/default 30, negative -> 30 (http_server.c)
            banner = v if v != 0 else 30         # the banner prints what main() passes on
            with tempfile.TemporaryDirectory(prefix="cfgw-") as d:
                d = Path(d)
                (d / "role.txt").write_text("r")
                port = free_port()
                (d / "t.conf").write_text(
                    f'OPENAI_BASE_URL="http://127.0.0.1:9/v1"\nOPENAI_MODEL="m"\nOPENAI_API_KEY="k"\nLISTEN="127.0.0.1"\n'
                    f'PORT="{port}"\nPROMPT_PREFIX_FILE="{ROOT}/PROMPT_PREFIX.txt"\nLANGUAGE_TABLE_FILE="{ROOT}/language_tables.conf"\n'
                    f'TRANS_CACHE_TYPE="text"\nTRANS_CACHE_FILE="{d}/c.jsonl"\n')
                args = [SERVER, "-c", str(d / "t.conf"), "-r", "role.txt"] + (["-w", value] if source == "-w" else [])
                env = {"PATH": "/usr/bin:/bin"} | ({"MAX_WORKERS": value} if source == "MAX_WORKERS" else {})
                log = open(d / "log", "wb")
                p = subprocess.Popen(args, cwd=d, stdout=log, stderr=subprocess.STDOUT, env=env)
                try:
                    for _ in range(200):
                        if p.poll() is not None or "listening on" in (d / "log").read_text(errors="replace"):
                            break
                        time.sleep(0.03)
                finally:
                    p.terminate()
                    p.wait(timeout=15)
                    log.close()
                text = (d / "log").read_text(errors="replace")
                n += 1
                check(f"  Workers: {banner}\n" in text, f"{source}={value!r}: banner Workers {banner}")
                check(f"initialized with {want} workers" in text, f"{source}={value!r}: server uses {want}")
                check((f"Warning: {source} '" in text) == (not plain), f"{source}={value!r}: warning = {not plain}")
    print(f"  workers: {n} server starts")


if __name__ == "__main__":
    config_rows()
    workers_rows()
    print("OK" if not FAILURES else f"FAILED: {len(FAILURES)}")
    sys.exit(0 if not FAILURES else 1)
