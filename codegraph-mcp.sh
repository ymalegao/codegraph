#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"
mkdir -p "$ROOT/.codegraph/logs"

# Find the codegraph binary across build layouts (macOS/WSL).
BIN=""
for candidate in "$ROOT/build/codegraph" "$ROOT/build-ninja/codegraph"; do
  if [ -x "$candidate" ]; then
    BIN="$candidate"
    break
  fi
done
if [ -z "$BIN" ]; then
  echo "codegraph binary not found in build/ or build-ninja/" >&2
  exit 1
fi

{
  date -Is
  echo "cwd=$(pwd)"
  echo "user=$(id -un)"
  echo "host=$(hostname)"
  echo "bin=$BIN"
} >> "$ROOT/.codegraph/logs/vscode-mcp-wrapper.log" 2>&1 || true

exec "$BIN" mcp "$ROOT"
