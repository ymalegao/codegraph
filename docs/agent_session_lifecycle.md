# Agent Session Lifecycle Design

## Objective

Make verified resume and useful handoffs the default behavior in Codex and
Claude Code without asking the user to remember lifecycle commands.

This integration exists to support the north-star experiment:

- baseline: a flat `HANDOFF.md`
- treatment: a CodeGraph handoff with live, re-resolved anchors
- differentiating condition: referenced code moves, changes name, or disappears

It is not intended to make CodeGraph the primary code-search interface.

## Lifecycle

### Session start

The `SessionStart` hook runs `codegraph resume-context`.

The command:

1. bootstraps or incrementally refreshes the repository graph;
2. loads the latest handoff;
3. resolves its affected nodes against the current repository;
4. reports unresolved anchors explicitly;
5. injects the result as developer context before the first user prompt.

The agent treats this as prior-session state. A new user request can replace it.

### During work

Normal search remains normal search (`rg`, diagnostics, tests). CodeGraph is
used when a discovered path or symbol becomes an action target:

- verify the current symbol body or enclosing symbol;
- load attached corrections and decisions before editing;
- distinguish an absent result from a failed tool call.

Mutation hooks refresh the graph after the worktree changes.

### Turn stop

The shared hook stores a fingerprint of tracked changes and untracked files at
session start or immediately after a successful handoff.

After mutation-capable tools run, it compares the current fingerprint:

- unchanged: no lifecycle action;
- changed: mark the session dirty and refresh the graph;
- `write_handoff`: mark the current state clean.

When the agent tries to stop while dirty, the `Stop` hook continues the agent
once with an instruction to write a concise operational handoff. The agent,
not the hook script, performs the semantic summarization.

This makes the handoff current before a later `/clear` or new chat without
forcing a handoff after read-only questions.

### Compaction

Manual compaction is blocked while dirty and asks for a handoff first.
Automatic compaction is not blocked because doing so near the context limit can
strand the session. Normal end-of-turn enforcement should leave the session
clean before most compactions.

## Handoff contents

A handoff should contain:

- current objective and success condition;
- completed work and verification;
- next concrete action;
- blockers or unresolved questions;
- affected file and symbol anchors.

It should not contain a transcript, generic progress narration, or speculative
future features.

## Constraints

- Hooks cannot guarantee a semantic handoff if the process is killed or the
  user clears an actively running turn before the stop hook executes.
- Codex currently runs command hooks only. The stop hook therefore asks the
  existing agent to write the handoff instead of launching a second model.
- Hook trust is client-controlled. Project Codex hooks must be reviewed after
  installation or modification.
- Worktree fingerprinting detects repository mutations, not important
  read-only discoveries. Agent instructions still require a handoff for
  resumable diagnosis even when no file changed.

## Experiment implications

The lifecycle must be installed for the treatment condition but disabled for
the flat-file baseline. The experiment harness must record:

- whether resume occurred before the first task action;
- whether the previous handoff was written before session termination;
- resolved, re-resolved, gone, and unresolved anchors;
- stale actions against old paths or symbols;
- task/test success, tool failures, latency, and token use.
