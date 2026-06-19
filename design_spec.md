# CodeGraph Memory — Implementation Specification

## 1. What you are building

A **local-first code memory engine** for a single code repository, written in **C++20**, exposed over a **command-line interface (CLI)** and a **Model Context Protocol (MCP) stdio server**.

It does two things:

1. **Exact source-span retrieval.** It indexes a C++ repository with tree-sitter and can return the *complete, current* source of any symbol (function, class, method, etc.) — exact file path, line range, and byte range — verified against the live file on every read. No fuzzy chunks.
2. **Code-anchored project memory.** A user (via CLI) or an agent (via MCP) can record durable notes — **corrections** (prefer/avoid path rules) and **architecture decisions** — and attach them to specific files or symbols. When something later reads a file or symbol, the relevant notes come back with it.

Everything is local. The repository is the source of truth for code; an append-only log is the source of truth for memory. SQLite is a rebuildable cache of both.

---

## 2. Core model

### Two halves of one graph
- **Source graph** (derived from code, rebuildable): `File` and `Symbol` nodes; `Contains` and `Imports` edges. Written by the scanner straight into SQLite.
- **Memory graph** (authored, durable): `Correction` and `ArchDecision` nodes; `Affects` edges pointing from a memory node to the `File`/`Symbol` it concerns. Written only by the materializer, by folding the append-only op log.

### The invariant (do not violate)
> **Only the op log is authoritative for memory. Every row in SQLite — source nodes, memory nodes, and all edges — is a rebuildable projection of (op log + a fresh scan of the repo).**

Consequences:
- The scanner can wipe and rebuild all source rows from the repo at any time.
- The materializer can wipe and rebuild all memory rows by replaying the op log from empty.
- A memory note records a **symbolic reference** (a string like `"src/foo.cpp::Bar::baz"`), never a numeric node id. The materializer resolves that string to a node locally. This is why memory survives re-indexing and would survive copying the op log to another machine.

### Three layers
```
   CLI / MCP write tools            scanner (tree-sitter)
            │ append                        │ upsert
            ▼                               ▼
     Op log (JSONL, append-only)      (no op log; rebuildable)
            │ fold (materializer)           │
            ▼                               ▼
   ┌──────────────────────────────────────────────────┐
   │ SQLite + FTS5  (queryable cache of both halves)    │
   └───────────────────────┬────────────────────────────┘
                           │ build at startup / on change
                           ▼
   In-memory: string interner · node array · CSR (forward+reverse) · sorted indexes
                           ▲
                           │ CLI commands / MCP tools
```

---

## 3. On-disk layout

```
.codegraph/
  config.yaml
  graph.sqlite
  device_id                 # stable random id for this machine
  ops/
    <device_id>.jsonl       # append-only op log for this machine
  logs/
    mcp.log
```

`config.yaml` minimal fields: `repo_id`, `ignore` (glob list), `max_file_size_mb`.
Default ignores: `.git/**`, `build/**`, `cmake-build-*/**`, `node_modules/**`, `**/__pycache__/**`, `third_party/**`, `generated/**`.

---

## 4. SQLite schema

