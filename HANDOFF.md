# CodeGraph Implementation Handoff

## Source of truth

- Specification: `design_spec.md`
- Current milestone completed in this handoff: Step 8, CSR + sorted indexes
- Build system: CMake + Ninja
- Executable target: `codegraph`

## Completed milestones

### Step 0: Skeleton

Already complete before this handoff.

- C++20 executable builds.
- SQLite with FTS5 is linked and smoke-tested.
- `git` availability is smoke-tested.
- `nlohmann/json` is linked and smoke-tested.
- `xxHash` is linked and smoke-tested with `XXH64`.
- `cpp-tree-sitter`, `tree-sitter-cpp`, and `tree-sitter-python` are linked and smoke-tested.
- Python remains dependency smoke-test only. No Python indexing has been added.
- Debug sanitizer setup is preserved through the existing `codegraph_sanitizers` target.

### Step 1: Core types + interner

Implemented in:

- `src/core.h`
- `src/core.cpp`
- `src/main.cpp`
- `CMakeLists.txt`

Added:

- Strong integer ID types: `NodeId`, `FileId`, `StringId`
- Enums: `NodeKind`, `SymbolKind`, `EdgeKind`, `Status`
- Structs: `Node`, `SourceSpan`, `SymbolData`, `MemoryData`
- `StringInterner` with intern/dedup/view behavior
- `SourceSpan` pack/unpack helpers
- Manual command: `./build/codegraph test-core`

### Step 2: Storage

Implemented in:

- `src/storage.h`
- `src/storage.cpp`
- `src/main.cpp`
- `CMakeLists.txt`

Added:

- `codegraph::Storage` SQLite RAII wrapper.
- Idempotent full schema initialization for all tables and indexes from spec section 4.
- External-content FTS5 tables:
  - `fts_symbols`
  - `fts_memories`
- Triggers that keep the FTS tables synchronized for insert/update/delete.
- `PRAGMA user_version = 1`.
- Manual command: `./build/codegraph test-storage`

Step 2 intentionally does not add:

- `init`
- `scan`
- `index`
- symbol extraction
- op log
- materializer
- memory reads
- CSR graph
- MCP
- benchmark commands

### Step 3: Scanner Tier 0 + source pruning

Implemented in:

- `src/scanner.h`
- `src/scanner.cpp`
- `src/hash_util.h`
- `src/hash_util.cpp`
- `src/sqlite_util.h`
- `src/sqlite_util.cpp`
- `src/storage.h`
- `src/storage.cpp`
- `src/main.cpp`
- `CMakeLists.txt`

Added:

- Recursive repo walk with default ignores:
  - `.git/**`
  - `build/**`
  - `cmake-build-*/**`
  - `node_modules/**`
  - `**/__pycache__/**`
  - `third_party/**`
  - `generated/**`
- C++ file detection for `.cpp`, `.cc`, `.cxx`, `.h`, and `.hpp`.
- xxHash64 file content hashes stored as lowercase 16-character hex strings.
- Line table construction and little-endian `uint32_t` blob packing.
- Git branch and HEAD commit capture during scans.
- Upserts into `files`.
- Upserts into `line_tables`.
- Unchanged-file detection by stored content hash.
- Shared SQLite statement/bind/check helpers for storage and scanner code.
- Shared xxHash64 hex helper for file hashes and future symbol span hashes.
- Scanner file ownership now comes from `FrontendRegistry` extension mappings instead of `is_cpp_path`.
- A scan reconciles the SQLite source cache with disk:
  - files for registered languages that are no longer seen are pruned
  - stale `line_tables`, `symbols`, source nodes, and `contains`/`imports` edges are removed
  - memory `affects` edges pointing at deleted source nodes are reset to unresolved for the later resolver pass
- CLI command: `./build/codegraph scan`
  - Writes to `.codegraph/graph.sqlite`.
  - Creates the `.codegraph` directory if needed.
- Manual smoke command: `./build/codegraph test-scan`
  - Uses `build/codegraph-test-scan.sqlite`.
  - Verifies `testing/sample.cpp` is scanned.
  - Verifies `testing/sample.py` is not scanned.
  - Verifies line table offsets round-trip.
  - Verifies a second scan detects unchanged files.
  - Verifies no files are pruned on an unchanged second scan.
  - Verifies `symbols` remains empty.

Step 3 intentionally does not add:

- tree-sitter symbol extraction
- source graph nodes or edges
- imports
- FTS symbol population from scanner output
- op log
- materializer
- memory reads
- CSR graph
- MCP
- benchmark commands

