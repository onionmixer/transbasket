"""Live verification of SOURCE_HINT_MODE="model" with the real codex CLI
(PLAN_SOURCE_HINT_METADATA.md section 11, approved criteria in section 13 Q4).

    python3 tests/live_source_hint.py --binary ./transbasket --base-config <deploy .conf> \
        --ignore-user-config true|false --workdir <scratch dir> [--latency 12]

The deployment config is copied; only PORT, the cache DB, CODEX_RUN_DIR,
file paths and the source_hint keys are changed (CODEX_* values are kept).
Cases and expected base languages come from tests/live_source_hint_cases.json,
committed before any live run. All numbers are computed here, in Python.
"""
import argparse
import json
import re
import socket
import sqlite3
import statistics
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
LEAK = re.compile(r'^\s*\{|"source_language"|"translation"\s*:|^\s*(translation|번역)\s*[:：]', re.I)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def make_config(base, workdir, name, mode, ignore_user_config):
    base = Path(base).resolve()
    port = free_port()
    run_dir = workdir / f"{name}-run"
    run_dir.mkdir(parents=True, exist_ok=True)
    cache = workdir / f"{name}-cache.db"
    override = {"PORT": str(port), "TRANS_CACHE_TYPE": "sqlite", "TRANS_CACHE_SQLITE_PATH": str(cache),
                "CODEX_RUN_DIR": str(run_dir), "DEBUG": "no", "TRANS_CACHE_CLEANUP_ENABLED": "false",
                "LISTEN": "127.0.0.1", "SOURCE_HINT_MODE": mode,
                "CODEX_IGNORE_USER_CONFIG": "true" if ignore_user_config else "false"}
    lines, seen = [], set()
    for line in base.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^([A-Z_]+)=", line)
        if m and m.group(1) in override:
            seen.add(m.group(1))
            lines.append(f'{m.group(1)}="{override[m.group(1)]}"')
        elif m and m.group(1) in ("PROMPT_PREFIX_FILE", "LANGUAGE_TABLE_FILE", "PROMPT_PREFIX_AUTO_FILE"):
            value = line.split("=", 1)[1].strip().strip('"')
            path = Path(value) if Path(value).is_absolute() else base.parent / value
            lines.append(f'{m.group(1)}="{path}"')
        else:
            lines.append(line)
    lines += [f'{k}="{v}"' for k, v in override.items() if k not in seen]
    conf = workdir / f"{name}.conf"
    conf.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return conf, port, cache, run_dir


class Server:
    def __init__(self, binary, conf, port, workdir, role):
        self.binary, self.conf, self.port, self.workdir, self.role = binary, conf, port, workdir, role

    def __enter__(self):
        self.log = open(self.workdir / (self.conf.stem + ".log"), "w")
        self.proc = subprocess.Popen([str(Path(self.binary).resolve()), "-c", str(self.conf), "-r", str(self.role)],
                                     cwd=self.workdir, stdout=self.log, stderr=self.log)
        for _ in range(300):
            try:
                urlopen(f"http://127.0.0.1:{self.port}/health", timeout=0.3).close()
                return self
            except OSError:
                time.sleep(0.05)
        raise RuntimeError("server did not start")

    def __exit__(self, *exc):
        self.proc.terminate()
        self.proc.wait(timeout=15)
        self.log.close()

    def ask(self, text, target, uid):
        payload = {"from": "auto", "to": target, "text": text, "timestamp": "2026-11-06T01:23:45.678Z",
                   "uuid": f"550e8400-e29b-41d4-a716-{uid:012d}"}
        t0 = time.monotonic()
        try:
            r = urlopen(Request(f"http://127.0.0.1:{self.port}/translate", json.dumps(payload).encode(),
                                {"Content-Type": "application/json"}), timeout=130)
        except HTTPError as e:
            r = e
        with r:
            body = json.load(r)
            return r.status, body, time.monotonic() - t0


def hints(cache):
    db = sqlite3.connect(cache)
    try:
        return {r[0]: (r[1], r[2], r[3]) for r in db.execute(
            "SELECT c.source_text, h.source_hint, h.mixed, h.origin FROM trans_cache c "
            "LEFT JOIN trans_cache_source_hint h ON h.hash = c.hash WHERE c.from_lang = 'auto'")}
    finally:
        db.close()


def target_for(case):
    return "eng" if case["expect"] == "kor" else "kor"