```sql
-- ---- source half (written by scanner; rebuildable) ----
CREATE TABLE files (
  file_id      INTEGER PRIMARY KEY,
  path         TEXT UNIQUE NOT NULL,
  language     TEXT NOT NULL,
  content_hash TEXT NOT NULL,          -- xxHash64 of file bytes, hex
  size_bytes   INTEGER NOT NULL,
  line_count   INTEGER NOT NULL,
  commit_hash  TEXT,
  indexed_at   TEXT NOT NULL
);

CREATE TABLE line_tables (             -- byte offset of the start of each line
  file_id      INTEGER PRIMARY KEY,
  offsets_blob BLOB NOT NULL,          -- packed little-endian uint32[]
  FOREIGN KEY(file_id) REFERENCES files(file_id)
);

CREATE TABLE symbols (
  symbol_id      INTEGER PRIMARY KEY,
  file_id        INTEGER NOT NULL,
  kind           TEXT NOT NULL,        -- function|method|class|struct|namespace|enum|field|other
  name           TEXT NOT NULL,
  qualified_name TEXT NOT NULL,        -- e.g. ns::Class::method
  signature      TEXT,
  start_line     INTEGER NOT NULL,
  end_line       INTEGER NOT NULL,
  start_byte     INTEGER NOT NULL,
  end_byte       INTEGER NOT NULL,
  content_hash   TEXT NOT NULL,        -- xxHash64 of the span bytes
  commit_hash    TEXT,
  FOREIGN KEY(file_id) REFERENCES files(file_id)
);
CREATE INDEX idx_sym_name ON symbols(name);
CREATE INDEX idx_sym_qual ON symbols(qualified_name);
CREATE INDEX idx_sym_file ON symbols(file_id);

-- ---- graph (nodes for both halves; edges support unresolved targets) ----
CREATE TABLE nodes (
  node_id    INTEGER PRIMARY KEY,
  stable_id  TEXT UNIQUE NOT NULL,     -- see §6 stable-id rules
  kind       TEXT NOT NULL,            -- file|symbol|correction|arch_decision
  title      TEXT NOT NULL,
  created_at TEXT NOT NULL,
  status     TEXT NOT NULL DEFAULT 'active'
);
CREATE INDEX idx_nodes_kind ON nodes(kind);

CREATE TABLE edges (
  edge_id   INTEGER PRIMARY KEY,
  from_node INTEGER NOT NULL,
  to_node   INTEGER,                   -- NULL while unresolved
  to_ref    TEXT,                      -- symbolic reference, kept until resolved
  kind      TEXT NOT NULL,             -- contains|imports|affects
  resolved  INTEGER NOT NULL DEFAULT 0,
  FOREIGN KEY(from_node) REFERENCES nodes(node_id)
);
CREATE INDEX idx_edges_from       ON edges(from_node);
CREATE INDEX idx_edges_to         ON edges(to_node);
CREATE INDEX idx_edges_unresolved ON edges(resolved);

-- ---- memory half (materialized from op log) ----
CREATE TABLE memories (
  memory_id   INTEGER PRIMARY KEY,
  node_id     INTEGER NOT NULL,
  memory_type TEXT NOT NULL,           -- correction|arch_decision
  title       TEXT NOT NULL,
  body        TEXT NOT NULL,
  created_at  TEXT NOT NULL,
  FOREIGN KEY(node_id) REFERENCES nodes(node_id)
);

CREATE TABLE path_rules (
  rule_id   INTEGER PRIMARY KEY,
  node_id   INTEGER NOT NULL,          -- the Correction node that owns this rule
  rule_kind TEXT NOT NULL,             -- prefer|avoid
  pattern   TEXT NOT NULL,             -- glob, e.g. resdb/**
  reason    TEXT,
  FOREIGN KEY(node_id) REFERENCES nodes(node_id)
);

CREATE TABLE op_index (                -- which ops have been applied (idempotency)
  op_id      TEXT PRIMARY KEY,
  device_id  TEXT NOT NULL,
  lamport    INTEGER NOT NULL,
  op_type    TEXT NOT NULL,
  applied_at TEXT NOT NULL
);

-- ---- search ----
CREATE VIRTUAL TABLE fts_symbols  USING fts5(name, qualified_name, signature,
  content='symbols', content_rowid='symbol_id');
CREATE VIRTUAL TABLE fts_memories USING fts5(title, body,
  content='memories', content_rowid='memory_id');
```

Maintain the external-content FTS tables with triggers (or rebuild them after each scan/materialize). Do not let them drift.

---

## 5. In-memory data structures (C++)

Everything is index-based — no pointers between graph elements. A `NodeId` is an index into a flat array; it stays valid across reallocation and serializes with no fixup. Deletions tombstone in place so `NodeId == array index` stays true.

