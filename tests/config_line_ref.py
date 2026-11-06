"""Specification of parse_config_line() in src/config_loader.c (PLAN_OPEN_ISSUES.md H, Q8).

The C code uses the POSIX ERE ^([A-Z_]+)=(["']?)(.+?)\\2$ on the trimmed line. In ERE
"(.+?)" means "(.+) optional" and matching is leftmost-longest, so:
  - every trimmed line that starts with [A-Z_]+= matches; the key is that run;
  - the quote group takes the opening quote whenever the line also ends with it and
    is at least two characters long; the value is then what lies between them,
    otherwise the whole right-hand side.
Q8 adds: a right-hand side that starts with a quote and does not end with the same
quote is an error (-3), judged before any quote is removed.
"""
import re

KEY_SIZE = VALUE_SIZE = 512
_BLANKS = b" \t\n\r"
_KEY = re.compile(rb"([A-Z_]+)=(.*)\Z", re.S)


def parse(line):
    """bytes (no NUL) -> (rc, key or None, value or None)."""
    t = line.strip(_BLANKS)
    if not t or t[:1] == b"#":
        return (0, None, None)
    m = _KEY.match(t)
    if not m:
        return (-1, None, None)
    key, rhs = m.group(1), m.group(2)
    if rhs and rhs[:1] in (b'"', b"'") and rhs[-1:] != rhs[:1]:
        return (-3, key if len(key) < KEY_SIZE else None, None)
    value = rhs[1:-1] if len(rhs) >= 2 and rhs[:1] in (b'"', b"'") and rhs[-1:] == rhs[:1] else rhs
    if len(key) >= KEY_SIZE or len(value) >= VALUE_SIZE:
        return (-2, None, None)
    return (1, key, value)


# (line, rc) written by hand from the rules; tests/test_config_syntax.py also checks
# each against parse() and against the C code.
CASES = [
    (b'TRANS_CACHE_FILE="a', -3), (b"TRANS_CACHE_FILE='a", -3), (b"TRANS_CACHE_FILE=\"a'", -3),
    (b"TRANS_CACHE_FILE='a\"", -3), (b'TRANS_CACHE_FILE="a".db', -3), (b'SEED="1" # comment', -3),
    (b'TRANS_CACHE_FILE="" # comment', -3), (b'INFLIGHT_MAX_WAITERS="256"       # followers', -3),
    (b"KEY=", 1), (b'KEY=""', 1), (b"KEY=''", 1), (b'KEY="', 1), (b'KEY="a"tail"', 1), (b"KEY=\"'a\"", 1),
    (b'KEY="v"', 1), (b"KEY='v'", 1), (b"KEY=v", 1), (b'KEY=v"', 1), (b'  KEY="v"  ', 1), (b'KEY="v"\r', 1),
    (b"KEY=a # b", 1), (b'KEY="a b"', 1), (b"key=v", -1), (b"KEY =v", -1), (b"# KEY=\"a", 0), (b"", 0),
    (b'KEY="' + b"x" * 512 + b'"', -2), (b'KEY="' + b"x" * 511 + b'"', 1), (b'KEY="' + b"x" * 600, -3),
]
