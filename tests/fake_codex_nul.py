#!/usr/bin/env python3
"""Offline codex fixture for tests/test_nul_guard.py. Never invokes a model.
TB_NUL_EXEC_HEX: bytes written to the exec -o file.
TB_NUL_MCP_HEX: the raw tools/call response line ("@ID@" is replaced by the request id)."""
import json
import os
import sys
from pathlib import Path

if "--version" in sys.argv:
    print("codex-cli 0.148.0")
elif "--help" in sys.argv:
    print("exec --ephemeral --ignore-user-config")
elif sys.argv[1:3] == ["mcp", "list"]:
    print("[]")
elif sys.argv[1] == "exec":
    sys.stdin.read()
    Path(sys.argv[sys.argv.index("-o") + 1]).write_bytes(bytes.fromhex(os.environ["TB_NUL_EXEC_HEX"]))
elif sys.argv[1] == "mcp-server":
    for line in sys.stdin:
        request = json.loads(line)
        if "id" not in request:
            continue
        if request["method"] == "tools/call":
            raw = bytes.fromhex(os.environ["TB_NUL_MCP_HEX"]).replace(b"@ID@", str(request["id"]).encode())
            sys.stdout.buffer.write(raw + b"\n")
            sys.stdout.flush()
        else:
            print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": {}}), flush=True)
else:
    sys.exit(1)