```cpp
// strong integer ids (zero-cost; prevent mixing id spaces at compile time)
enum class NodeId   : uint32_t { Invalid = 0xFFFFFFFFu };
enum class FileId   : uint32_t {};
enum class StringId : uint32_t {};

enum class NodeKind   : uint8_t { File, Symbol, Correction, ArchDecision };
enum class SymbolKind : uint8_t { Function, Method, Class, Struct,
                                  Namespace, Enum, Field, Other };
enum class EdgeKind   : uint8_t { Contains, Imports, Affects };
enum class Status     : uint8_t { Active, Tombstoned, Stale };

// hot node record (one per node, kept small and contiguous)
struct Node {
    NodeKind kind;
    Status   status;
    uint16_t flags;
    StringId title;     // interned identifier, NOT long prose
    uint32_t payload;   // index into the per-kind component table below
};

// packed span — an attribute of a Symbol, not its own node
struct SourceSpan {
    uint32_t start_line, end_line;
    uint32_t start_byte, end_byte;
    uint64_t content_hash;   // xxHash64 of the span bytes (change detection, non-crypto)
    uint16_t flags;          // bit flags: Complete | Approximate | Stale | ReResolved
};

// per-kind component tables, indexed by Node.payload
struct SymbolData {
    FileId     file;
    SymbolKind sym_kind;
    StringId   qualified_name;   // interned
    StringId   signature;        // interned
    SourceSpan span;
};
struct MemoryData {
    uint8_t  memory_type;        // 0 = correction, 1 = arch_decision
    int64_t  body_rowid;         // long prose lives in SQLite memories.body, loaded lazily
};

// CSR adjacency: integer indices only. Forward = "who I point to",
// reverse = "who points to me" (the latter answers "what affects this file").
struct Csr {
    std::vector<uint32_t> offsets;    // size = num_nodes + 1
    std::vector<NodeId>   neighbors;  // size = num_edges
    std::vector<EdgeKind> kinds;      // parallel to neighbors
};
struct Graph {
    std::vector<Node>       nodes;     // indexed by NodeId
    std::vector<SymbolData> symbols;   // indexed by Node.payload (Symbol nodes)
    std::vector<MemoryData> memos;     // indexed by Node.payload (memory nodes)
    Csr forward, reverse;
};
// out-neighbors of v:
//   uint32_t s = forward.offsets[(uint32_t)v], e = forward.offsets[(uint32_t)v + 1];
//   for (uint32_t i = s; i < e; ++i) use(forward.neighbors[i], forward.kinds[i]);

// intern hot, repeated identifiers: paths, qualified names, signatures.
// (Long unique prose is NOT interned — it lives in SQLite TEXT.)
class StringInterner {
    std::vector<char>     blob_;
    std::vector<uint32_t> starts_;   // StringId -> offset; length = next start - this start
    std::unordered_map<std::string_view, StringId> lookup_;
public:
    StringId intern(std::string_view);
    std::string_view view(StringId) const;
};

// exact point lookups: sorted vectors + binary search (O(log n)).
// Fuzzy / keyword search uses SQLite FTS5; these are exact indexes only.
std::vector<std::pair<uint64_t, NodeId>> symbol_by_namehash;  // sorted; std::lower_bound
std::vector<std::pair<StringId, FileId>> file_by_path;        // sorted
```

The in-memory `Graph`, interner, CSR, and sorted indexes are built from SQLite at startup and rebuilt when the data changes. For lookups and memory queries you may read SQLite directly first; build and use the CSR for graph traversal (it is the latency optimization, not a correctness requirement).

---

## 6. Op log

Append-only JSONL. One JSON object per line. Each op:

```json
{
  "op_id":     "<device_id>:<counter>",
  "device_id": "<device_id>",
  "lamport":   137,
  "created_at":"2026-06-18T20:10:00Z",
  "op_type":   "ADD_CORRECTION",
  "payload":   { ... }
}
```

`lamport` is a monotonic per-device counter. Apply order across devices is `(lamport, device_id)`. `op_id` makes application idempotent.

### Op types

**`ADD_CORRECTION`** — a durable prefer/avoid rule, scoped by path/symbol.
```json
{ "title": "Use ResDB, not BFT-SMaRt",
  "reason": "Current implementation uses ResDB app + gossip plumbing.",
  "prefer_paths": ["resdb/**"],
  "avoid_paths":  ["bftsmart/**"],
  "affects":      ["resdb/app/intersection_schedule.cc"] }
```

**`ADD_DECISION`** — a durable architecture decision / project rule.
```json
{ "title": "Move packet parsing out of Session",
  "body":  "Session owns socket lifetime; PacketParser owns protocol decoding. Do not reintroduce parsing into Session.",
  "affects": ["network/Session.cpp", "network/PacketParser.cpp"] }
```

### Stable-id rules
```
File node:        file:<repo_id>:<normalized_path>
Symbol node:      symbol:<repo_id>:<qualified_name>:<path>
Memory node:      memory:<device_id>:<counter>      (event-created, not content-derived)
```

---

## 7. Materializer

The **only** writer of memory rows. A deterministic, idempotent fold over the op log.

