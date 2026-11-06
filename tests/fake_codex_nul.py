#!/usr/bin/env python3
"""Offline codex fixture for tests/test_nul_guard.py. Never invokes a model.
TB_NUL_EXEC_HEX: bytes written to the exec -o file."""
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
else:
    sys.exit(1)
