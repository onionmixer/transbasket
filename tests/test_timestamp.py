"""PLAN_CODE_ISSUES_4.md 2/3: validate_timestamp() (src/utils.c) against tests/timestamp_ref.py.

    python3 -I tests/test_timestamp.py <harness> [--mutants]

1. hand-written verdicts agree with the specification;
2. the C function equals the specification on the fixed cases plus generated inputs
   (every field at and around its limits, every month/leap-year combination, random
   byte edits, lengths 62-65 for both zone forms);
3. nothing is accepted that the old regex rejected (the change only narrows);
4. --mutants: each wrong variant of the C code is caught.
"""
import os
import random
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import timestamp_ref as ref  # noqa: E402

FAILURES = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        FAILURES.append(what)


def run(harness, inputs):
    p = subprocess.run([harness], input="".join(b.hex() + "\n" for b in inputs), capture_output=True,
                       text=True, timeout=300)
    assert p.returncode == 0 and not p.stderr, (p.returncode, p.stderr[-500:])   # e.g. a sanitizer report
    out = p.stdout.split()
    assert len(out) == len(inputs), (len(out), len(inputs))
    return [o == "1" for o in out]


def corpus(seed=20261107):
    r = random.Random(seed)
    out = [c for c, _ in ref.CASES]
    for y in (0, 1, 4, 100, 400, 1900, 1999, 2000, 2023, 2024, 2100, 2400, 9999):
        for mo in range(0, 14):
            for d in (0, 1, 28, 29, 30, 31, 32):
                out.append(b"%04d-%02d-%02dT12:00:00Z" % (y, mo, d))
    for h in (0, 23, 24, 99):
        for mi in (0, 59, 60):
            for s in (0, 59, 60, 61):
                out.append(b"2026-11-06T%02d:%02d:%02dZ" % (h, mi, s))
    for sign in b"+-":
        for oh in (0, 9, 23, 24):
            for om in (0, 30, 59, 60):
                out.append(b"2026-11-06T01:02:03%c%02d:%02d" % (sign, oh, om))
    for zone in (b"Z", b"+09:00"):
        base = b"2026-11-06T01:02:03."
        for total in range(60, 67):
            k = total - len(base) - len(zone)
            if k >= 0:
                out.append(base + b"7" * k + zone)
    alphabet = b"0123456789-:.TZtz+ \xea\xb0"
    seeds = [c for c, v in ref.CASES if v]
    for _ in range(60000):
        b = bytearray(r.choice(seeds))
        for _ in range(r.randrange(1, 4)):
            op = r.randrange(3)
            if op == 0 and b:
                b[r.randrange(len(b))] = r.choice(alphabet)
            elif op == 1:
                b.insert(r.randrange(len(b) + 1), r.choice(alphabet))
            elif b:
                del b[r.randrange(len(b))]
        if 0 not in b and 10 not in b:
            out.append(bytes(b))
    return out


MUTANTS = [
    ("century rule dropped", "bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;", "bool leap = year % 4 == 0;"),
    ("400 rule dropped", "bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;", "bool leap = (year % 4 == 0 && year % 100 != 0);"),
    ("April has 31 days", "{ 31, 28, 31, 30, 31, 30,", "{ 31, 28, 31, 31, 31, 30,"),
    ("second 60 refused", "second < 0 || second > 60", "second < 0 || second > 59"),
    ("length cap 64", "if (len > TIMESTAMP_MAX_LEN || len < 20)", "if (len > TIMESTAMP_MAX_LEN + 1 || len < 20)"),
    ("offset hour 24 accepted", "off_h >= 0 && off_h <= 23", "off_h >= 0 && off_h <= 24"),
    ("empty fraction accepted", "if (i == start) {\n            return false;", "if (0) {\n            return false;"),
    ("lower-case z accepted", "if (s[i] == 'Z') {", "if (s[i] == 'Z' || s[i] == 'z') {"),
    ("trailing bytes after Z", "return s[i + 1] == '\\0';", "return true;"),
    ("day range not checked", "if (day > last_day) {", "if (0) {"),
]


def mutants(sample):
    src = (ROOT / "src/utils.c").read_text()
    want = [ref.valid(b) for b in sample]
    survived = []
    with tempfile.TemporaryDirectory(prefix="ts-mutant-") as d:
        for name, old, new in MUTANTS:
            assert src.count(old) == 1, f"anchor: {name}"
            (Path(d) / "utils.c").write_text(src.replace(old, new))
            exe = Path(d) / "h"
            subprocess.run(["gcc", "-std=c11", "-O1", "-D_POSIX_C_SOURCE=200809L", "-D_DEFAULT_SOURCE", f"-I{ROOT}/include",
                            "-o", str(exe), str(HERE / "test_timestamp.c"), str(Path(d) / "utils.c"), "-luuid"], check=True)
            killed = run(str(exe), sample) != want
            print(f"  mutant {name!r}: {'killed' if killed else 'SURVIVED'}")
            if not killed:
                survived.append(name)
    return survived


def main():
    harness = str(Path(sys.argv[1]).resolve())
    for c, v in ref.CASES:
        if b"1" * 37 in c or b"1" * 42 in c:
            assert len(c) in (63, 64), (len(c), c[:30])
    hand = [(c, v) for c, v in ref.CASES if ref.valid(c) != v]
    check(not hand, f"hand-written verdicts agree with the specification ({len(ref.CASES)} cases) {hand[:2]}")
    sample = corpus()
    got = run(harness, sample)
    bad = [(b, g) for b, g in zip(sample, got) if g != ref.valid(b)]
    check(not bad, f"C = specification on {len(sample)} inputs ({sum(got)} accepted) {bad[:3]}")
    widened = [b for b, g in zip(sample, got) if g and not ref.old_valid(b)]
    check(not widened, f"nothing accepted that the old regex refused {widened[:3]}")
    narrowed = sum(1 for b, g in zip(sample, got) if not g and ref.old_valid(b))
    print(f"  (old regex accepted {narrowed} of these inputs that are now refused)")
    if "--mutants" in sys.argv:
        survived = mutants(sample)
        check(not survived, f"all {len(MUTANTS)} mutants caught {survived}")
    print("OK" if not FAILURES else f"FAILED: {len(FAILURES)}")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