def accuracy_run(args, workdir, cases):
    conf, port, cache, run_dir = make_config(args.base_config, workdir, f"acc-iuc-{args.ignore_user_config}",
                                             "model", args.ignore_user_config == "true")
    with Server(args.binary, conf, port, workdir, args.role) as server:
        with ThreadPoolExecutor(4) as pool:
            results = list(pool.map(lambda c: (c, server.ask(c["text"], target_for(c), c["id"])), cases))
    stored = hints(cache)
    leftovers = sorted(p.name for p in run_dir.iterdir())
    rows = []
    for case, (status, body, secs) in results:
        h = stored.get(case["text"], (None, None, None))
        text = body.get("translatedText", "")
        rows.append({"id": case["id"], "category": case["category"], "expect": case["expect"], "status": status,
                     "hint": h[0], "mixed": h[1], "origin": h[2], "secs": round(secs, 2),
                     # protocol leak = envelope/label text in the translation that the SOURCE did not contain
                     "leak": bool(text and LEAK.search(text) and not LEAK.search(case["text"])),
                     "empty": status == 200 and not text.strip(),
                     "forbidden": bool(case.get("forbid_translation") and text.strip() == case["forbid_translation"])
                                  or bool(case.get("forbid_lang") and h[0] == case["forbid_lang"])})
    single = [r for r in rows if r["category"] == "single"]
    correct = sum(r["hint"] == r["expect"] for r in single)
    summary = {
        "ignore_user_config": args.ignore_user_config,
        "single_correct": correct, "single_total": len(single),
        "single_accuracy_pct": round(100 * correct / len(single), 1),
        "structured_failures": sum(r["status"] != 200 for r in rows),
        "model_origin": sum(r["origin"] == "model" for r in rows),
        "script_fallbacks": sum(r["origin"] == "script" for r in rows),
        "translation_leaks": sum(r["leak"] for r in rows), "empty_translations": sum(r["empty"] for r in rows),
        "injection_followed": sum(r["forbidden"] for r in rows), "run_dir_leftovers": leftovers,
        "pass": correct * 100 >= 95 * len(single) and all(r["status"] == 200 for r in rows)
                and not any(r["leak"] or r["empty"] for r in rows),
    }
    return summary, rows


def latency_run(args, workdir, cases):
    texts = [c for c in cases if c["category"] == "single"][:args.latency]
    plain = make_config(args.base_config, workdir, "lat-plain", "script", args.ignore_user_config == "true")
    model = make_config(args.base_config, workdir, "lat-model", "model", args.ignore_user_config == "true")
    times = {"plain": [], "model": []}
    with Server(args.binary, plain[0], plain[1], workdir, args.role) as sp, \
         Server(args.binary, model[0], model[1], workdir, args.role) as sm:
        for i, case in enumerate(texts):           # interleaved, alternating which goes first
            order = [("plain", sp), ("model", sm)] if i % 2 == 0 else [("model", sm), ("plain", sp)]
            for name, server in order:
                status, _, secs = server.ask(case["text"], target_for(case), 1000 + i)
                if status == 200:
                    times[name].append(secs)
    def stats(v):
        v = sorted(v)
        return {"n": len(v), "median": round(statistics.median(v), 3), "mean": round(statistics.mean(v), 3),
                "p90": round(v[min(len(v) - 1, int(0.9 * len(v)))], 3)}
    s = {k: stats(v) for k, v in times.items()}
    s["median_increase_pct"] = round(100 * (s["model"]["median"] - s["plain"]["median"]) / s["plain"]["median"], 1)
    s["pass"] = s["median_increase_pct"] <= 20 and s["plain"]["n"] == s["model"]["n"] == len(texts)
    return s, times


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--base-config", required=True)
    ap.add_argument("--role", default=str(ROOT / "ROLS.txt"))
    ap.add_argument("--ignore-user-config", choices=["true", "false"], required=True)
    ap.add_argument("--workdir", required=True)
    ap.add_argument("--latency", type=int, default=0)
    args = ap.parse_args()
    workdir = Path(args.workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    cases = json.loads((ROOT / "tests/live_source_hint_cases.json").read_text(encoding="utf-8"))["cases"]
    summary, rows = accuracy_run(args, workdir, cases)
    out = {"accuracy": summary, "rows": rows}
    if args.latency:
        out["latency"], out["latency_samples"] = latency_run(args, workdir, cases)
    (workdir / f"result-iuc-{args.ignore_user_config}.json").write_text(json.dumps(out, ensure_ascii=False, indent=1))
    print(json.dumps({"accuracy": summary, "latency": out.get("latency")}, ensure_ascii=False, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
