# CodeGraph — North Star

## The promise

**Nothing CodeGraph hands you is stale.** Every source span, every memory, every handoff anchor is re-verified against the live repository at the moment you ask for it. When the code has moved, CodeGraph catches it and re-resolves — it never serves a remembered fact that is no longer true.

Companion clause: **empty means absent, not missed.** If CodeGraph returns nothing, that is a verified "this does not exist here," not "I failed to find it." A tool whose whole value is trust cannot silently drop things that exist.

Everything in the product is downstream of these two sentences. A feature that does not serve the promise is not part of the product.

## Why this is the whole product (and not a markdown file)

A flat `HANDOFF.md` — write it before `/clear`, read it on resume — already works, and for most of a session CodeGraph and the flat file are indistinguishable. They diverge at exactly one moment: **when the code moves.** The agent records "the relevant symbol is `BuildIntersectionSchedule`," does an hour of work, and now that note points at a function that has shifted, been renamed, or been deleted. The flat file has no way to know it is now lying. CodeGraph does: it re-resolves the anchor and re-verifies the span.

That single moment — a remembered reference going stale the instant work continues — is the entire justification for the graph machinery. If CodeGraph does not win there, it is overhead.

## The one earned use case

CodeGraph was built from two failures actually lived, not from a survey of what other systems do:

1. A handoff that went stale the moment work resumed (→ verified resume).
2. Stale architecture context bleeding into the wrong subsystem (→ decisions and corrections anchored to code).

These earned their place. They are the spine. Everything else must earn its place the same way.

## The discipline (how anything new gets in)

> Build a memory feature when a real session fails for lack of it. Never because a respected system (ADR, Letta, LangGraph) has it.

`resume_from_handoff` earned itself by a failure you hit. Memory lifecycle fields — `status`, `supersedes`, `scope`, `actor` — have not. They wait in a "later, if a session demands it" pile until the day a superseded decision actually causes a wrong edit. The north star is not designed in advance; it is named by failures, exactly as resume was.

## What `find_symbol` is actually for

Not navigation. As a way to *find* code it competes with `grep`/`rg`, loses, and loses by more as base models get better at agentic search — vector RAG over code was already abandoned by the major labs for this reason. Do not rebuild that.

`find_symbol` / `read_symbol` exist as the **verification primitive**: when a handoff is anchored to a symbol, they re-resolve and re-verify that anchor against current code instead of trusting a stored line number. They keep the promise. They are not a search feature, and they should not be sold or measured as one.

## Non-goals

- Not a coding agent (Claude Code / Codex / Cursor do that; CodeGraph sits under them).
- Not a navigation tool — `grep` wins; do not compete there.
- Not vector RAG / semantic chunk search — proven to lose to agentic grep; do not revert to it.
- Not a docs platform — one generated handoff is on-thesis; a suite of generated docs is not the product.
- Not tamper-evident / cryptographic — there is no adversary; content hashes already do the only verification that matters (staleness). Revisit only if CodeGraph ever becomes shared/multi-writer.

## The gate

No feature ships claiming to beat the baseline until it beats the baseline.

- **Baseline:** flat `HANDOFF.md`, written before `/clear`, read on resume.
- **Test:** a real task where the code *drifts between sessions* — the things the handoff referenced get moved, renamed, or deleted.
- **Pass condition:** under drift, verified resume avoids the stale action the flat handoff walks into.

If it ties without drift and wins under drift, the product is proven. If it does not win even under drift, that is the most valuable thing you could learn, and the flat file was the answer.

## Priority, always

Defend the trust contract before adding anything. A resolved memory that becomes invisible (a duplicate-node lookup miss), or a stale span served as current, is not a normal bug — it breaks the one promise the product makes. Those get fixed before any feature.