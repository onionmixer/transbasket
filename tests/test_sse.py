"""PLAN_OPEN_ISSUES.md D/E: handle_streaming_response() vs tests/sse_ref.py.

    python3 -I tests/test_sse.py <harness tests/test_sse.c> [--mutants]

- fixed cases (hand-written, also checked against the specification) and generated bodies
- allocation failure at every malloc() the function makes: it must end (no loop) with
  SSE_NO_MEMORY - the old chunk loop retried the same chunk forever
- mutants of src/backend_normal.c must each be caught
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sse_ref as ref  # noqa: E402

ROOT = HERE.parent
LIBS = ["-lcurl", "-lmicrohttpd", "-lcjson", "-luuid", "-lm", "-lssl", "-lcrypto", "-lsqlite3", "-pthread"]


def run(harness, reqs, timeout=300):
    r = subprocess.run([harness], input="".join(reqs), capture_output=True, text=True, timeout=timeout)
    if r.returncode != 0:
        raise RuntimeError(r.stderr[-2000:])
    return r.stdout.splitlines()


def expect(body):
    st, c = ref.parse(body)
    return st, c


def differential(harness, bodies, quiet=False):
    got = run(harness, [f"S {b.hex()}\n" for b in bodies])
    assert len(got) == len(bodies)
    bad, mix = [], {}
    for b, g in zip(bodies, got):
        st, hexc, _ = g.split(" ")
        want = expect(b)
        have = (int(st), None if hexc == "-" else bytes.fromhex(hexc))
        if have != want:
            bad.append((b[:80], have, want))
        mix[want[0]] = mix.get(want[0], 0) + 1
    if not quiet:
        print(f"  C = specification on {len(bodies)} bodies: {len(bad)} mismatches; status mix {dict(sorted(mix.items()))}")
    return bad


def alloc_failures(harness, bodies):
    reqs, labels = [], []
    counts = run(harness, [f"S {b.hex()}\n" for b in bodies])
    assert len(counts) == len(bodies)
    for b, g in zip(bodies, counts):
        n = int(g.split(" ")[2])
        for k in range(1, n + 1):
            reqs.append(f"F {k} {b.hex()}\n")
            labels.append((b[:40], k))
    got = run(harness, reqs, timeout=120)      # a retry loop would hang here
    assert len(got) == len(reqs), (len(got), len(reqs))
    wrong = [(lab, g) for lab, g in zip(labels, got) if int(g.split(" ")[0]) != ref.NO_MEMORY]
    print(f"  allocation failure at each of {len(reqs)} malloc calls: {len(reqs) - len(wrong)} ended with NO_MEMORY")
    return wrong


MUTANTS = [
    ("raw NUL check removed", "    if (memchr(data, 0, size)) {\n        *status = SSE_RAW_NUL;", "    if (0) {\n        *status = SSE_RAW_NUL;"),
    ("junk after JSON accepted", "        if (*q != ' ' && *q != '\\t' && *q != '\\r' && *q != '\\n') {", "        if (0) {"),
    ("error member ignored", "    if (err && !cJSON_IsNull(err)) {", "    if (0) {"),
    ("content type unchecked", "            if (!cJSON_IsString(content) || !content->valuestring) {", "            if (!content->valuestring) {"),
    ("CR not a line end", "while (j < size && data[j] != '\\n' && data[j] != '\\r') {", "while (j < size && data[j] != '\\n') {"),
    ("event: error ignored", "    if (error_event) {\n        return SSE_ERROR_EVENT;", "    if (0) {\n        return SSE_ERROR_EVENT;"),
    ("multi-line data not joined", "        if (*ev_data) ev[(*ev_len)++] = '\\n';", "        if (*ev_data) *ev_len = 0;"),
    ("leading BOM not skipped", "size_t i = (size >= 3 && memcmp(data, \"\\xEF\\xBB\\xBF\", 3) == 0) ? 3 : 0;", "size_t i = 0;"),
    ("colonless field ignored", "    const char *colon = memchr(line, ':', len);\n", "    const char *colon = memchr(line, ':', len);\n    if (!colon) return;\n"),
]


# cJSON 1.7.15 fills valuestring only for strings when parsing (measured for numbers,
# objects, arrays, booleans and null), so the type test and the NULL test reject the
# same contents.
EQUIVALENT = {"content type unchecked": "only strings carry valuestring in parsed cJSON"}


def mutants(sample):
    src = (ROOT / "src/backend_normal.c").read_text()
    survived = []
    others = [str(p) for p in sorted((ROOT / "src").glob("*.c")) if p.name not in ("backend_normal.c", "main.c", "cache_tool.c")]
    with tempfile.TemporaryDirectory(prefix="sse-mutant-") as d:
        d = Path(d)
        (d / "src").mkdir()
        (d / "tests").mkdir()
        harness_src = (HERE / "test_sse.c").read_text()
        (d / "tests/test_sse.c").write_text(harness_src)
        for name, old, new in MUTANTS:
            assert src.count(old) == 1, f"anchor: {name}"
            (d / "src/backend_normal.c").write_text(src.replace(old, new))
            exe = d / "h"
            subprocess.run(["gcc", "-std=c11", "-O1", "-D_POSIX_C_SOURCE=200809L", "-D_DEFAULT_SOURCE", f"-I{ROOT}/include",
                            "-o", str(exe), str(d / "tests/test_sse.c"), *others, *LIBS], check=True)
            killed = bool(differential(str(exe), sample, quiet=True))
            if name in EQUIVALENT:
                print(f"  mutant {name!r}: {'killed' if killed else 'equivalent (expected)'} - {EQUIVALENT[name]}")
                continue
            print(f"  mutant {name!r}: {'killed' if killed else 'SURVIVED'}")
            if not killed:
                survived.append(name)
    return survived


def main():
    harness = str(Path(sys.argv[1]).resolve())
    ok = True
    hand = [(b[:60], st, c, ref.parse(b)) for b, st, c in ref.CASES if ref.parse(b) != (st, c)]
    print(f"  hand-written verdicts vs specification: {len(hand)} disagreements {hand[:2]}")
    ok &= not hand
    bodies = [b for b, _, _ in ref.CASES] + ref.corpus(20261106, 60000)
    bad = differential(harness, bodies)
    for x in bad[:5]:
        print("   MISMATCH", x)
    ok &= not bad
    ok &= not alloc_failures(harness, [b for b, _, _ in ref.CASES])
    if "--mutants" in sys.argv:
        ok &= not mutants([b for b, _, _ in ref.CASES] + ref.corpus(7, 5000))
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
