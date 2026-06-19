# CodeGraph Implementation Handoff

## Source of truth

- Specification: `design_spec.md`
- Current milestone completed in this handoff: Step 4, Tree-sitter C++ extraction
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

### Step 3: Scanner Tier 0

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
- CLI command: `./build/codegraph scan`
  - Writes to `.codegraph/graph.sqlite`.
  - Creates the `.codegraph` directory if needed.
- Manual smoke command: `./build/codegraph test-scan`
  - Uses `build/codegraph-test-scan.sqlite`.
  - Verifies `testing/sample.cpp` is scanned.
  - Verifies `testing/sample.py` is not scanned.
  - Verifies line table offsets round-trip.
  - Verifies a second scan detects unchanged files.
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

### Step 4: Tree-sitter C++ extraction

Implemented in:

- `src/cpp_indexer.h`
- `src/cpp_indexer.cpp`
- `src/file_util.h`
- `src/file_util.cpp`
- `src/time_util.h`
- `src/time_util.cpp`
- `src/main.cpp`
- `CMakeLists.txt`

Added:

- C++ tree-sitter parser/extractor isolated in `cpp_indexer.cpp`.
- CLI command: `./build/codegraph index`
  - Runs scan first.
  - Parses scanned C++ files.
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
- exact read commands
- verify-before-trust
- op log
- materializer
- memory reads
- CSR graph
- MCP
- benchmark commands

## Verification commands

Run from the repo root:

```bash
cmake --build build
./build/codegraph test-core
./build/codegraph test-storage
./build/codegraph test-scan
./build/codegraph test-index
./build/codegraph --version
./build/codegraph doctor-deps
./build/codegraph parse-smoke testing/sample.cpp
./build/codegraph parse-smoke testing/sample.py
```

## Next milestone

Step 5: Exact reads + verify-before-trust.

Scope from the spec:

- Read a symbol by name.
- Verify the backing file hash before returning source.
- If the file hash changed, re-parse and re-resolve before returning.
- Return fresh spans flagged `ReResolved`.
- Report when a deleted symbol is gone.
- Add exact file-range reads if needed by the read flow.

Do not implement op log, materializer, memory reads, CSR, MCP, or benchmarks.
