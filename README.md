# CodeGraph

CodeGraph indexes a repository into a local `.codegraph/graph.sqlite` database and serves exact, memory-aware code reads through a CLI and MCP stdio server.

## Quick Start

From a fresh clone:

```sh
./setup
./build/codegraph init
./build/codegraph mcp
```

`./setup` installs host dependencies, configures CMake, builds `./build/codegraph`, and runs `codegraph doctor-deps`. CMake/CPM downloads the bundled C++ dependencies during configure, including tree-sitter wrappers, tree-sitter grammars, and `nlohmann/json`.

Useful variants:

```sh
./setup --check
./setup --no-install
./setup --release
./setup --init
```

## Host Requirements

The setup script handles the common paths:

- macOS: Xcode Command Line Tools plus Homebrew packages `cmake`, `ninja`, `pkg-config`, `sqlite`, `xxhash`, and `git`.
- Debian/Ubuntu: `build-essential`, `cmake`, `ninja-build`, `pkg-config`, `libsqlite3-dev`, `libxxhash-dev`, `git`, `curl`, and `ca-certificates`.
- Fedora: `gcc`, `gcc-c++`, `cmake`, `ninja-build`, `pkgconf-pkg-config`, `sqlite-devel`, `xxhash-devel`, `git`, `curl`, and `ca-certificates`.
- Arch: `base-devel`, `cmake`, `ninja`, `pkgconf`, `sqlite`, `xxhash`, `git`, `curl`, and `ca-certificates`.

If the machine uses another package manager, install the equivalent packages and run:

```sh
./setup --no-install
```

## Repository Bootstrap

`codegraph init [path]` is idempotent. It creates `.codegraph/`, writes default config if needed, initializes the SQLite schema, scans the repo, indexes symbols, and materializes memory ops.

The MCP server also self-bootstraps:

```sh
./build/codegraph mcp [path]
```

If `.codegraph` is missing or the database is empty, `mcp` performs the same initialization before serving tools.

## MCP Protocol Smoke Benchmark

Run the low-level protocol benchmark against a disposable fixture:

```sh
python3 benchmarks/mcp_benchmark.py
```

The JSON report defaults to `build/mcp-benchmark.json`. It verifies that the
complete MCP tool surface is advertised, fails immediately on any tool error,
and records per-scenario success, p50/p95/max latency, wire/content bytes, and
an estimated content-token count. The token figure is explicitly an estimate
(`ceil(characters / 4)` by default); raw byte counts are retained so a chosen
model tokenizer can be applied later.

This is a transport/reliability smoke test, not the product benchmark. It does
not demonstrate the value of verified resume. The task-based, cross-session
experiment is specified in
[`docs/verified_resume_experiment.md`](docs/verified_resume_experiment.md).

Useful options:

```sh
python3 benchmarks/mcp_benchmark.py \
  --repetitions 100 \
  --warmup 5 \
  --chars-per-token 4 \
  --output build/mcp-benchmark.json
```

## Agent Lifecycle Hooks

The repository includes shared Codex and Claude Code lifecycle hooks. They
inject verified resume context at session start, refresh after mutations, and
require a semantic handoff before stopping with unhanded-off changes.

See [`docs/agent_session_lifecycle.md`](docs/agent_session_lifecycle.md) for the
design and limitations.

Validate the hook state machine with:

```sh
python3 testing/session_hook_smoke.py
```

## Agent Usage

`CLAUDE.md` and `AGENTS.md` describe the intended tool policy for agents. The short version is: use CodeGraph read tools for exact symbol/file reads and memory, and let hooks or `mcp` bootstrap keep the index current.

## Persisting Memory

`.codegraph/` is ignored by default because the SQLite database, logs, and device identity are machine-local. If you temporarily want to move memory between machines before a dedicated sync format exists, force-add the parts you intend to share:

```sh
git add -f .codegraph
```

That is a pragmatic testing path, not the long-term storage model.
