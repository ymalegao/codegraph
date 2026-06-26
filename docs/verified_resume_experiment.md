# Verified Resume — Experiment Protocol

## 1. The claim under test

> When code drifts between fresh agent sessions, a CodeGraph handoff whose anchors
> are re-resolved and re-verified against the live repository prevents stale actions
> and/or reduces recovery cost, compared with an identical handoff served as flat
> text that is not re-verified.

The hypothesis is **verification**, not note quality. Both arms get the same
structured handoff content; only one arm re-resolves it against current code.
That isolation is the whole point — protect it everywhere below.

## 2. The one-variable rule

Exactly one thing may differ between the two arms: **whether the handoff's anchors
are re-resolved/verified at resume.** Everything else is held identical:

- same model and pinned version, same temperature/seed where available;
- same fixture repo and same starting commit;
- same prompts, verbatim;
- same operating discipline (the two `AGENTS.md` files are matched clause-for-clause);
- same handoff *content* (see §6 — the matched-payload control);
- same automatic-refresh affordance (neither arm gets a manual advantage the other lacks).

If you cannot point to a difference and say "that is the verification mechanism,"
it is a confound. Remove it.

## 3. Two repos, matched

- **Baseline repo:** the fixture + `AGENTS.baseline.md` (renamed to `AGENTS.md`).
  No CodeGraph. Continuity via `HANDOFF.md`.
- **Treatment repo:** the same fixture at the same commit + CodeGraph initialized
  (its `AGENTS.md`). Continuity via `resume_from_handoff`. No `HANDOFF.md`.

Both repos start from byte-identical fixture source. The drift commits (§5) are
prepared once and applied identically to both.

## 4. The fixture (build this; do not use a public repo)

Do **not** use a well-known repo (googletest, etc.). Two reasons: (1) it is in the
model's training data, so the agent can reconstruct "where the symbol went" from
memory and mask stale-reference behavior; (2) you cannot author its drift or its
ground truth. Build a purpose-made fixture:

- **Size:** ~30–80 C++ files. A small deterministic service/library.
- **Shape:** one clear subsystem spanning several files (e.g. a request scheduler:
  `scheduler/`, `policy/`, `config/`, `util/`), with a primary function, a
  supporting policy function, and a helper that can later be deleted.
- **Tests:** fast, deterministic, and split into *visible* tests (the agent runs
  these) and *hidden* tests (you score with these; the agent never sees them).
- **Invariant:** the task centers on one named invariant the agent must preserve
  (e.g. "a higher-priority job is never flushed after a lower-priority one").
- **Determinism:** no clocks, no randomness, no network. Same input → same output.
- **Drift-ready:** the primary function, the helper, and a config rule must be
  movable/renamable/deletable without changing externally visible behavior.

Keep the fixture in its own repo with the drift commits on separate branches or as
patch files, so a run is: checkout base → session 1 → apply drift-1 → session 2 →
apply drift-2 → session 3.

## 5. Drift commits (prepared in advance, with a manifest)

Author these once; they are the ground truth for scoring.

- **Drift 1 (move + rename):** move the primary function to another file, rename it
  (behavior identical), update all callers, tests stay green, the old path+name are
  gone.
- **Drift 2 (delete + fold):** delete one previously-relevant helper, fold its
  behavior into a replacement abstraction, move a config rule to a different
  subsystem, tests stay green.

**Drift manifest (`drift_manifest.json`):** for each drift, record the exact
`removed_paths`, `removed_symbols`, `renamed_from→to`, `moved_from→to`. This file
is what the scorer matches against. Stale-action detection is only as good as this
manifest, so make it exhaustive.

## 6. The matched-payload control (the part people get wrong)

In **both** arms, session 1 ends by writing a handoff with the same fields:
objective, success condition, completed work, exact next action, and **named
anchors** (path + symbol). Capture that handoff content.

- Baseline: session 2 reads that content verbatim from `HANDOFF.md`.
- Treatment: session 2 receives `resume_from_handoff`, which takes the *same*
  anchors and re-resolves them against current code, reporting each as
  resolved / re-resolved / gone / unresolved.

Same input, same structure. The only difference is re-resolution. If you let the
treatment arm carry richer content than the baseline, you are measuring formatting,
not verification — and a skeptic will say "I'll just write a better markdown note."

Practical tip: if the agent naturally writes a leaner note in one arm, normalize by
having the harness pass the *same* recorded anchor set into both resume paths.

## 7. Task chain (per run): three sessions, each fresh context

1. **Session 1 — diagnose & begin.** Prompt (verbatim):
   > Diagnose the failing scheduling behavior, identify the invariant being
   > violated, implement the safest first part of the fix, and leave the remaining
   > follow-up for the next session.
   Ends with a handoff. Record ground truth: relevant file+symbol, the invariant,
   the patch made, the exact remaining task.
2. **Apply Drift 1.**
3. **Session 2 — continue & extend.** Prompt (verbatim):
   > Continue the previous work. Add the remaining edge-case behavior and tests,
   > preserving the invariant already identified.
   Relies on prior state. Ends with a second handoff.
4. **Apply Drift 2.**
5. **Session 3 — complete & verify.** Prompt (verbatim):
   > Finish the task from the previous session, resolve any remaining issue, and
   > run the relevant verification.
   Correct behavior: do not act on deleted/renamed anchors, find the current
   replacement, complete the task, pass the hidden tests.

"Fresh session" = new context window, no carryover except the handoff mechanism
under test. So: not three prompts in one chat — three separate agent invocations
with a drift step and a logging step between each.

## 8. Metrics — operationalized

### Primary

