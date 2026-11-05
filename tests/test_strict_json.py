"""Differential test: C strict JSON validator (src/strict_json.c) vs the
normative reference tests/strict_json_ref.py - verdict AND reason.

    python3 -I tests/test_strict_json.py obj/test_strict_json [--seed S]
"""
import collections
import os
import random
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import strict_json_ref as J  # noqa: E402


def reference(data):
    try:
        J.validate(data)
        return "accept"
    except J.Reject as exc:
        return str(exc)


def mutations(rnd, n):
    """The same mutation generator as strict_json_ref.fuzz()."""
    seeds = [c for c, _ in J.CASES if len(c) < 200] + [
        b'{"a":[1,2,{"b":null}],"c":"\\n\\u00e9"}', b'[true,false,-1.5e-3,"x"]', b'{"k":{"k":{"k":[]}}}',
        b'"\\ud800\\ud800\\udc00"', b'{"a":"\\ud83d\\ude00\\ud800x"}']
    alphabet = b' \t\n\r{}[]:,"\\/-+.0123456789eEtrufalsn\x00\x01\x7fu\xc3\xa9dDcC8'
    for _ in range(n):
        b = bytearray(rnd.choice(seeds))
        for _ in range(rnd.randrange(1, 4)):
            op, pos = rnd.randrange(3), rnd.randrange(len(b) + 1)
            if op == 0 and b:
                del b[min(pos, len(b) - 1)]
            elif op == 1:
                b.insert(pos, rnd.choice(alphabet))
            elif b:
                b[min(pos, len(b) - 1)] = rnd.choice(alphabet)
        yield bytes(b)


ESCAPES = [b'\\"', b'\\\\', b'\\/', b'\\b', b'\\f', b'\\n', b'\\r', b'\\t', b'\\u0041', b'\\u00e9', b'\\u0000',
           b'\\ud83d\\ude00', b'\\ud800', b'\\udc00', b'\\ud800\\u0041', b'\\ud800\\uZZZZ', b'\\ud800\\q', b'\\U0041',
           b'\\x', b'\\u', b'\\u0', b'\\u00', b'\\u000', b'\\u00000', b'\\\\u0000', b'\\uD83D\\uDE00', b'\\ud800\\ud800\\udc00']
NUMBERS = [b'0', b'-0', b'1', b'-1', b'01', b'1.', b'.5', b'+1', b'1e', b'1e+', b'1E+2', b'1.5e-3', b'1e400',
           b'1' * 50, b'-', b'00', b'0.0', b'1e99999']
KEYS = [b'"translation"', b'"source_language"', b'"mixed"', b'"Translation"', b'"\\u0074ranslation"',
        b'"x"', b'"translation\\u0000junk"', b'"translation\\uZZZZ"', b'"' + b'k' * 40 + b'"']


def gen_string(rnd):
    parts = [b'"']
    for _ in range(rnd.randrange(0, 5)):
        r = rnd.random()
        if r < 0.4:
            parts.append(rnd.choice(ESCAPES))
        elif r < 0.6:
            parts.append(rnd.choice(["가".encode(), "é".encode(), b"\xf0\x9f\x98\x80", b"\x7f", b"\x01", b"\n", b"a"]))
        else:
            parts.append(b"abc")
    if rnd.random() > 0.03:
        parts.append(b'"')
    return b"".join(parts)


def gen_value(rnd, depth, max_depth):
    r = rnd.random()
    if depth < max_depth and r < 0.35:
        if rnd.random() < 0.5:
            members = [rnd.choice(KEYS) + b":" + gen_value(rnd, depth + 1, max_depth) for _ in range(rnd.randrange(0, 4))]
            return b"{" + b",".join(members) + b"}"
        return b"[" + b",".join(gen_value(rnd, depth + 1, max_depth) for _ in range(rnd.randrange(0, 4))) + b"]"
    if r < 0.6:
        return gen_string(rnd)
    if r < 0.85:
        return rnd.choice(NUMBERS)
    return rnd.choice([b"true", b"false", b"null", b"tru", b"nul", b"NaN", b"Infinity"])


def structured(rnd, n):
    for _ in range(n):
        if rnd.random() < 0.15:          # deep chains around the limit
            k = rnd.randrange(29, 36)
            body = rnd.choice([b"[" * k + b"0" + b"]" * k, b"[" * k + b"]" * k])
            yield b'{"translation":"ok","x":' + body + b"}"
            continue
        members = [rnd.choice(KEYS) + b":" + gen_value(rnd, 2, rnd.randrange(2, 6)) for _ in range(rnd.randrange(0, 5))]
        doc = b"{" + b",".join(members) + b"}"
        r = rnd.random()
        if r < 0.05:
            doc = gen_value(rnd, 1, 3)                     # non-object root
        elif r < 0.10:
            doc = b"\xef\xbb\xbf" + doc
        elif r < 0.15:
            doc = doc[:rnd.randrange(len(doc) + 1)]       # truncated
        elif r < 0.20:
            doc = rnd.choice([b" ", b"\t", b"\x0b", b"\x00"]) + doc + rnd.choice([b"", b"\n", b"\x01", b"}"])
        yield doc


def main():
    binary = sys.argv[1]
    seed = int(sys.argv[sys.argv.index("--seed") + 1]) if "--seed" in sys.argv else 20261106
    rnd = random.Random(seed)
    cases = [c for c, _ in J.CASES] + list(mutations(rnd, 200000)) + list(structured(rnd, 60000))
    payload = b"".join(struct.pack("<I", len(c)) + c for c in cases)
    out = subprocess.run([binary], input=payload, capture_output=True, check=True).stdout.decode().split("\n")
    mismatches, reasons = 0, collections.Counter()
    for data, got in zip(cases, out):
        want = reference(data)
        reasons[want] += 1
        if got != want:
            mismatches += 1
            if mismatches <= 10:
                print(f"MISMATCH C={got} ref={want} input={data[:80]!r}")
    if len([l for l in out if l]) != len(cases):
        mismatches += 1
        print("RESULT COUNT MISMATCH")
    print(f"strict JSON differential: {len(cases)} documents (seed {seed}), mismatches {mismatches}")
    print("reference reasons:", dict(sorted(reasons.items())))
    return 0 if mismatches == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