### Step 4: Language frontend + Tree-sitter C++ extraction

Implemented in:

- `src/extraction.h`
- `src/frontend.h`
- `src/frontend.cpp`
- `src/cpp_frontend.h`
- `src/cpp_frontend.cpp`
- `src/indexer.h`
- `src/indexer.cpp`
- `src/source_projection.h`
- `src/source_projection.cpp`
- `src/file_util.h`
- `src/file_util.cpp`
- `src/time_util.h`
- `src/time_util.cpp`
- `src/main.cpp`
- `CMakeLists.txt`

Added:

- Language-neutral extraction contract:
  - `LanguageFrontend::language()`
  - `LanguageFrontend::extensions()`
  - `LanguageFrontend::extract() -> ExtractedFile`
- Neutral extraction payloads:
  - `ExtractedFile`
  - `SymbolInfo`
  - `IncludeInfo`
- `FrontendRegistry` as the single source of truth for extension/language ownership.
- C++ tree-sitter parser/extractor isolated in `cpp_frontend.cpp`.
- Generic `index_repository` persistence in `indexer.cpp`; it does not include tree-sitter headers or grammar node-type strings.
- Shared source projection cleanup for changed and deleted files.
- CLI command: `./build/codegraph index`
  - Runs scan first.
  - Parses files through registered language frontends.
  - Writes to `.codegraph/graph.sqlite`.
- Manual smoke command: `./build/codegraph test-index`
  - Uses `build/codegraph-test-index.sqlite`.
  - Verifies expected symbols from `testing/sample.cpp`.
  - Verifies exact byte span for `demo::add`.
  - Verifies `fts_symbols` is populated.
  - Verifies `Contains` edges are created.
  - Verifies `Imports` edges are created for includes.
  - Verifies no edge kinds outside `contains` and `imports`.
  - Verifies Python files are not indexed.
  - Verifies a deleted C++ file is pruned and a following index run does not fail on a stale path.
- Symbol extraction for:
  - `function_definition`
  - methods inside classes/structs
  - out-of-class qualified method definitions
  - `class_specifier`
  - `struct_specifier`
  - `namespace_definition`
  - `enum_specifier`
- Symbol rows with:
  - kind
  - name
  - qualified name
  - signature text for functions/methods
  - exact start/end lines
  - exact start/end bytes
  - xxHash64 span content hash
- Source graph rows:
  - file nodes
  - symbol nodes
  - `Contains` edges
  - unresolved `Imports` edges with `to_ref`

Step 4 intentionally does not add:

- Calls edges
- References edges
- Python indexing
- verify-before-trust
- op log
- materializer
- memory reads
- CSR graph
- MCP
- benchmark commands

### Step 5: Exact reads + verify-before-trust

Implemented in:

- `src/read_tools.h`
- `src/read_tools.cpp`
- `src/main.cpp`
- `CMakeLists.txt`

Added:

- CLI command: `./build/codegraph find-symbol <name>`
  - Finds exact `qualified_name` or `name` matches.
  - Also includes simple qualified-name substring matching for local inspection.
- CLI command: `./build/codegraph search-symbol <query> [--kind K] [--limit N]`
  - Searches `fts_symbols` with BM25 ranking.
  - Sanitizes raw input into quoted FTS terms so punctuation such as `:`, `*`, `-`, and `(` does not throw FTS syntax errors.
  - Supports optional symbol kind filtering.
  - Returns symbol ids, qualified names, file locations, kind, signature, and score.
  - Does not return source bodies; callers should use `read-symbol` for verified reads.
- CLI command: `./build/codegraph read-symbol <name>`
  - Reads a complete symbol body by exact stored byte span when the live file hash matches the cached hash.
  - Re-runs `scan_repository` + `index_repository` internally when the live file hash differs.
  - Returns a fresh span with status `re_resolved` after edits that move a symbol.
  - Reports status `gone` when the symbol disappears after re-resolution.
- CLI command: `./build/codegraph read-file <path> --start N --end M`
  - Returns exact 1-based inclusive line ranges from the live file.
  - Includes the live file hash.
