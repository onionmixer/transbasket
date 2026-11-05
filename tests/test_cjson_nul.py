"""json_decodes_nul(): specification vs the real cJSON 1.7.15 (oracle), and the
C implementation vs the specification (PLAN_CLEANUP_DAYS_AND_NUL.md section 5 V1, V2).

    python3 -I tests/test_cjson_nul.py obj/test_cjson_nul [--seed S] [--count N]
"""
import argparse
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cjson_nul_ref as R  # noqa: E402


def run_c(binary, inputs):
    payload = b"".join(struct.pack("<I", len(d)) + d for d in inputs)
    out = subprocess.run([binary], input=payload, capture_output=True, check=True).stdout.split()
    if len(out) != len(inputs):
        sys.exit(f"harness returned {len(out)} verdicts for {len(inputs)} inputs")
    return [v == b"1" for v in out]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--seed", type=int, default=20261106)
    ap.add_argument("--count", type=int, default=200000)
    args = ap.parse_args()
    failures = 0
    oracle = R.Oracle()

    # 1. fixed cases: hand-written verdicts == spec, and consistent with the library
    fixed = R.cases()
    for data, want in fixed:
        if R.decodes_nul(data) != want:
            failures += 1
            print(f"SPEC != table: {data!r} table={want}")
        res = oracle.parse(data) if b"\xff" not in data else None
        if res is not None:
            cut, consumed = res
            if R.decodes_nul(data[:consumed]) != cut:
                failures += 1
                print(f"SPEC != cJSON on consumed range: {data!r} cJSON cut={cut}")

    # 2. oracle over the fuzz corpus
    corpus = R.corpus(args.seed, args.count)
    accepted = cut_n = outside = 0
    for data in corpus:
        res = oracle.parse(data)
        if res is None:
            continue
        accepted += 1
        cut, consumed = res
        cut_n += cut
        spec_range = R.decodes_nul(data[:consumed])
        if spec_range != cut:
            failures += 1
            print(f"SPEC != cJSON on consumed range: {data!r} cJSON cut={cut} spec={spec_range}")
        elif cut and not R.decodes_nul(data):
            failures += 1
            print(f"FALSE NEGATIVE on whole input: {data!r}")
        elif not cut and R.decodes_nul(data):
            outside += 1
    print(f"oracle: {len(corpus)} inputs, cJSON accepted {accepted}, cut {cut_n}; "
          f"spec == cJSON on every consumed range: {failures == 0}; whole-input extra positives {outside} (all outside the range)")

    # 3. C vs spec
    inputs = [d for d, _ in fixed] + corpus
    got = run_c(args.binary, inputs)
    mism = [(d, g) for d, g in zip(inputs, got) if g != R.decodes_nul(d)]
    for d, g in mism[:10]:
        print(f"C != SPEC: {d!r} C={g}")
    failures += len(mism)
    print(f"C vs spec: {len(inputs)} inputs ({len(fixed)} fixed), {len(mism)} mismatches")
    print("FAIL" if failures else "OK")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
