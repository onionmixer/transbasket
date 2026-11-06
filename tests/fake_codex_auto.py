#!/usr/bin/env python3
"""Offline codex exec fixture. Never invokes a model or external service."""
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
else:
    sys.exit(1)