- Manual smoke command: `./build/codegraph test-read`
  - Verifies `find-symbol` finds an indexed test symbol.
  - Verifies `search-symbol` handles unsafe raw FTS punctuation.
  - Verifies `search-symbol` ranks a name hit above a signature-only hit.
  - Verifies `search-symbol` applies limits and returns signatures/locations.
  - Verifies `read-symbol` returns the exact initial body.
  - Verifies `read-file` returns an exact line range.
  - Edits the backing file so the symbol moves and changes body.
  - Verifies `read-symbol` returns status `re_resolved` and the current body/span.
  - Deletes the symbol from the backing file.
  - Verifies `read-symbol` returns status `gone`.

Step 5 intentionally does not add:

- Memory reads
- MCP server
- CSR graph
- benchmark commands

### Step 6: Op log + materializer

Implemented in:

- `src/materializer.h`
- `src/materializer.cpp`
- `src/resolver.h`
- `src/resolver.cpp`
- `src/scanner.cpp`
- `src/main.cpp`
- `CMakeLists.txt`

Added:

- `.codegraph/device_id` creation with a stable local device id.
- `.codegraph/ops/<device_id>.jsonl` append-only op log.
- `ADD_CORRECTION` op appending.
- `ADD_DECISION` op appending.
- Deterministic materialization sorted by `(lamport, device_id)`.
- Idempotency through `op_index`.
- Memory node stable ids as `memory:<device_id>:<lamport>`.
- Memory row projection into `memories`.
- Correction path rule projection into `path_rules`.
- `affects` edge projection into `edges`.
- Resolver pass for:
  - file references: `path`
  - symbol references: `path::qualified_name`
  - unique qualified-name references as a convenience
- Pending unresolved `affects` edges.
- Scanner now upserts File nodes and runs `resolver_pass()` after each scan, so memory edges that reference newly scanned files can resolve.
- CLI command: `./build/codegraph remember --title T --body B --affects P [--affects P2 ...]`
  - Appends an `ADD_DECISION` op.
  - Runs `materialize`.
- CLI command: `./build/codegraph correct --reason R --affects P [--prefer G] [--avoid G] [--title T]`
  - Appends an `ADD_CORRECTION` op.
  - Runs `materialize`.
- CLI command: `./build/codegraph materialize`
  - Replays unapplied ops and runs resolver pass.
- Manual smoke command: `./build/codegraph test-materialize`
  - Verifies correction and decision ops append.
  - Verifies materialized `memories`, `path_rules`, and `affects` edges.
  - Verifies running materialize twice does not duplicate memory rows or edges.
  - Verifies an `affects` edge for a not-yet-indexed path lands pending.
  - Verifies a later `scan` resolves that pending edge once the file exists.

Step 6 intentionally does not add:

- CSR graph
- MCP server
- benchmark commands

### Step 7: Memory reads

Implemented in:

- `src/memory_reads.h`
- `src/memory_reads.cpp`
- `src/main.cpp`
- `CMakeLists.txt`

Added:

- SQLite-backed memory reads for the current milestone.
  - CSR is not required yet; Step 8 remains the latency/indexing milestone.
- Reverse `affects` lookup:
  - Resolve a file/symbol target to a source node.
  - Query `edges` where `kind='affects'` and `to_node` is that source node.
  - Join back to `memories` for correction/decision content.
- Path-rule lookup for corrections:
  - Match `path_rules.pattern` globs against the requested path.
  - Supports `*` and `**` glob behavior.
  - Returns matching `prefer`/`avoid` rule provenance and reason.
- CLI command: `./build/codegraph memory-for <path-or-symbol>`
  - Prints corrections and decisions for a file path or `path::qualified_name`.
  - Shows correction path rules and reasons.
- Manual smoke command: `./build/codegraph test-memory`
  - Verifies `memory-for` on a ResDB-like path shows the prefer correction and reason.
  - Verifies `memory-for` on a BFT-SMaRt-like path shows the avoid correction.
  - Verifies a directly affected file shows an architecture decision.
  - Verifies symbol-target memory includes matching file path rules.

Step 7 intentionally does not add:

- MCP server
- benchmark commands

### Step 8: CSR + sorted indexes

Implemented in:

- `src/graph_store.h`
- `src/graph_store.cpp`
- `src/core.h`
- `src/hash_util.h`
- `src/hash_util.cpp`
- `src/main.cpp`
- `CMakeLists.txt`

Added:

- In-memory `Csr` and `Graph` containers.
- `GraphIndex` snapshot built from SQLite.
- `StringInterner` use for graph titles, file paths, qualified names, and signatures.
- Node vector indexed by SQLite `node_id`, preserving `NodeId == array index`.
- Symbol payload table (`SymbolData`) loaded from `symbols`.
- Memory payload table (`MemoryData`) loaded from `memories`.
- Forward CSR adjacency for resolved edges.
- Reverse CSR adjacency for resolved edges.
- Sorted exact `symbol_by_namehash` index.
  - Includes both unqualified names and qualified names.
