# CodeGraph Implementation Handoff

## Source of truth

- Specification: `design_spec.md`
- Current milestone completed in this handoff: Step 7, Memory reads
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

- CSR graph
- MCP server
- benchmark commands

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
./build/codegraph --version
./build/codegraph doctor-deps
./build/codegraph parse-smoke testing/sample.cpp
./build/codegraph parse-smoke testing/sample.py
```

## Next milestone

Step 8: CSR + sorted indexes.

Scope from the spec:

- Build in-memory graph structures from SQLite.
- Add CSR forward and reverse adjacency.
- Add exact sorted indexes such as `symbol_by_namehash` and `file_by_path`.
- Keep SQLite as correctness source; use CSR/indexes as latency optimization.
- Add benchmark command(s) for lookup and memory reads.

Do not implement MCP yet.
