# Working in this repo (CodeGraph)

CodeGraph exposes exact, current, memory-annotated code reads through its MCP server.
Do not manage indexing by hand; the repo bootstraps and refreshes itself.

## Prefer CodeGraph Reads

- Use `read_symbol` or `read_enclosing_symbol` before raw file reads when working on a symbol.
- These reads verify spans against disk and report `hash_status=ReResolved` if the file moved.
- Use attached corrections and decisions as project memory before editing.

## Discovery to Retrieval

- Path and line from an error, stack trace, or diff: `read_enclosing_symbol(path, line)`.
- Known symbol name: `find_symbol`, then `read_symbol`.
- Keywords or intent without a name: `search_symbol`, then `read_symbol` on the chosen candidate.
- Free text or non-symbol search: use `rg`, then resolve the hit through CodeGraph tools.

## Before Editing

- Call `get_memory_for_file` or `get_memory_for_symbol`.
- Honor prefer/avoid rules and architecture decisions.

## Durable Memory

- Use `record_correction` for prefer/avoid rules with a reason.
- Use `record_decision` for durable architecture choices.
- Do not record transient notes.

## Indexing

Do not run `scan`, `index`, or `materialize` manually during normal work. The MCP server self-bootstraps and refreshes its graph when the SQLite cache changes.
