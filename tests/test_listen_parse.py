"""V1/V2 of PLAN_LISTEN_ADDRESS.md: src/listen_address.c against tests/listen_address_ref.py.

    python3 -I tests/test_listen_parse.py <harness> <harness with config loader> [--mutants]

Every expected value is computed here; the C side only reports.
"""
import os
import random
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import listen_address_ref as ref  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
SEED = 20261106
N_ADDR, N_SCAN, N_LINES = 120_000, 60_000, 40_000


def expect_addr(text, port):
    r = ref.parse(text, port)
    if r[0] == "err":
        return f"err {r[1]}"
    _, mode, fam, raw, p = r
    return f"ok {mode} {fam} {raw.hex()} {p}"


def run(harness, requests):
    out = subprocess.run([harness], input="".join(requests), capture_output=True, text=True, check=True)
    return out.stdout.splitlines()


def address_requests(n):
    cases = [(t, p) for t in ref.ADDR_CASES for p in (8889, 1, 65535)] + [(b"127.0.0.1", 0), (b"::", 65536)]
    cases += ref.corpus(SEED, n)
    return cases, [f"A {p} {t.hex()}\n" for t, p in cases]


def scan_requests(n):
    r = random.Random(SEED + 1)
    lines = list(ref.SCAN_CASES) + [ref.gen_line(r) for _ in range(n)]
    reqs = ["S " + ",".join(c.hex() for c in ref.split_random(r, ln)) + "\n" for ln in lines]
    return lines, reqs


def differential(harness, n_addr, n_scan, quiet=False):
    cases, reqs = address_requests(n_addr)
    got = run(harness, reqs)
    assert len(got) == len(cases), (len(got), len(cases))
    bad = [(t, p, g, expect_addr(t, p)) for (t, p), g in zip(cases, got) if g != expect_addr(t, p)]
    lines, sreqs = scan_requests(n_scan)
    sgot = run(harness, sreqs)
    assert len(sgot) == len(lines)
    sbad = [(ln, g) for ln, g in zip(lines, sgot) if g != ("1" if ref.scan(ln) else "0")]
    if not quiet:
        kinds = {}
        for t, p in cases:
            e = expect_addr(t, p)
            k = e.split()[0] + (" " + e.split()[1] if e.startswith("ok") else ": " + e[4:40])
            kinds[k] = kinds.get(k, 0) + 1
        print(f"  address: {len(cases)} inputs, {len(bad)} mismatches; outcome mix:")
        for k in sorted(kinds):
            print(f"    {kinds[k]:7d}  {k}")
        print(f"  recognizer: {len(lines)} lines in random pieces, {len(sbad)} mismatches, "
              f"{sum(ref.scan(x) for x in lines)} recognized")
    return bad, sbad


def config_loader_invariant(harness):
    """A line the real parser accepts as key LISTEN must be recognized, otherwise the
    recognizer would reject valid configs (and the fixed cases document the rest)."""
    r = random.Random(SEED + 2)
    keys = [b"LISTEN", b"PORT", b"LISTEN_X", b"OPENAI_MODEL", b"LISTENER"]
    lines = list(ref.SCAN_CASES)
    for _ in range(N_LINES):
        if r.random() < 0.5:
            lead = b"".join(r.choice([b" ", b"\t", b"\r", b"\v", b"\xef\xbb\xbf", b""]) for _ in range(r.randrange(3)))
            q = r.choice([b"", b'"', b"'"])
            val = r.choice([b"127.0.0.1", b"::", b"x y", b'"', b"", b"0.0.0.0 # c"])
            lines.append(lead + r.choice(keys) + r.choice([b"=", b" =", b"= "]) + q + val + q + r.choice([b"", b"\n", b" \r\n"]))
        else:
            lines.append(ref.gen_line(r)[:1000])
    lines = [ln for ln in lines if b"\x00" not in ln and b"\n" not in ln.rstrip(b"\r\n")]
    got = run(harness, [f"P {ln.hex()}\n" for ln in lines])
    assert len(got) == len(lines)
    violations, table = [], {}
    for ln, g in zip(lines, got):
        rc, key, scanned = g.split()[:3]
        if rc == "1" and key == "LISTEN" and scanned != "1":
            violations.append(ln)
        if (scanned == "1") != ref.scan(ln):
            violations.append(ln)
        k = (rc, key == "LISTEN", scanned)
        table[k] = table.get(k, 0) + 1
    print(f"  config loader: {len(lines)} lines through parse_config_line(), {len(violations)} violations")
    for (rc, is_listen, scanned), cnt in sorted(table.items()):
        verdict = "accepted" if scanned == "0" or (rc == "1" and is_listen) else "startup error"
        print(f"    {cnt:6d}  rc={rc:>2} key=LISTEN:{str(is_listen):5} recognized={scanned} -> {verdict}")
    return violations


