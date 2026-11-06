"""PLAN_OPEN_ISSUES.md C: strip_emoji_and_shortcodes() / unescape_string() vs tests/strip_ref.py.

    python3 -I tests/test_strip.py <harness> [--mutants]

- fixed cases (hand-written verdicts, also checked against the specification)
- generated inputs: shortcode-like runs, times, URLs, emoji, whitespace, valid and
  invalid UTF-8, escapes; compared byte for byte
- properties: output never longer than input; with output_size = strlen + 1 the
  call succeeds; with less it returns -1 instead of truncating
- mutants of the C source must each produce mismatches
"""
import random
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import strip_ref as ref  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
SEED = 20261106
TOKENS = [b":", b"::", b"smile", b"Smile", b"+1", b"-1", b"_", b"a", b"Z", b"0", b"10", b"30", b" ", b"  ", b"\t",
          b"\n", b"\r", b"\v", b"\f", b"http", b"//", b"/", b"[", b"]", b")", b"(", b"`", b"&",
          "가".encode(), "😀".encode(), "☀".encode(), "✅".encode(), b"\xf0", b"\xf0\x82\x98\x80", b"\xed\xa0\x80",
          b"\xc0\x80", b"\x80", b"\xff", b"\xe2\x98", b"\xf4\x90\x80\x80", b"\\", b"\\n", b"\\t", b"\\\\", b"\\'",
          b"\\\"", b"\\x", b"x"]


def generate(n):
    r = random.Random(SEED)
    out = []
    for _ in range(n):
        out.append(b"".join(r.choice(TOKENS) for _ in range(r.randrange(0, 14))))
    return out


def run(harness, reqs):
    res = subprocess.run([harness], input="".join(reqs), capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(res.stderr[-2000:])
    return res.stdout.splitlines()


def compare(harness, inputs, quiet=False):
    reqs = [f"S - {b.hex()}\n" for b in inputs] + [f"U - {b.hex()}\n" for b in inputs]
    got = run(harness, reqs)
    assert len(got) == 2 * len(inputs)
    bad = []
    for i, b in enumerate(inputs):
        for k, fn in ((0, ref.strip), (1, ref.unescape)):
            rc, _, h = got[k * len(inputs) + i].partition(" ")
            if rc != "0" or bytes.fromhex(h) != fn(b):
                bad.append(("strip" if k == 0 else "unescape", b[:40], rc, h[:40]))
    if not quiet:
        print(f"  C = specification on {len(inputs)} inputs x 2 functions: {len(bad)} mismatches")
    return bad


def properties(harness, inputs):
    longer = [b for b in inputs if len(ref.strip(b)) > len(b) or len(ref.unescape(b)) > len(b)]
    # output_size one byte short of what the result needs -> -1, never a truncated 0
    reqs, want = [], []
    for b in inputs:
        for mode, fn in (("S", ref.strip), ("U", ref.unescape)):
            need = len(fn(b)) + 1
            if need > 1:
                reqs.append(f"{mode} {need - 1} {b.hex()}\n")
                want.append("-1")
    got = [g.split(" ")[0] for g in run(harness, reqs)]
    short = sum(1 for g, w in zip(got, want) if g != w)
    print(f"  never longer than the input: {len(longer)} exceptions; one byte short -> -1: {len(reqs) - short}/{len(reqs)}")
    return not longer and short == 0


MUTANTS = [
    ("shortcode without the letter rule", "if (j == pos + 1 || !lower || j >= len || in[j] != ':') {",
     "if (j == pos + 1 || j >= len || in[j] != ':') {"),
    ("no left boundary", "if (pos > 0 && ascii_alnum((unsigned char)in[pos - 1])) {", "if (0) {"),
    ("no right boundary", "if (j + 1 < len && ascii_alnum((unsigned char)in[j + 1])) {", "if (0) {"),
    ("overlong accepted", "if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {",
     "if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {"),
    ("surrogates accepted", "if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {",
     "if (v < min || v > 0x10FFFF) {"),
    ("no upper bound", "if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {",
     "if (v < min || (v >= 0xD800 && v <= 0xDFFF)) {"),
    ("vertical tab not space", " || c == '\\v'", ""),
    ("newline does not reset", "                last_was_space = false;   /* Preserve newlines */", ""),
    ("unescape truncates silently", "        return -1;   /* output_size too small: never truncate silently */", "        return 0;"),
]


# Surrogates and code points past U+10FFFF are not emoji, and copying such a sequence as
# one character or as single invalid bytes writes the same bytes, so for strip output
# these two checks are equivalent mutants (measured: 0 of 50,000 outputs differ). The
# checks stay because utf8_decode() follows RFC 3629.
EQUIVALENT = {"surrogates accepted": "not emoji, same bytes copied", "no upper bound": "not emoji, same bytes copied"}


def mutants():
    src = (ROOT / "src/utils.c").read_text()
    survived = []
    sample = [ref.bs(a) for a, _ in ref.STRIP_CASES] + [b for b, _ in ref.STRIP_RAW] + generate(20000)
    with tempfile.TemporaryDirectory(prefix="strip-mutant-") as d:
        for name, old, new in MUTANTS:
            n = src.count(old)
            assert n >= 1, f"anchor missing: {name}"
            mutated = src.replace(old, new, 1)
            m = Path(d) / "utils.c"
            m.write_text(mutated)
            exe = Path(d) / "h"
            subprocess.run(["gcc", "-std=c11", "-O1", "-D_POSIX_C_SOURCE=200809L", "-D_DEFAULT_SOURCE", f"-I{ROOT}/include",
                            "-o", str(exe), str(ROOT / "tests/test_strip.c"), str(m), "-luuid"], check=True)
            bad = compare(str(exe), sample, quiet=True)
            if name == "unescape truncates silently":
                ok = not properties_quiet(str(exe), sample[:3000])
            else:
                ok = bool(bad)
            if name in EQUIVALENT:
                print(f"  mutant {name!r}: {'killed' if ok else 'equivalent (expected)'} - {EQUIVALENT[name]}")
                continue
            print(f"  mutant {name!r}: {'killed' if ok else 'SURVIVED'}")
            if not ok:
                survived.append(name)
    return survived


def properties_quiet(harness, inputs):
    reqs = []
    for b in inputs:
        need = len(ref.unescape(b)) + 1
        if need > 1:
            reqs.append(f"U {need - 1} {b.hex()}\n")
    return all(g.split(" ")[0] == "-1" for g in run(harness, reqs))


def main():
    harness = str(Path(sys.argv[1]).resolve())
    ok = True
    hand = [(a, e) for a, e in ref.STRIP_CASES if ref.strip(ref.bs(a)) != ref.bs(e)]
    hand += [(a, e) for a, e in ref.STRIP_RAW if ref.strip(a) != e]
    hand += [(a, e) for a, e in ref.UNESCAPE_CASES if ref.unescape(ref.bs(a)) != ref.bs(e)]
    print(f"  hand-written verdicts vs specification: {len(hand)} disagreements")
    ok &= not hand
    fixed = [ref.bs(a) for a, _ in ref.STRIP_CASES] + [b for b, _ in ref.STRIP_RAW] + [ref.bs(a) for a, _ in ref.UNESCAPE_CASES]
    inputs = fixed + generate(150_000)
    bad = compare(harness, inputs)
    for x in bad[:5]:
        print("   MISMATCH", x)
    ok &= not bad
    ok &= properties(harness, inputs)
    if "--mutants" in sys.argv:
        ok &= not mutants()
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
