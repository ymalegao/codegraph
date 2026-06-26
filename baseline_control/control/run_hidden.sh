#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "usage: $0 /path/to/fixture" >&2
    exit 2
fi

fixture="$(realpath "$1")"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cmake -S "$(dirname "$0")/hidden" -B "$work/build" -G Ninja \
    -DFIXTURE_DIR="$fixture"
cmake --build "$work/build"
ctest --test-dir "$work/build" --output-on-failure
