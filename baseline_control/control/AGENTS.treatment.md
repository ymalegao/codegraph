# Working in this repo (CodeGraph)

CodeGraph is the verification and durable-memory layer under the coding agent.
It is not the repository search or navigation layer.

## Session lifecycle

- At session start, a hook should inject the latest verified handoff. If no
  CodeGraph resume context is present, call `resume_from_handoff` before
  substantial work.
- Treat resumed context as prior-session state, not as a replacement for the
  user's current request. The current request wins when they conflict.
- Before ending a turn after meaningful mutations or durable-memory writes,
  write a handoff if the work would not be obvious to a fresh agent. The stop
  hook enforces this after detected repository changes.

## Discovery and verification

- Use `rg`, compiler output, tests, and normal agentic search for discovery.
- Once a symbol or path matters to an action, verify it with CodeGraph:
  - known symbol: `find_symbol`, then `read_symbol`
  - path and line: `read_enclosing_symbol`
  - exact live range: `read_file_range`
- `find_symbol` and `read_symbol` are verification primitives. Do not use
  CodeGraph as a slower replacement for ordinary repository search.
- A tool error is not an empty result. Report or resolve tool failures; never
  reinterpret a failed lookup as "absent."

## Before editing

- Call `get_memory_for_file` or `get_memory_for_symbol` for the target.
- Honor attached corrections, prefer/avoid rules, and architecture decisions.
- Verify handoff anchors against current code before acting on them.

## Durable memory

- Use `record_correction` for a reusable prefer/avoid rule learned from a real
  failure.
- Use `record_decision` for an architecture choice future work must preserve.
- Do not record transient notes, progress updates, or speculative ideas.

## Handoff contract

Use `write_handoff` for resumable session state. A useful handoff states:

- current objective and user-visible success condition
- completed work and verification already run
- exact next action, blockers, and unresolved questions
- affected files and symbols as `affects` anchors

Keep it concise and operational. Do not use a flat `HANDOFF.md` as the current
session mechanism; the experiment compares that baseline against verified
CodeGraph resume.

## Indexing

Do not run `scan`, `index`, or `materialize` manually during normal work.
Bootstrap and mutation hooks keep the graph refreshed.
