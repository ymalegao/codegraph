# CodeGraph — Postmortem

Written at wind-down, while the findings are sharp. The purpose of this document
is to stop future-me from rebuilding this under a new name. If the idea comes
back dressed in new clothes, read this first.

## What this was

A local-first, C++-first memory and verification layer meant to sit under coding
agents (Claude Code, Codex, Cursor): exact verified source spans, durable
code-anchored memory (decisions, corrections, handoffs), graph-backed session
resume, merge-safe op-log sync. Built in C++20: scanner + tree-sitter, SQLite+FTS5,
in-memory CSR, append-only op log, materializer with pending-edge resolution, MCP
server. The engine worked. The thesis did not survive contact with a competent agent.

## The thesis

"Never rediscover context, never overwrite context." More precisely: nothing the
tool hands you is stale, because anchors are re-resolved/verified against the live
repo on every read — and that verification is worth more than what an agent can do
on its own.

## What the experiment actually showed

Ran the pilot: two drift chains (a clean rename/move, then a delete/fold), three
fresh agent sessions each, baseline (flat HANDOFF.md + a discipline-matched
AGENTS.md) vs treatment (CodeGraph verified resume). Both arms passed all hidden
tests. Both recovered from the drift. The baseline noticed the stale path
("src/config/config_rules.cpp does not exist"), searched, and re-resolved it
itself in well under two minutes. The treatment did the same thing the tool was
supposed to make unnecessary — and spent MORE tool calls and tokens to reach the
identical edit (multiple find_symbol / read_symbol / get_memory_for_file round
trips, several returning empty, vs the baseline reasoning straight to the change).

Verdict: **on ordinary drift, against a competent agent that has been told to
verify references, CodeGraph's auto-re-resolution does not earn its keep. grep +
instruction already closes the gap, and querying empty memory is a net token tax.**

The critical methodological point: the baseline only looked smart because I gave it
a fair AGENTS.md ("re-check references before editing"). That was the correct,
honest thing to do — and the moment the baseline had the same discipline, the
flat-file arm stopped being dumb and the tool's advantage evaporated. A rigged
benchmark (careful treatment, naive baseline) would have shown a fake win.

## The three pillars, judged

1. **Retrieval / re-resolution** (search symbol, get span, re-resolve moved anchors):
   REFUTED for ordinary drift. Competes with grep; grep wins and keeps winning as
   base models improve. This was always the weakest pillar and the pilot confirmed it.

2. **Handoff / session continuity**: WORKS but is MATCHED by a flat HANDOFF.md that
   gets reliably written. The genuinely useful experience I had — picking up context
   in a new chat or on another machine — is delivered by a disciplined handoff with
   no graph machinery underneath. The machinery added nothing the file lacked.

3. **Correction / decision memory** (knowledge that is NOT in the code): UNTESTED,
   and the only pillar reality has not killed. But see the discriminator below — most
   candidate "decisions" fail it.

## The discriminator (the portable lesson)

> If a competent agent could rediscover it by reading the repo, do not store it.

This killed pillar 1 (a moved symbol is rediscoverable by grep) and it kills most of
pillar 3 too. In the fixture, both agents independently rederived the
"priority > 0 = interactive" convention from the code — so storing that convention as
a "decision" would also have been redundant. The ONLY memory worth storing is the
non-rediscoverable kind: a forbidden-but-plausible path, a decision that contradicts
what the code suggests, the reason a non-obvious workaround exists, a correction I
already tried that failed. That knowledge is not in the code, so an agent cannot
search its way to it — and, crucially, a generator cannot produce it either.

## Why "deterministic document generation" is not the escape hatch

Considered pivoting to generated living docs (ARCHITECTURE.md / DECISIONS.md). It
fails on its own terms: a generator can only emit what is in its source. Generate
from code and you produce exactly the rediscoverable knowledge that is worthless to
store; you cannot generate the non-rediscoverable knowledge because it is not in any
source to generate from. A generated architecture doc is a strictly worse version of
the agent reading the repo. The one technique that actually works for context files
is human curation kept ruthlessly small and pruned every sprint (per practitioners) —
which is a discipline, not a tool, and at solo scale needs no product.

## Why the "living knowledge base" companies are not a counterexample

Mintlify / Fern / GitBook / ReadMe are documentation *publishing* platforms with AI
authoring, AI search, and staleness *nudges* bolted on. In every case a human stays
in the authoring loop — the AI drafts and flags; a person decides. They fight
staleness by making human curation cheaper and wiring it into the PR flow
(docs-as-code, CI doc checks, "last reviewed" alerts, AI-drafted updates from diffs),
NOT by self-maintaining docs — because that is the same rediscoverability wall and
they are on the wrong side of it too. Their value is team-scale collaboration and
publishing discipline. A solo agent-memory tool has none of that and cannot
manufacture it. So they are not evidence the direction works for one developer; they
are evidence that the only part that works keeps a human in the loop.

## What was actually gained

- Hands-on C++20 systems practice: op-logs, materializers, CSR, string interning,
  pending-edge resolution, an MCP server. Real and transferable.
- A tested instinct for killing a direction cheaply with one honest pilot instead of
  building for months on a demo.
- The discriminator above, which scopes future tools in minutes.

## If this idea returns

Do not reach backward to justify the CSR/materializer/tree-sitter asset. Start from a
problem verified to be one a competent agent CANNOT already solve by reading the repo,
then build the least code that solves it (probably not this code). The one open
question worth a future look: can an agent reuse *non-rediscoverable* experiential
knowledge (a past correction/failure) on a NEW task, and does anything beat a human
keeping a 20-line curated file? Everything else here is settled.