**Stale-action rate.** A "stale action" is a logged event that matches the drift
manifest. Detect deterministically from the harness trace; do not eyeball it:

- a file read / `cat` / open whose path ∈ `removed_paths`;
- an `rg`/find for a symbol ∈ `removed_symbols` that is **not** followed (within the
  same session, before any edit) by a search that finds the replacement;
- an edit/patch whose target path ∈ `removed_paths` or whose target symbol ∈
  `removed_symbols`;
- a shell/tool command that fails with a path ∈ `removed_paths`;
- (proxy for "reasoning as if current") the agent's stated next-action names a
  removed symbol/path as the edit target with no verification step logged before it.
  Use this *observable* proxy, not a judgment of the prose, so two scorers agree.

Report two numbers per cell: **fraction of runs with ≥1 stale action**, and **mean
stale actions per run**.

**Task success (deterministic).** All true → success:
hidden tests pass; required behavior present; no unrelated regression (a pre-drift
hidden-test suite still passes); no human rescue prompt was needed mid-session.

### Secondary — and exactly where each number comes from

All of these are computed from the harness trace (§9), not by hand:

- **tokens to first valid action:** sum of prompt+completion tokens from session
  start until the first event that touches a *current* ground-truth file/symbol.
- **time to first valid action:** monotonic elapsed from session start to that same
  event.
- **total tokens / session and / chain:** sum of per-call token counts.
- **tool calls / failed tool calls:** counts of tool events; failed = non-OK status.
- **searches before locating current target:** count of `rg`/find events before the
  first event that hits the current (post-drift) location.
- **handoff size / resume-context size:** byte/token length of the handoff written
  and of the resume payload served.
- **anchor outcomes (treatment only):** counts of resolved / re-resolved / gone /
  unresolved reported by `resume_from_handoff`. (Diagnostic: shows the mechanism
  firing.)
- **user interventions:** count of any manual rescue messages (should be 0 in a
  valid run; a run needing rescue is recorded as a task-success failure).
- **patch quality:** model-judge score (§10), used only for what hidden tests
  cannot capture.

## 9. Harness — what actually runs

The experiment is **not** three prompts; it is a harness that drives three prompts
per session and logs everything. Minimum harness responsibilities:

1. checkout fixture at base commit; install the correct `AGENTS.md`;
2. for each session: start a fresh agent, feed the verbatim prompt, stream and log
   every event;
3. between sessions: apply the next drift commit; run the resume mechanism for the
   next session (read `HANDOFF.md`, or call `resume_from_handoff`);
4. after session 3: run hidden tests; run deterministic scorer against the manifest;
   run the blinded judge.

**Trace format:** one JSONL line per event, with: run id, condition, task, session,
model+version, seed; timestamp + monotonic elapsed; event type
(prompt|completion|tool_call|tool_result|edit|shell|drift_applied|test_result);
token counts when available; tool name, args, result status, latency; repo HEAD
before/after each session; the handoff text + anchors; which drift was applied;
stale-action classification; final test + score. Keep **raw transcripts separate**
from the normalized trace so you can re-score without re-running the model.

## 10. Scoring

- **Deterministic first:** hidden tests; stale path/symbol matches vs the manifest;
  tool/command failures; whether the first edit targets a current ground-truth file.
  These produce the primary metrics with no judgment.
- **Model-judge second, blinded:** only for patch quality hidden tests can't capture.
  Strip condition labels and any tool-name tells from the transcript before judging.

## 11. Repetitions & validity

- ≥10 runs per matrix cell (4 cells: {flat, codegraph} × {no-drift, drift}).
- Multiple *distinct* task chains if you can author them — better than 10 repeats of
  one rename, which over-fits a single drift shape.
- Randomize condition order; reset fixture + agent state between runs.
- Pin model/version + harness config in every trace.
- Run the full matrix in one sitting; if the agent client updates mid-experiment,
  that is a new cohort — do not pool it.

## 12. Pre-registered pass condition (decide before running)

CodeGraph earns the claim only if, **under drift**, it:

- reduces the fraction of runs with a stale action by a margin you set *now*
  (suggested: baseline ≥6/10 runs with a stale action, CodeGraph ≤2/10);
- preserves or improves task success;
- introduces no new practical tool failures;
- has acceptable no-drift overhead (suggested: ≤ ~15% extra tokens/time, no success
  regression).

If a structured-but-unverified flat handoff recovers equally well at comparable
cost, the experiment **does not** validate the machinery. Record that outcome and
believe it.

## 13. Threats to validity — check each before trusting a result

- **Instruction asymmetry:** the two `AGENTS.md` must be matched clause-for-clause.
  The baseline must be told to re-verify references too. If only the treatment is
  told to be careful, the win is the instructions, not the tool. (Biggest risk.)
- **Payload asymmetry:** if treatment carries richer handoff content than baseline,
  you measure formatting. Match the content (§6).
- **Training-data contamination:** a known fixture lets the model recover from memory.
  Use a private synthetic fixture (§4).
- **Manifest incompleteness:** stale actions you forgot to list in the manifest are
  scored as clean. Make the manifest exhaustive; spot-check transcripts against it.
- **Model drift mid-run:** pin the version; one sitting; new version = new cohort.
- **Refresh asymmetry:** if the treatment graph auto-refreshes but the baseline
  agent isn't allowed an equivalent re-check, that's a hidden advantage. Equalize.
- **Scorer subjectivity:** keep primary metrics deterministic; quarantine judgment
  in the blinded patch-quality judge only.
- **Single-drift over-fit:** vary the drift shape across chains so you're not proving
  CodeGraph beats one specific rename.