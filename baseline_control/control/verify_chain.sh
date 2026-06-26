#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$root/control/prepare_run.sh" treatment "$root/fixture" "$work/run"

git -C "$work/run" am --3way "$root/control/gold/session-1.patch"
"$root/control/apply_drift.sh" "$work/run" 1
git -C "$work/run" am --3way "$root/control/gold/session-2.patch"
"$root/control/apply_drift.sh" "$work/run" 2
git -C "$work/run" am --3way "$root/control/gold/session-3.patch"

cmake -S "$work/run" -B "$work/build" -G Ninja
cmake --build "$work/build"
ctest --test-dir "$work/build" --output-on-failure
"$root/control/run_hidden.sh" "$work/run"