```
materialize():
  ops = read every line of .codegraph/ops/*.jsonl
  ops = [o for o in ops if o.op_id not in op_index]      # unapplied only
  sort ops by (lamport, device_id)
  for op in ops:
      apply(op)
      insert op_index(op_id, ...)                         # mark applied
  resolver_pass()                                          # fill unresolved edges
  rebuild_in_memory()                                      # interner, nodes, CSR, indexes

apply(op):  # re-applying an already-indexed op_id must be a no-op
  ADD_CORRECTION:
     insert nodes(kind='correction', stable_id, title)
     insert memories(memory_type='correction', title, body=reason)
     insert path_rules for each prefer_path (rule_kind='prefer') and avoid_path ('avoid')
     for ref in affects:   add_edge(correction_node, kind='affects', to_ref=ref)
  ADD_DECISION:
     insert nodes(kind='arch_decision', stable_id, title)
     insert memories(memory_type='arch_decision', title, body)
     for ref in affects:   add_edge(decision_node, kind='affects', to_ref=ref)

add_edge(from_node, kind, to_ref):
  target = resolve(to_ref)
  if target found: insert edges(from_node, to_node=target, kind, resolved=1)
  else:            insert edges(from_node, to_ref=to_ref, kind, resolved=0)   # pending

resolve(ref):
  "path"               -> File node whose path == ref
  "path::qualified"    -> Symbol node by (file from path, qualified_name)
                          · multiple matches (overloads) -> the File node (degrade), or all
                          · no match -> None  (edge stays pending)

resolver_pass():       # run after every scan and every materialize
  for e in edges where resolved = 0:
      t = resolve(e.to_ref)
      if t: set e.to_node = t, e.resolved = 1

full_replay():         # recovery / migration
  delete all memory rows (nodes of memory kinds, memories, path_rules, memory edges, op_index)
  materialize()        # from empty
```

`scan` writes/updates source rows and source edges (`contains`, `imports`) directly — it does **not** go through the op log. After a scan, always run `resolver_pass()` so memory edges that referenced not-yet-indexed code get resolved.

Import/merge of an op log from elsewhere is just: append the lines, then `materialize()`. Idempotency by `op_id` and deterministic order make this safe with no overwrite.

---

## 8. Scanner, language frontends, and extraction

Keep the parser abstraction language-neutral. A language frontend owns grammar details and turns source bytes into a neutral `ExtractedFile`; the generic indexer owns all persistence and never names tree-sitter types, SQLite parser details, or grammar node-type strings.

```cpp
struct SymbolInfo {
    std::string kind;            // shared vocabulary: function|method|class|struct|namespace|enum|field|other
    std::string name;
    std::string qualified_name;
    std::string signature;
    uint32_t start_line, end_line;
    uint32_t start_byte, end_byte;
    std::string content_hash;
    int parent_index = -1;       // index into ExtractedFile.symbols, or -1 for file-level
};

struct IncludeInfo {
    std::string target;          // import target as written
};

struct ExtractedFile {
    std::vector<SymbolInfo> symbols;
    std::vector<IncludeInfo> includes;
};

class LanguageFrontend {
public:
    virtual ~LanguageFrontend() = default;
    virtual std::string_view language() const = 0;
    virtual std::span<const std::string_view> extensions() const = 0;
    virtual ExtractedFile extract(std::string_view source) const = 0;
};
```

The pure virtual interface stays narrow: `language()`, `extensions()`, and `extract()`. Do not leak SQLite types, `TSNode`, tree-sitter parser handles, or grammar-specific node-type strings into these signatures. Adding a language should mean adding a new frontend and registering it, not editing the generic indexer.

```
scan():
  read git branch + HEAD commit (shell out to `git`)
  walk repo, skip ignored globs and files > max_file_size_mb
  for each file:
     choose language by FrontendRegistry extension ownership
     read bytes
     content_hash = xxHash64(bytes)
     if files.content_hash unchanged: skip line-table rewrite
     else:
        build line-offset table (byte offset of each line start); store packed uint32[]
        upsert files row
     upsert File node
  prune any registered-language files rows not seen in this scan:
     delete line_tables row
     delete symbols and Symbol nodes for that file
     delete source Contains/Imports edges for the file and its symbols
     delete the File node and files row
     reset memory Affects edges that pointed at deleted source nodes back to unresolved
  resolver_pass()
```

Indexing is generic:

