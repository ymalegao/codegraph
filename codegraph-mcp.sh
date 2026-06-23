#!/usr/bin/env bash
set -euo pipefail
cd /home/yash/codegraph
mkdir -p /home/yash/codegraph/.codegraph/logs
{
  date -Is
  echo "cwd=$(pwd)"
  echo "user=$(id -un)"
  echo "host=$(hostname)"
  ls -l /home/yash/codegraph/build-ninja/codegraph
} >> /home/yash/codegraph/.codegraph/logs/vscode-mcp-wrapper.log 2>&1
exec /home/yash/codegraph/build-ninja/codegraph mcp /home/yash/codegraph