MUTANTS = [
    ("v4-mapped check removed", "if (memcmp(b, mapped, sizeof(mapped)) == 0) {", "if (0) {"),
    ("'::' as IPv6 only", "LISTEN_DUAL_STACK : LISTEN_IPV6_ONLY", "LISTEN_IPV6_ONLY : LISTEN_IPV6_ONLY"),
    ("multicast upper bound off by one", "b[0] <= 239", "b[0] < 239"),
    ("multicast lower bound off by one", "b[0] >= 224", "b[0] >= 225"),
    ("IPv4 port byte order", "sin->sin_port = htons((unsigned short)port);", "sin->sin_port = (unsigned short)port;"),
    ("IPv6 port byte order", "sin6->sin6_port = htons((unsigned short)port);", "sin6->sin6_port = (unsigned short)port;"),
    ("link-local mask too narrow", "(b[1] & 0xc0) == 0x80", "(b[1] & 0xf0) == 0x80"),
    ("IPv6 multicast check removed", "if (b[0] == 0xff) {", "if (0) {"),
    ("broadcast check removed", "b[0] == 255 && b[1] == 255", "0 && b[1] == 255"),
    ("port range check removed", "if (port < 1 || port > 65535) {", "if (0) {"),
    ("'_' not an identifier byte", " || c == '_'", ""),
    ("BOM not skipped", "} else if (c == bom[0]) {", "} else if (0) {"),
    ("export needs no blank", "scan->state = is_blank(c) ? SCAN_AFTER_EXPORT : SCAN_NO;", "scan->state = SCAN_AFTER_EXPORT;"),
    ("LISTEN at end of input not matched", "return scan->state == SCAN_YES || scan->state == SCAN_WORD_DONE;",
     "return scan->state == SCAN_YES;"),
    ("vertical tab not blank", " || c == '\\v'", ""),
    ("case-sensitive LISTEN", "unsigned char l = ascii_lower(c);", "unsigned char l = c;"),
]


def mutants():
    src = (ROOT / "src/listen_address.c").read_text()
    survived = []
    with tempfile.TemporaryDirectory(prefix="listen-mutant-") as d:
        for name, old, new in MUTANTS:
            assert src.count(old) == 1, f"mutant anchor not unique: {name}"
            m = Path(d) / "listen_address.c"
            m.write_text(src.replace(old, new))
            exe = Path(d) / "h"
            subprocess.run(["gcc", "-std=c11", "-O2", "-D_POSIX_C_SOURCE=200809L", "-D_DEFAULT_SOURCE",
                            f"-I{ROOT / 'include'}", "-o", str(exe), str(ROOT / "tests/test_listen_address.c"), str(m)],
                           check=True)
            bad, sbad = differential(str(exe), 20_000, 20_000, quiet=True)
            print(f"  mutant {name!r}: {len(bad)} + {len(sbad)} mismatches -> {'killed' if bad or sbad else 'SURVIVED'}")
            if not bad and not sbad:
                survived.append(name)
    return survived


def main():
    harness, harness_cl = (str(Path(p).resolve()) for p in sys.argv[1:3])
    bad, sbad = differential(harness, N_ADDR, N_SCAN)
    for x in (bad[:5] + sbad[:5]):
        print("   MISMATCH", x)
    violations = config_loader_invariant(harness_cl)
    for x in violations[:5]:
        print("   VIOLATION", x)
    survived = mutants() if "--mutants" in sys.argv else []
    ok = not bad and not sbad and not violations and not survived
    print("OK" if ok else f"FAILED (survived mutants: {survived})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