```
index_repository(storage, registry):
  scan() has already reconciled files with disk
  for each frontend in registry:
     load files WHERE language = frontend.language()
     for each file:
        if the file is missing anyway, prune its source projection and continue
        extracted = frontend.extract(source_bytes)
        delete the old source projection for that file
        upsert File node
        insert symbols rows + Symbol nodes from extracted.symbols
        add Contains edges (File->Symbol, and Symbol->Symbol for nesting)
        add Imports edges from extracted.includes (to_ref = import target; unresolved initially)
  resolver_pass()
```

Symbols to extract from C++ (tree-sitter node types): `function_definition`, method definitions inside `class_specifier`/`struct_specifier`, `class_specifier`, `struct_specifier`, `namespace_definition`, `enum_specifier`. For each: `name`, `qualified_name` (prefix with enclosing namespaces/classes), `signature` (raw parameter text is acceptable), exact start/end line and byte from the tree-sitter node range.

Only the C++ frontend should know these tree-sitter node types. Note: do **not** attempt to populate call/reference edges from tree-sitter — it cannot resolve them on C++. Only `Contains` and `Imports` are produced here.

---

## 9. verify-before-trust (deterministic, on every span read)

```
read a symbol's span:
  recompute current file content_hash
  if == stored files.content_hash:
       return the span bytes read at the stored byte offsets        # fast path
  else:
       refresh source projection internally (scan + language frontend index, or an equivalent single-file refresh)
       find the symbol by qualified_name
       if found:  update its span (offsets + content_hash), set ReResolved flag, return fresh span
       if gone:   return "symbol no longer present", suggest find_symbol
```

Never return cached span content when the file hash has changed. Re-resolve and update before returning; do not ask the caller to re-read. A full scan+index refresh is acceptable for the early implementation. Later incremental work can replace it with a single-file refresh while preserving the same read contract.

---

## 10. MCP tools (stdio JSON-RPC server)

Protocol messages on **stdout only**; all logging to stderr / `.codegraph/logs/mcp.log`.

```
find_symbol(name, kind?)
    -> [ { qualified_name, file, start_line, end_line, symbol_id } ]   # ranked, top few

read_symbol(name | symbol_id, body=true, include_memory=true)
    -> { qualified_name, file, start_line, end_line, hash_status, body?,
         memory: [ attached corrections/decisions with reason ] }      # via §9 verify

read_enclosing_symbol(path, line)
    -> the smallest symbol whose span contains `line`, same shape as read_symbol

read_file_range(path, start_line, end_line)
    -> { path, start_line, end_line, current_hash, text }              # exact bytes

get_memory_for_file(path)
    -> { corrections: [...], decisions: [...] }  each with title, reason/body, provenance

get_memory_for_symbol(name | symbol_id)
    -> same shape, for a symbol

record_correction(title, prefer_paths[], avoid_paths[], affects[], reason)
    -> { node_id }   # appends ADD_CORRECTION, runs materialize
```

`get_memory_for_*` is answered by walking **reverse** `Affects` edges from the File/Symbol node to its memory nodes. Always include provenance (which rule, prefer vs avoid, the reason text).

Design rules for the tool surface: complete spans never chunks; always include `path:start-end`; bundle attached memory into the read response (the caller will not think to ask separately); reads never mutate; accept human-friendly arguments (name + file) and return ids for follow-up.

---

## 11. CLI

```
codegraph init                                   # create .codegraph/, device_id, config
codegraph scan                                   # walk + hash + line tables + git state
codegraph index                                  # tree-sitter symbols/spans/edges (implies scan)
codegraph read-file <path> --start N --end M     # exact range
codegraph find-symbol <name>
codegraph read-symbol <name>
codegraph remember --title T --body B --affects P [--affects P2 ...]   # ADD_DECISION
codegraph correct  --prefer G [--avoid G2 ...] --affects P --reason R   # ADD_CORRECTION
codegraph memory-for <path>
codegraph materialize                            # fold op log -> SQLite (auto after writes)
codegraph doctor                                 # integrity checks (orphan edges, fts drift, etc.)
codegraph bench lookup|memory-for|read           # latency measurement
codegraph mcp                                    # launch the MCP stdio server
```

The CLI is the human surface (setup, authoring memory, maintenance, inspection). The query/read tools listed in §10 are the agent-facing MCP surface; the CLI versions (`find-symbol`, `read-symbol`, `read-file`, `memory-for`) exist for local inspection and testing only.

---

## 12. Build order

Each step has a done-criterion. Do them in order.