- Sorted exact `file_by_path` index.
- CLI command: `./build/codegraph bench lookup <symbol> [repetitions]`
  - Compares exact graph symbol lookup, exact SQL symbol lookup, and `rg` fixed-string text search.
  - Reports `rg_text_count` separately because `rg` is intentionally inexact and can include comments, calls, strings, and docs.
- CLI command: `./build/codegraph bench memory-for <target> [repetitions]`
  - Compares repeated SQL reverse-edge lookup against CSR reverse-edge traversal.
- CLI command: `./build/codegraph bench read <symbol> [repetitions]`
- Manual smoke command: `./build/codegraph test-graph`
  - Builds an indexed fixture with many direct `affects` edges.
  - Runs 10 SQL reverse-edge queries and 10 CSR reverse traversals.
  - Verifies CSR and SQL counts match.
  - Verifies CSR is faster on the fixture.
  - Verifies `symbol_by_namehash` count matches SQL.
  - Verifies exact graph symbol lookup is faster than SQL and `rg` on the fixture.
  - Verifies `file_by_path` contains the fixture path.

Step 8 intentionally does not add:

- MCP server
- doctor command

### Step 9: MCP stdio server

Implemented in:

- `src/mcp_server.h`
- `src/mcp_server.cpp`
- `src/graph_store.h`
- `src/graph_store.cpp`
- `src/memory_reads.h`
- `src/memory_reads.cpp`
- `src/main.cpp`
- `CMakeLists.txt`

Added:

- CLI command: `./build/codegraph mcp`
  - Long-lived newline-delimited JSON-RPC server over stdin/stdout.
  - Emits protocol messages on stdout only.
  - Sends logs to stderr and `.codegraph/logs/mcp.log`.
- Single MCP tool registry used by both `tools/list` and `tools/call`.
- Startup graph snapshot built once with `build_graph_index(storage)`.
- Graph additions:
  - in-memory `FileId -> {path, content_hash}` table
  - file-node-by-path lookup
  - typed symbol accessor from symbol `NodeId`
- Graph-backed tools:
  - `find_symbol`
  - `read_symbol`
  - `read_enclosing_symbol`
  - `get_memory_for_file`
  - `get_memory_for_symbol`
- Disk-backed exact byte tool:
  - `read_file_range`
- SQLite-backed tools where the graph cannot serve the data:
  - `search_symbol` via FTS5/BM25
  - memory body/path-rule hydration
- Write tools:
  - `record_correction`
  - `record_decision`
  - Both append ops, materialize, return `node_id`, and rebuild the graph snapshot.
- Manual smoke command: `./build/codegraph test-mcp`
  - Runs `codegraph mcp` as a subprocess in an isolated throwaway repo.
  - Verifies stdout is parseable JSON-RPC responses only.
  - Verifies `initialize`, `tools/list`, graph `find_symbol`, graph+disk `read_symbol`, graph `read_enclosing_symbol`, graph+SQLite memory reads, FTS `search_symbol`, and write-triggered graph rebuild.

Step 9 intentionally does not add:

- HTTP transport
- MCP resources/prompts

### Step 10: launch-anywhere + hooks + auto-freshness

Implemented in:

- `src/bootstrap.h`
- `src/bootstrap.cpp`
- `src/scanner.h`
- `src/scanner.cpp`
- `src/indexer.h`
- `src/indexer.cpp`
- `src/mcp_server.cpp`
- `src/main.cpp`
- `.claude/settings.json`
- `CLAUDE.md`
- `CMakeLists.txt`

Added:

- CLI command: `./build/codegraph init [path]`
  - Creates `.codegraph/`, `.codegraph/ops/`, and `.codegraph/logs/`.
  - Creates `.codegraph/device_id`.
  - Writes `.codegraph/config.yaml` if missing.
  - Initializes SQLite schema.
  - Runs scan, index, and materialize.
  - Is idempotent.
- Config loading:
  - `repo_id`
  - `ignore`
  - `max_file_size_mb`
- `scan` and `index` accept optional `[path]`.
- `scan_repository` uses config-provided ignore patterns and keeps the prior defaults when none are supplied.
- `scan_repository` captures Git branch/commit best-effort; non-Git directories still index.
- `index` now runs materialize after indexing so pending memory edges resolve without a manual materialize step.
- Incremental `index_repository` skip:
  - skips files whose symbol projection already matches the current file content hash
  - reparses changed files and files without a current projection
