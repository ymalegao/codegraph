#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 /path/to/run-worktree 1|2" >&2
    exit 2
fi

repo="$(realpath "$1")"
drift="$2"
case "$drift" in
    1|2) ;;
    *) echo "drift must be 1 or 2" >&2; exit 2 ;;
esac

git -C "$repo" am --3way "$(dirname "$0")/drifts/drift-$drift.patch"
