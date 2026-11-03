#!/usr/bin/env python3
"""Offline codex exec/MCP fixture. Never invokes a model or external service."""
import json
import os
from pathlib import Path
import sys


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
    capture(sys.stdin.read())
    Path(sys.argv[sys.argv.index("-o") + 1]).write_text("번역 결과", encoding="utf-8")
elif sys.argv[1] == "mcp-server":
    for line in sys.stdin:
        request = json.loads(line)
        if "id" not in request:
            continue
        result = {}
        if request["method"] == "tools/call":
            capture(request["params"]["arguments"]["prompt"])
            result = {"structuredContent": {"content": "번역 결과"}}
        print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}), flush=True)
else:
    sys.exit(1)
