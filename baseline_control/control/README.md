# Experiment Controller Assets

Agents must be sandboxed to `../fixture`. This directory contains private
ground truth: hidden tests, drift patches, the drift manifest, and matched
instruction templates.

`run_hidden.sh` copies the fixture to a temporary directory before compiling,
so scoring does not modify the agent worktree.