- `codegraph mcp [path]`
  - accepts optional repo path
  - self-bootstraps when `.codegraph` is missing or the DB has zero files
  - builds the graph after bootstrap
- MCP auto-freshness:
  - tracks SQLite `PRAGMA data_version`
  - rebuilds the in-memory graph before read tools when another process committed changes
  - keeps explicit graph rebuilds after `record_*` writes
- Claude Code project hooks in `.claude/settings.json`:
  - `SessionStart`: `init`
  - `UserPromptSubmit`: `index`
  - `PostToolUse` for `Edit|Write|MultiEdit`: `index`
  - hook output goes to `.codegraph/logs/hook.log`
- `CLAUDE.md` policy:
  - use CodeGraph MCP reads before raw reads
  - discovery-to-retrieval workflow
  - memory checks before edits
  - durable memory guidance
  - do not manually run scan/index/materialize
- `AGENTS.md` policy for Codex:
  - mirrors the CodeGraph MCP usage guidance for Codex agents
  - prefers exact CodeGraph reads before raw file reads
  - documents discovery-to-retrieval and durable memory rules
- MCP fixes:
  - ReResolved symbol kind uses shared kind vocabulary
  - `read_file_range` rejects zero line defaults consistently
- Manual validation:
  - `codegraph init` works in a standalone non-Git temp directory.
  - a long-lived `codegraph mcp` process sees a separate `codegraph index` process through `PRAGMA data_version`.

Step 10 intentionally does not add:

- background file watcher
- sync commands
- incremental graph patching
- new language frontends

### Step 11: doctor + bench + acceptance tests

Implemented in:

- `src/main.cpp`
- `HANDOFF.md`
- `design_spec.md`

Added:

- CLI command: `./build/codegraph doctor [path]`
  - Checks schema version.
  - Checks file/line-table consistency.
  - Checks symbols reference existing files.
  - Checks node kind/status vocabulary.
  - Checks edge endpoints.
  - Checks memories/path rules reference existing nodes.
  - Checks FTS row-count drift for symbols and memories.
  - Returns nonzero if any integrity check fails.
- CLI command: `./build/codegraph bench index [repetitions]`
  - Runs the actual scan+index incremental path.
  - Reports changed and unchanged file counts plus elapsed time.
- Manual smoke command: `./build/codegraph test-acceptance`
  - Runs the §13 acceptance suite.
  - Reuses the existing scan/index/read/memory/materialize tests.
  - Adds the missing two-op-stream materialization check.
  - Runs `doctor` on an acceptance fixture.
  - Invokes the real `codegraph bench` subcommands for lookup, memory-for, read, and index performance checks.

Step 11 intentionally does not add:

- new benchmark storage format
- CI integration
- background daemon

### Step 12: list_symbols_in_file MCP tool (first v2 increment)

Implemented in:

- `src/graph_store.h`
- `src/graph_store.cpp`
- `src/mcp_server.cpp`
- `src/main.cpp`

Added:

- `graph_symbols_in_file(index, file_node)` in the graph layer.
  - Recursive forward-`Contains` walk from a file node.
  - Returns `{symbol_node, parent_node}` pairs for every symbol in the file.
  - `parent_node` is the file node for top-level symbols, the enclosing symbol node otherwise.
  - Peer to `graph_symbols_by_name_hash`; keeps traversal in the graph layer.
- MCP tool `list_symbols_in_file`.
  - Args: `{ "path": string }`.
  - Resolves the file node, calls `graph_symbols_in_file`, serializes each via
    `symbol_json`, attaches `parent_node_id`, and sorts by `start_line`.
  - Returns `{ path, file_node_id, count, symbols[] }` (flat, recursive).
  - Registered in `tool_registry()` and added to `read_tool_name()` so the
    external-change graph rebuild fires before it runs.
- Test coverage added to `test-mcp` (response id 11).
  - Asserts the nested `mcpstep::target` is returned (a flat top-level walk
    would drop it), proving recursion.
  - Asserts its `parent_node_id` equals the `mcpstep` namespace node, not the
    file node, proving parent tracking.
  - Asserts `start_line` ordering.

Known caveat:

- Anonymous-namespace / file-local symbols are not yet extracted, so they are
  absent from the listing. This is an upstream extraction gap (the in-tree
  `cpp_frontend.cpp` work targets it), not a defect in this tool.

