#!/usr/bin/env bash
#
# run_dkfs_codex.sh - launch transbasket with the codex (MCP) translation backend.
#
# Usage:
#   ./run_dkfs_codex.sh                 # run in foreground
#   ./run_dkfs_codex.sh -w 8            # extra args are passed through to transbasket
#   ./run_dkfs_codex.sh -d              # run as daemon (transbasket -d)
#
# Requires: built ./transbasket binary, codex CLI on PATH, and `codex login`
# completed for the user that runs this script.
#
set -euo pipefail

# Resolve the directory of this script (project root) and work from there so
# relative paths in the config (cache, run dir, prompt files) resolve correctly.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

CONF="transbasket_dkfs_codex.conf"
BIN="./transbasket"
RUN_DIR="./caches/dkfs/run"

# 1. Binary present and built?
if [[ ! -x "$BIN" ]]; then
    echo "Error: $BIN not found or not executable." >&2
    echo "       Build it first:  make" >&2
    exit 1
fi

# 2. Config present?
if [[ ! -f "$CONF" ]]; then
    echo "Error: config file '$CONF' not found in $SCRIPT_DIR" >&2
    exit 1
fi

# 3. codex CLI available?
if ! command -v codex >/dev/null 2>&1; then
    echo "Error: 'codex' CLI not found on PATH (required for MODEL_TYPE=codex)." >&2
    echo "       Install codex or set CODEX_BIN to its absolute path in $CONF." >&2
    exit 1
fi

# 4. codex login status (warn only - do not block startup)
if ! codex login status >/dev/null 2>&1; then
    echo "Warning: codex does not appear to be logged in." >&2
    echo "         Run:  codex login" >&2
fi

# 5. Ensure the per-request temp-cwd root exists and is private.
mkdir -p "$RUN_DIR"
chmod 700 "$RUN_DIR" 2>/dev/null || true

echo "==========================================="
echo " transbasket - codex (MCP) backend [env: dkfs]"
echo "  config:  $CONF"
echo "  binary:  $BIN"
echo "  run dir: $RUN_DIR"
echo "  codex:   $(command -v codex)"
echo "==========================================="

# Replace this shell with the server (clean signal handling / exit code).
exec "$BIN" -c "$CONF" "$@"
