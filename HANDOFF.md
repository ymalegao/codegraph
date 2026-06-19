# CodeGraph Implementation Handoff

## Source of truth

- Specification: `design_spec.md`
- Current milestone completed in this handoff: Step 3, Scanner Tier 0
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

## Verification commands

Run from the repo root:

```bash
cmake --build build
./build/codegraph test-core
./build/codegraph test-storage
./build/codegraph test-scan
./build/codegraph --version
./build/codegraph doctor-deps
./build/codegraph parse-smoke testing/sample.cpp
./build/codegraph parse-smoke testing/sample.py
```

## Next milestone

Step 4: Tree-sitter C++ extraction.

Scope from the spec:

- Parse indexed C++ files with tree-sitter-cpp.
- Extract expected symbols from a known file.
- Store symbol rows with complete spans.
- Store symbol source span hashes.
- Add `File -> Symbol` and nested `Symbol -> Symbol` `Contains` edges.
- Add C++ include/import extraction as `Imports` edges only.
- Populate `fts_symbols`.
- Add minimal tests proving extracted spans match source exactly.

Do not implement calls/references. Do not implement Python indexing.
