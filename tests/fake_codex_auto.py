#!/usr/bin/env python3
"""Offline codex exec/MCP fixture. Never invokes a model or external service."""
import json
import os
from pathlib import Path
import sys
import time


def gate(stage):
    """Optional coalescing-test controls; ordinary auto tests remain immediate."""
    gate_path = os.environ.get("TB_INFLIGHT_GATE")
    if not gate_path or os.environ.get("TB_INFLIGHT_STAGE", "reply") != stage:
        return
    pid_path = os.environ.get("TB_INFLIGHT_PIDS")
    if pid_path:
        with open(pid_path, "a") as output:
            output.write(str(os.getpid()) + "\n")
    while not Path(gate_path).exists():
        time.sleep(0.01)


def capture(prompt):
    with open(os.environ["TB_AUTO_CAPTURE"], "a", encoding="utf-8") as output:
        output.write(json.dumps(prompt, ensure_ascii=False) + "\n")


if "--version" in sys.argv:
    print("codex-cli 0.148.0")
elif "--help" in sys.argv:
    print("exec --ephemeral --ignore-user-config")
elif sys.argv[1:3] == ["mcp", "list"]:
    print("[]")
elif sys.argv[1] == "exec":
    gate("stdin")
    capture(sys.stdin.read())
    gate("reply")
    if os.environ.get("TB_INFLIGHT_ERROR"):
        print("ERROR: fixture failure", file=sys.stderr)
        sys.exit(1)
    Path(sys.argv[sys.argv.index("-o") + 1]).write_text("번역 결과", encoding="utf-8")
elif sys.argv[1] == "mcp-server":
    for line in sys.stdin:
        request = json.loads(line)
        if "id" not in request:
            continue
        result = {}
        if request["method"] == "initialize":
            gate("initialize")
        if request["method"] == "tools/call":
            capture(request["params"]["arguments"]["prompt"])
            gate("reply")
            result = {"structuredContent": {"content": "번역 결과"}}
            if os.environ.get("TB_INFLIGHT_ERROR"):
                result = {"isError": True, "structuredContent": {"content": "fixture failure"}}
        print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}), flush=True)
        if request["method"] == "initialize":
            gate("stdin")  # acknowledge init, then stop reading a large tools/call
else:
    sys.exit(1)
