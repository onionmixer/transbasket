"""Over-long configuration lines and values must be ignored with a warning, not
overflow a stack buffer (the old binary aborted with "buffer overflow
detected" in release builds; debug builds have no fortify check).

    python3 tests/test_config_limits.py [transbasket binary]
"""
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BINARY = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "transbasket")
MAX_VALUE = 511          # MAX_VALUE_LENGTH - 1
MAX_LINE = 1023          # MAX_LINE_LENGTH - 1


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class ConfigLimits(unittest.TestCase):
    def start(self, extra_line):
        d = Path(self.temp.name)
        port = free_port()
        (d / "role.txt").write_text("You are a translator.", encoding="utf-8")
        conf = d / "t.conf"
        conf.write_text(
            f'OPENAI_BASE_URL="http://127.0.0.1:9/v1"\nOPENAI_MODEL="m"\nOPENAI_API_KEY="k"\nLISTEN="127.0.0.1"\n'
            f'PORT="{port}"\nPROMPT_PREFIX_FILE="{ROOT}/PROMPT_PREFIX.txt"\nLANGUAGE_TABLE_FILE="{ROOT}/language_tables.conf"\n'
            f'TRANS_CACHE_TYPE="text"\nTRANS_CACHE_FILE="{d}/c.jsonl"\n' + extra_line + "\nDEBUG=no\n")
        p = subprocess.Popen([BINARY, "-c", str(conf), "-r", "role.txt"], cwd=d,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        status = None
        for _ in range(100):
            if p.poll() is not None:
                break
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=0.2) as r:
                    status = r.status
                    break
            except OSError:
                time.sleep(0.05)
        p.terminate()
        out = p.communicate(timeout=10)[0].decode(errors="replace")
        return status, out

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="tb-config-")
        self.addCleanup(self.temp.cleanup)

    def test_value_at_limit_is_accepted(self):
        status, out = self.start('CODEX_RUN_DIR="' + "c" * MAX_VALUE + '"')
        self.assertEqual(status, 200, out)
        self.assertNotIn("ignored", out)

    def test_over_long_value_is_ignored(self):
        status, out = self.start('CODEX_RUN_DIR="' + "a" * (MAX_VALUE + 1) + '"')
        self.assertEqual(status, 200, out)
        self.assertIn(f"key or value longer than {MAX_VALUE} bytes, ignored", out)
        self.assertNotIn("a" * 64, out, "the ignored value is not echoed")

    def test_over_long_line_is_ignored_entirely(self):
        # The tail of a split line must not be parsed as a line of its own.
        tail = "\nPORT=\"1\""
        status, out = self.start('CODEX_RUN_DIR="' + "b" * (MAX_LINE + 50) + '"' + tail.replace("\n", "X"))
        self.assertEqual(status, 200, out)
        self.assertIn(f"longer than {MAX_LINE} bytes and was ignored", out)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
