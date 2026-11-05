#!/usr/bin/env python3
"""Offline codex exec fixture that supports --output-schema. Never calls a model.

Environment:
  TB_STRUCT_CAPTURE  file: one JSON line per exec run {"argv", "prompt", "schema"}
  TB_STRUCT_REPLY    file: raw bytes written to the -o file (or stdout without -o)
  TB_STRUCT_STDOUT   optional byte count of filler printed to stdout as well
  TB_STRUCT_NO_FILE  if set, the -o file is NOT written (reply goes to stdout)
  TB_STRUCT_SLEEP    optional seconds to sleep before replying
"""
import json
import os
import sys
import time

if "--version" in sys.argv:
    print("codex-cli 0.166.0")
elif "--help" in sys.argv:
    print("exec --ephemeral --ignore-user-config --output-schema --output-last-message")
elif sys.argv[1:3] == ["mcp", "list"]:
    print("[]")
elif sys.argv[1] == "exec":
    prompt = sys.stdin.read()
    schema = None
    if "--output-schema" in sys.argv:
        with open(sys.argv[sys.argv.index("--output-schema") + 1], encoding="utf-8") as fp:
            schema = json.load(fp)
    with open(os.environ["TB_STRUCT_CAPTURE"], "a", encoding="utf-8") as out:
        out.write(json.dumps({"argv": sys.argv[1:], "prompt": prompt, "schema": schema}, ensure_ascii=False) + "\n")
    time.sleep(float(os.environ.get("TB_STRUCT_SLEEP", "0")))
    with open(os.environ["TB_STRUCT_REPLY"], "rb") as fp:
        reply = fp.read()
    filler = int(os.environ.get("TB_STRUCT_STDOUT", "0"))
    if filler:
        sys.stdout.buffer.write(b"x" * filler)
    if "-o" in sys.argv and not os.environ.get("TB_STRUCT_NO_FILE"):
        with open(sys.argv[sys.argv.index("-o") + 1], "wb") as fp:
            fp.write(reply)
    elif not filler:
        sys.stdout.buffer.write(reply)
else:
    sys.exit(1)