Not changed (deliberately):

- `enclosing_symbol` remains inline in `mcp_server.cpp`. It is now the only
  Contains-traversal not living in `graph_store`; consolidating it is a clean
  future cleanup, left out to keep this change surgical.

## Verification commands

Run from the repo root:

```bash
cmake --build build
./build/codegraph test-core
./build/codegraph test-storage
./build/codegraph test-scan
./build/codegraph test-index
./build/codegraph test-read
./build/codegraph test-materialize
./build/codegraph test-memory
./build/codegraph test-graph
./build/codegraph test-mcp
./build/codegraph test-acceptance
./build/codegraph init /tmp/some-repo
./build/codegraph doctor
./build/codegraph bench index 1
./build/codegraph --version
./build/codegraph doctor-deps
./build/codegraph parse-smoke testing/sample.cpp
./build/codegraph parse-smoke testing/sample.py
```

## Next milestone: v2

Thesis from dogfooding the MCP tools: CodeGraph is currently "nice to have."
To become "need to have" for agents, three things must hold, in order:

1. **Trust / completeness (precondition).** An empty result must mean "does not
   exist," not "extraction missed it." Until then, agents double-check with `rg`
   and the tool is redundant. Requires closing extraction gaps AND making tools
   report unindexed paths/languages explicitly instead of silent-empty.
   - Evidence: `find_symbol("tool_registry")` returned empty because it lives in
     an anonymous namespace (extraction gap), indistinguishable from "absent."
2. **Discovery by intent (entry point).** `search_symbol` indexes only
   names+signatures with whole-token matching, so intent queries
   ("register mcp tool", "tools list dispatch") return empty. Agents navigate by
   intent, so this must work for CodeGraph to be the first call, not the second.
3. **Accumulated memory (the moat).** Corrections/decisions attached to symbols,
   surfaced at edit time, are the one thing `rg`/LSP cannot do and the only part
   that compounds with use.

### v2 work, sequenced

- **A. Trust (precondition).**
  - Land the in-tree `cpp_frontend.cpp` anonymous-namespace extraction fix
    (build + reindex). This also un-hides symbols from `list_symbols_in_file`.
  - Coverage honesty: tools report when a path/language is not indexed instead
    of returning empty.
  - Fix `doctor`'s `handoff`-node vocabulary (currently fails `nodes_bad_kind`
    on the live DB).
- **B. Intent discovery + memory moat (shared FTS infra, build together).**
  - Source-text FTS search returning verified ranges.
  - `find_prior_incidents`: FTS over correction/decision bodies with provenance.
  - Same `fts_*` + trigger pattern as existing FTS tables.
- **C. Benchmark harness (do before the call/reference graph).**
  - Tool-vs-no-tool: measure tokens + task success with vs without CodeGraph
    over the existing tools (list/search/read/memory). First data point:
    `list_symbols_in_file` (one call) vs `rg` + N reads.
  - Rationale: the benchmark is the instrument that decides whether and *where*
    the expensive Tier 2 work earns its cost. It is also the corpus that
    surfaces "tree-sitter handed back wrong/missing references — precisely here."
- **C. Intent discovery + memory moat (shared FTS infra, build together).**
  - Source-text FTS search returning verified ranges.
  - `find_prior_incidents`: FTS over correction/decision bodies with provenance.
  - Same `fts_*` + trigger pattern as existing FTS tables.
- **D. Reference/call graph (tiered, gated on the benchmark).**
  - **Tier 1 (tree-sitter, syntactic).** Frontend emits `Calls`/`References`
    edges. Edge schema carries a `resolution` field (`syntactic` | `resolved`)
    from day one so Tier 2 is a data-quality upgrade, not a schema migration.
  - Run the benchmark against Tier 1 to quantify wrong/missing references.
  - **Tier 2 (clangd for C++, semantic).** Upgrade edges to `resolved` where a
    `compile_commands.json` is available; degrade gracefully to Tier 1
    `syntactic` edges when clangd or the compile DB is absent — the tool must
    not break. CMake already gives this repo a compile DB via
    `CMAKE_EXPORT_COMPILE_COMMANDS=ON`, so validate Tier 2 on CodeGraph's own
    source first.
  - Decoupled from Python: clangd Tier 2 does not wait for the Python frontend.
    Pyright Tier 2 rides with the Python frontend whenever that lands.

This sequence is the recommended ordering; revise as priorities shift.

Do not add new language frontends yet.
