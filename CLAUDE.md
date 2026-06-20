# Working in this repo (CodeGraph)

A CodeGraph MCP server gives you exact, current, memory-annotated views of the code.
You do not manage indexing; the repo reindexes itself. Use the tools.

## Read with the tools, not raw file reads

- `read_symbol` and `read_enclosing_symbol` return the exact current span, verified against disk, plus attached corrections and decisions. Prefer them over opening files raw.
- Spans are current. If a file changed, the tool re-resolves and sets `hash_status=ReResolved`.
- Do not re-read just in case the file changed.

## Discovery to retrieval

- Path and line from an error, stack trace, or diff: use `read_enclosing_symbol(path, line)`.
- Known name or prefix: use `find_symbol`, then `read_symbol`.
- Keywords or intent without a name: use `search_symbol`, then `read_symbol` on the best candidate.
- Free text, or unsure whether the target is a symbol: use `rg`, then `read_symbol` or `search_symbol` on the hit.

## Before changing a symbol or file

- Use `get_memory_for_file` or `get_memory_for_symbol` first.
- Honor prefer and avoid rules and architecture decisions before editing.

## Recording durable memory

- Use `record_correction` for prefer/avoid rules with a reason.
- Use `record_decision` for durable architecture rules.
- Do not record transient notes.

## Indexing

Never run `scan`, `index`, or `materialize` manually. Edits are reindexed automatically.
