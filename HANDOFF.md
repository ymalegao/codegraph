# CodeGraph Implementation Handoff

## Source of truth

- Specification: `design_spec.md`
- Current milestone completed in this handoff: Step 5, Exact reads + verify-before-trust
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
- op log
- materializer
- CSR graph
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
./build/codegraph --version
./build/codegraph doctor-deps
./build/codegraph parse-smoke testing/sample.cpp
./build/codegraph parse-smoke testing/sample.py
```

## Next milestone

Step 6: Op log + materializer.

Scope from the spec:

- Create `.codegraph/device_id` and `.codegraph/ops/<device_id>.jsonl`.
- Append `ADD_CORRECTION` and `ADD_DECISION` ops from CLI commands.
- Materialize memory rows from the append-only op log.
- Make materialization idempotent via `op_index`.
- Resolve `affects` references to file/symbol nodes where possible, leaving unresolved edges pending.
- Verify running materialize twice creates no duplicates.

Do not implement memory reads, CSR, MCP, or benchmarks.