0. **Skeleton.** C++20 + CMake; link SQLite (FTS5 on), tree-sitter + tree-sitter-cpp, nlohmann/json, xxHash; shell out to `git`. *Done:* `codegraph --version` runs.
1. **Core types + interner** (§5 subset). *Done:* interner intern/dedup/view tests pass; span packs/unpacks.
2. **Storage** (§4). *Done:* fresh DB created with full schema; reopening is a no-op.
3. **Scanner** (§8 Tier 0: walk, hash, line tables, git, source prune). *Done:* `scan` fills `files`; line table round-trips (offset of line N is correct); deleting a file prunes stale source rows.
4. **Language frontend + Tree-sitter C++** (§8 extraction). *Done:* `index` uses `LanguageFrontend`/`ExtractedFile`, extracts expected C++ symbols from a known file, spans match the source exactly, and `fts_symbols` is populated.
5. **Exact reads + verify-before-trust** (§9, §10 read tools). *Done:* read a symbol; edit the file (move the function); re-read returns the *current* span, flagged ReResolved; deleted symbol reports "gone."
6. **Op log + materializer** (§6, §7: `ADD_CORRECTION`, `ADD_DECISION`, resolution, pending edges). *Done:* `correct`/`remember` append ops; `materialize` builds rows; running it twice makes no duplicates; a correction affecting a not-yet-indexed path lands pending, then resolves after `scan`.
7. **Memory reads** (§10 `get_memory_for_*` via reverse edges). *Done:* after a correction on `resdb/**`, `memory-for resdb/app/x.cc` shows it with reason; `bftsmart/**` shows the avoid rule.
8. **CSR + sorted indexes** (§5). *Done:* `bench` meets §14 targets.
9. **MCP server** (§10 stdio JSON-RPC). *Done:* a JSON-RPC script drives `find_symbol`, `read_symbol`, `get_memory_for_file`, `record_correction` correctly.
10. **doctor + bench + acceptance tests** (§13). *Done:* all §13 tests green.

---

## 13. Acceptance tests

1. `init` + `scan` + `index` on a C++ repo → sane file/symbol counts.
2. `read-file x.cpp --start 1 --end 20` → exact lines, no off-by-one.
3. `find-symbol Foo::bar` → correct file + line range; `read-symbol` → complete body + hash status.
4. Index, then edit the file so the function moves; `read-symbol` of the old symbol → hash mismatch detected → returns the current span flagged ReResolved. Delete the symbol → reports "no longer present."
5. Index, then delete or rename a source file; the next `scan` prunes the old `files`, `line_tables`, `symbols`, source nodes, and source edges, and the next `index` completes without trying to read the missing path.
6. `correct --prefer resdb/** --avoid bftsmart/**` → `memory-for resdb/app/x.cc` shows the prefer rule with its reason; `memory-for bftsmart/y.h` shows the avoid rule.
7. Record a correction that affects a path not yet indexed → its edge is pending → after `scan`, the edge resolves.
8. Run `materialize` twice → no duplicate nodes/edges/memories.
9. Append two separate op streams (simulating two machines) and `materialize` → both memories present, neither overwritten.
10. `bench` meets the targets below.

---

## 14. Performance targets

- symbol lookup < 10 ms
- memory-for-file < 20 ms
- exact span read < 10 ms
- incremental re-index of one changed file < 100 ms (typical file)

---

## 15. Dependencies

C++20, CMake; SQLite with FTS5; tree-sitter + tree-sitter-cpp; nlohmann/json; xxHash; system `git`.

---

## 16. Explicitly out of scope — planned for later, do not build now

Ignore these entirely for this implementation. They are noted only so you don't add hooks for them prematurely:

- Incident memory and prior-incident search (recording bugs with symptom/cause/fix, recurrence counting, similarity matching).
- Session handoffs and one-call session resume.
- Superseding and tombstoning of memory nodes.
- clangd / Pyright semantic enrichment (call graphs, reference edges, types, diagnostics). The `Calls`/`References` relationships are intentionally **not** produced; do not fake them from syntax.
- A Python language frontend (the language frontend interface should be clean enough to add one later, but only C++ is required now).
- Open-ended ranked context retrieval with inclusion/exclusion scoring.
- Cross-machine sync commands and a background file-watching daemon. (The append-only op log and idempotent materializer already make later sync safe — but build no sync commands now.)

Build only what is specified in sections 1–14.
