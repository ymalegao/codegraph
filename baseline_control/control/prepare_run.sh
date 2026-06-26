#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 baseline|treatment /path/to/fixture /path/to/run" >&2
    exit 2
fi

condition="$1"
source_repo="$(realpath "$2")"
destination="$3"

case "$condition" in
    baseline) agents="AGENTS.baseline.md" ;;
    treatment) agents="AGENTS.treatment.md" ;;
    *) echo "condition must be baseline or treatment" >&2; exit 2 ;;
esac

git clone --quiet --single-branch "$source_repo" "$destination"
git -C "$destination" config user.name "Experiment Runner"
git -C "$destination" config user.email "runner@example.invalid"
cp "$(dirname "$0")/$agents" "$destination/AGENTS.md"
git -C "$destination" add AGENTS.md
git -C "$destination" commit --quiet -m "experiment: install matched operating rules"
