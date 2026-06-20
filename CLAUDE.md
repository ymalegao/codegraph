# Working in this repo (CodeGraph)

A CodeGraph MCP server gives you exact, current, memory-annotated views of the code.
You do not manage indexing; the repo reindexes itself. Use the tools.

## Read with the tools, not raw file reads

**Never use `grep`, `cat`, `head`, or `tail` on source files. A CodeGraph tool always answers faster with less token waste.**

- `read_symbol` and `read_enclosing_symbol` return the exact current span, verified against disk, plus attached corrections and decisions.
- Spans are current. If a file changed, the tool re-resolves and sets `hash_status=ReResolved`.
- Do not re-read just in case the file changed.

## Discovery to retrieval

Follow this order — stop at the first tool that answers the question:

1. Path and line from an error, stack trace, or diff → `read_enclosing_symbol(path, line)`
2. Known name or prefix → `find_symbol` → `read_symbol`
3. Keywords or intent without a name → `search_symbol` → `read_symbol` on the best candidate
4. Free text only, CodeGraph tools returned nothing → `rg` (not `grep`), then `read_symbol` or `search_symbol` on the hit

`rg` is a last resort, not a first instinct. If you find yourself reaching for `grep` or `cat`, stop and use step 1–3 first.

## Before changing a symbol or file

- Use `get_memory_for_file` or `get_memory_for_symbol` first.
- Honor prefer and avoid rules and architecture decisions before editing.

## Recording durable memory

- Use `record_correction` for prefer/avoid rules with a reason.
- Use `record_decision` for durable architecture rules.
- Do not record transient notes.

## Indexing

Never run `scan`, `index`, or `materialize` manually. Edits are reindexed automatically.
