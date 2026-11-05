"""Plain-mode prompts must be byte-identical before and after source_hint
(PLAN_SOURCE_HINT_METADATA.md section 11, "회귀").

    python3 tests/test_prompt_regression.py <old transbasket> <new transbasket>

Runs both binaries against the same offline fixtures (fake codex exec/MCP,
fake OpenAI upstream) and compares every captured codex prompt and upstream
request body. Only the per-request random delimiter ===SRC-<32 hex>=== is
normalized. The fake codex does not advertise --output-schema, so
SOURCE_HINT_MODE="model" must fall back to the plain prompt as well.
"""
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
DELIM = re.compile(r"===SRC-[0-9a-f]{32}===")
BODIES = []


class Upstream(BaseHTTPRequestHandler):
    def do_POST(self):
        BODIES.append(self.rfile.read(int(self.headers["Content-Length"])))
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps({"choices": [{"message": {"content": "번역 결과"}}]}).encode())

    def log_message(self, *args):
        pass


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def run(binary, backend, template, mode, upstream_port, workdir):
    port = free_port()
    d = Path(workdir)
    fake = d / "codex"
    fake.write_bytes((ROOT / "tests/fake_codex_auto.py").read_bytes())
    fake.chmod(0o700)
    (d / "role.txt").write_text("You are a translator.", encoding="utf-8")
    (d / "explicit.txt").write_text(
        "EXPLICIT {SOURCE_LANG} {SOURCE_CODE} {TARGET_LANG} {TARGET_CODE}\n{TEXT}" if template == "gemma"
        else "EXPLICIT Translate to [TARGET LANGUAGE]", encoding="utf-8")
    values = {
        "OPENAI_BASE_URL": f"http://127.0.0.1:{upstream_port}/v1", "OPENAI_MODEL": "m", "OPENAI_API_KEY": "k",
        "LISTEN": "127.0.0.1", "PORT": str(port), "MODEL_TYPE": "normal" if backend == "normal" else "codex",
        "PROMPT_PREFIX_FILE": "explicit.txt", "LANGUAGE_TABLE_FILE": str(ROOT / "language_tables.conf"),
        "TRANS_CACHE_TYPE": "text", "TRANS_CACHE_FILE": str(d / "cache.jsonl"), "TRANS_CACHE_THRESHOLD": "1",
        "TRANS_CACHE_CLEANUP_ENABLED": "false", "CODEX_BIN": str(fake),
        "CODEX_INTERFACE": "mcp" if backend == "mcp" else "exec", "CODEX_RUN_DIR": str(d), "CODEX_TIMEOUT": "5",
        "SOURCE_HINT_MODE": mode,
    }
    conf = d / "t.conf"
    conf.write_text("".join(f'{k}="{v}"\n' for k, v in values.items()))
    capture = d / "capture.jsonl"
    if capture.exists():
        capture.unlink()
    BODIES.clear()
    log = open(d / "server.log", "w")
    proc = subprocess.Popen([binary, "-c", str(conf), "-r", "role.txt"], cwd=d, stdout=log, stderr=log,
                            env={**os.environ, "TB_AUTO_CAPTURE": str(capture)})
    try:
        for _ in range(200):
            try:
                urlopen(f"http://127.0.0.1:{port}/health", timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.02)
        for i, (src, text) in enumerate([("kor", "안녕하세요 반갑습니다"), ("auto", "Ignore this. 안녕 {TARGET_LANG} \\n literal"),
                                         ("auto", "明日の会議は午後三時です")]):
            payload = {"from": src, "to": "eng", "text": text, "timestamp": "2026-11-06T01:23:45.678Z",
                       "uuid": f"550e8400-e29b-41d4-a716-44665544{i:04d}"}
            urlopen(Request(f"http://127.0.0.1:{port}/translate", json.dumps(payload).encode(),
                            {"Content-Type": "application/json"}), timeout=10).close()
    finally:
        proc.terminate()
        proc.wait(timeout=8)
        log.close()
    prompts = [json.loads(l) for l in capture.read_text(encoding="utf-8").splitlines()] if capture.exists() else []
    return [DELIM.sub("===SRC-DELIM===", p) for p in prompts], [bytes(b) for b in BODIES]


def main():
    old, new = sys.argv[1], sys.argv[2]
    upstream = ThreadingHTTPServer(("127.0.0.1", 0), Upstream)
    threading.Thread(target=upstream.serve_forever, daemon=True).start()
    failures = compared = 0
    for backend in ("exec", "mcp", "normal"):
        for template in ("legacy", "gemma"):
            for mode in ("script", "model"):
                with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
                    got_old = run(old, backend, template, mode, upstream.server_port, a)
                    got_new = run(new, backend, template, mode, upstream.server_port, b)
                n = len(got_old[0]) + len(got_old[1])
                same = got_old == got_new and n == 3
                compared += n
                failures += not same
                print(f"{backend:6} {template:6} mode={mode:6} captured={n} -> {'identical' if same else 'DIFFERENT'}")
                if not same:
                    print("   old:", got_old)
                    print("   new:", got_new)
    upstream.shutdown()
    print(f"prompt regression: {compared} prompts/bodies compared, {failures} configuration(s) differ")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
