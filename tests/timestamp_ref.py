"""Specification of validate_timestamp() in src/utils.c (PLAN_CODE_ISSUES_4.md 2/3).

Written independently of the C code: a regular expression for the shape plus
calendar arithmetic for the ranges. Works on bytes (the C function sees a C string).

    RFC 3339 section 5.6 date-time, with these choices:
    - at most 63 bytes (TIMESTAMP_MAX_LEN), so a stored value is never cut;
    - upper-case "T" and "Z" only (RFC 3339 also allows lower case; not accepted here);
    - second 00-60 at any time (section 5.7 allows 60 only at a real leap second);
    - year 0000-9999 in the proleptic Gregorian calendar (0000 is a leap year).
"""
import re

MAX_LEN = 63
SHAPE = re.compile(rb"([0-9]{4})-([0-9]{2})-([0-9]{2})T([0-9]{2}):([0-9]{2}):([0-9]{2})"
                   rb"(?:\.[0-9]+)?(?:Z|[+-]([0-9]{2}):([0-9]{2}))")
# what the regex in src/utils.c accepted before this change (no length cap, no ranges)
OLD = re.compile(rb"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)?(Z|[+-][0-9]{2}:[0-9]{2})")


def leap(year):
    return year % 400 == 0 or (year % 4 == 0 and year % 100 != 0)


def last_day(year, month):
    if month == 2:
        return 29 if leap(year) else 28
    return 30 if month in (4, 6, 9, 11) else 31


def valid(b):
    if len(b) > MAX_LEN:
        return False
    m = SHAPE.fullmatch(b)
    if not m:
        return False
    y, mo, d, h, mi, s = (int(x) for x in m.groups()[:6])
    if not (1 <= mo <= 12 and 1 <= d <= last_day(y, mo) and h <= 23 and mi <= 59 and s <= 60):
        return False
    if m.group(7) is not None and not (int(m.group(7)) <= 23 and int(m.group(8)) <= 59):
        return False
    return True


def old_valid(b):
    return OLD.fullmatch(b) is not None


# (input, verdict written by hand) - checked against valid() by the test
CASES = [
    (b"2026-11-06T01:02:03Z", True),
    (b"2026-11-06T01:02:03.456Z", True),
    (b"2026-11-06T01:02:03.123456789+09:00", True),
    (b"2026-11-06T01:02:03-00:00", True),
    (b"2026-11-06T23:59:60Z", True),
    (b"2026-11-06T12:34:60+05:30", True),
    (b"2026-11-06T01:02:61Z", False),
    (b"2026-11-06T24:00:00Z", False),
    (b"2026-11-06T23:60:00Z", False),
    (b"2026-13-01T00:00:00Z", False),
    (b"2026-00-01T00:00:00Z", False),
    (b"2026-01-00T00:00:00Z", False),
    (b"2026-01-31T00:00:00Z", True),
    (b"2026-04-31T00:00:00Z", False),
    (b"2026-06-30T00:00:00Z", True),
    (b"2024-02-29T00:00:00Z", True),
    (b"2023-02-29T00:00:00Z", False),
    (b"2000-02-29T00:00:00Z", True),
    (b"1900-02-29T00:00:00Z", False),
    (b"2100-02-29T00:00:00Z", False),
    (b"0000-02-29T00:00:00Z", True),
    (b"9999-12-31T23:59:59Z", True),
    (b"2026-99-99T99:99:99Z", False),
    (b"2026-11-06T01:02:03+23:59", True),
    (b"2026-11-06T01:02:03+24:00", False),
    (b"2026-11-06T01:02:03+09:60", False),
    (b"2026-11-06t01:02:03Z", False),
    (b"2026-11-06T01:02:03z", False),
    (b"2026-11-06 01:02:03Z", False),
    (b"2026-11-06T01:02:03.Z", False),
    (b"2026-11-06T01:02:03", False),
    (b"2026-11-06T01:02:03.5", False),
    (b"2026-11-06T01:02:03ZZ", False),
    (b"2026-11-06T01:02:03Z ", False),
    (b" 2026-11-06T01:02:03Z", False),
    (b"2026-11-06T01:02:03+0900", False),
    (b"2026-11-06T01:02:03+09:00:00", False),
    (b"2026-1-06T01:02:03Z", False),
    (b"20261-11-06T01:02:03Z", False),
    (b"", False),
    (b"2026", False),
    (b"2026-11-06T01:02:03.\xea\xb0\x80Z", False),
    (b"\xef\xbc\x92026-11-06T01:02:03Z", False),
    (b"2026-11-06T01:02:03." + b"1" * 42 + b"Z", True),     # 63 bytes
    (b"2026-11-06T01:02:03." + b"1" * 43 + b"Z", False),    # 64 bytes
    (b"2026-11-06T01:02:03." + b"1" * 37 + b"+09:00", True),  # 63 bytes
    (b"2026-11-06T01:02:03." + b"1" * 38 + b"+09:00", False), # 64 bytes
]
